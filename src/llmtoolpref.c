/*
 * llmtoolpref.c : préfs des outils natifs par profil (llm.json).
 * Voir llmtoolpref.h pour le schéma. Les écritures suivent la même
 * mécanique que la config retry : recharger l'arbre, muter, réécrire —
 * préservation stricte des autres membres (providers, harness, active).
 */

#include "llm.h"
#include "i18n.h"
#include <json-glib/json-glib.h>
#include <string.h>

const char *const LLM_PROFILE_NAMES[LLM_PROFILE_COUNT] = {
    "MINIMAL", "DEFAULT", "YOLO"
};
/* Libellés affichables, séparés des noms techniques ci-dessus : traduire
 * LLM_PROFILE_NAMES changerait la CLE relue dans llm.json au démarrage,
 * traduire ceux-ci ne change que l'écran. */
const char *const LLM_PROFILE_LABELS[LLM_PROFILE_COUNT] = {
    /* TRANSLATORS: name of the strictest tool profile (everything is either
       refused or asked about). The word is identical in French: leave the
       msgstr empty. */
    N_("MINIMAL"),
    /* TRANSLATORS: name of the default tool profile (asks before running).
       The word is identical in French: leave the msgstr empty. */
    N_("DEFAULT"),
    /* TRANSLATORS: name of the most permissive tool profile (runs at once).
       A universal acronym: leave the msgstr empty. */
    N_("YOLO")
};
const char *const LLM_TOOL_MODE_NAMES[4] = {
    "off", "ask", "allow", "allowplus"
};

/* Défaults par profil pour un outil neuf : MINIMAL silencieux,
 * DEFAULT = le comportement historique (ASK), YOLO = exécution directe. */
static void
tool_pref_apply_defaults(LlmToolPref *p)
{
    p->modes[LLM_PROFILE_MINIMAL] = LLM_TOOL_OFF;
    /* read est en lecture seule (sans effet destructeur) : il est
     * annonce en ALLOW des le profil DEFAULT. Les autres outils restent
     * en ASK (decision d'Eric avant execution). */
    if (p->name != NULL && g_strcmp0(p->name, "read") == 0)
        p->modes[LLM_PROFILE_DEFAULT] = LLM_TOOL_ALLOW;
    else
        p->modes[LLM_PROFILE_DEFAULT] = LLM_TOOL_ASK;
    p->modes[LLM_PROFILE_YOLO]    = LLM_TOOL_ALLOW;
}

static LlmToolMode
mode_from_wire(const char *s, LlmToolMode fallback)
{
    if (s == NULL)
        return fallback;
    for (int i = 0; i < 4; i++)
        if (g_strcmp0(s, LLM_TOOL_MODE_NAMES[i]) == 0)
            return (LlmToolMode)i;
    return fallback;
}

const char *
llm_profile_name(LlmToolProfile p)
{
    return (p >= 0 && p < LLM_PROFILE_COUNT)
               ? LLM_PROFILE_NAMES[p] : "DEFAULT";
}

const char *
llm_profile_label(LlmToolProfile p)
{
    return (p >= 0 && p < LLM_PROFILE_COUNT)
               ? _(LLM_PROFILE_LABELS[p]) : _("DEFAULT");
}

const char *
llm_tool_mode_name(LlmToolMode m)
{
    return (m >= 0 && m <= LLM_TOOL_ALLOWPLUS)
               ? LLM_TOOL_MODE_NAMES[m] : "off";
}
LlmToolMode
llm_tool_pref_mode(const LlmToolPref *pref, LlmToolProfile profile)
{
    if (pref == NULL || profile < 0 || profile >= LLM_PROFILE_COUNT)
        return LLM_TOOL_OFF;
    return pref->modes[profile];
}

const LlmToolPref *
llm_tools_pref_find(GPtrArray *prefs, const char *name)
{
    if (prefs == NULL || name == NULL)
        return NULL;
    for (guint i = 0; i < prefs->len; i++) {
        const LlmToolPref *p = g_ptr_array_index(prefs, i);

        if (p != NULL && g_strcmp0(p->name, name) == 0)
            return p;
    }
    return NULL;
}

/* Migration des noms d'outils. Le prefixe cdb_ est tombe et remove est ne.
 *
 * Sans cette table, un llm.json enregistre avant le changement ne retrouve
 * AUCUNE de ses lignes : cherchee sous "read", trouvee sous "cdb_read",
 * chaque outil retombe silencieusement sur ses defauts. L'utilisateur
 * perd ses reglages sans le moindre message, et le crois ensuite a un bug
 * des modes. On reecrit le nom a la LECTURE : la prochaine sauvegarde
 * ecrira le nom neuf, et un llm.json deja neuf traverse sans rien changer.
 *
 * Les noms a gauche sont les SEULS endroits du projet ou "cdb_read" & co
 * survivent volontairement. */
static const char *
tool_name_migrate(const char *legacy)
{
    static const struct { const char *from, *to; } map[] = {
        { "cdb_bash",    "bash"    },
        { "cdb_read",    "read"    },
        { "cdb_insert",  "insert"  },
        { "cdb_replace", "replace" },
        { "cdb_create",  "create"  },
        { "cdb_delete",  "delete"  },
    };

    if (legacy == NULL)
        return NULL;
    for (guint i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_strcmp0(legacy, map[i].from) == 0)
            return map[i].to;
    return legacy;
}

/* Lecture llm.json "tools". Outil inconnu (jamais vu) : absent de la
 * liste. bash est TOUJOURS présent (défauts appliqués si manquant) :
 * c'est l'outil natif historique du dashboard. */
GPtrArray *
llm_tools_prefs_load(void)
{
    GPtrArray  *out = g_ptr_array_new();
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    gboolean    have_bash = FALSE;
    gboolean    have_read = FALSE;
    gboolean    have_insert = FALSE;
    gboolean    have_remove = FALSE;
    gboolean    have_replace = FALSE;
    gboolean    have_create = FALSE;
    gboolean    have_delete = FALSE;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *root =
            json_node_get_object(json_parser_get_root(parser));

        if (root != NULL && json_object_has_member(root, "tools")) {
            JsonArray *arr = json_object_get_array_member(root, "tools");

            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject  *o = json_array_get_object_element(arr, i);
                LlmToolPref *p;

                if (o == NULL || !json_object_has_member(o, "name"))
                    continue;
                p = g_new0(LlmToolPref, 1);
                p->name = g_strdup(tool_name_migrate(
                    json_object_get_string_member(o, "name")));
                tool_pref_apply_defaults(p);

                /* Compat : ancien format {enabled:bool} -> tout ASK/OFF. */
                if (json_object_has_member(o, "modes")) {
                    JsonArray *ma = json_object_get_array_member(o,
                                                                 "modes");
                    for (guint k = 0;
                         k < json_array_get_length(ma) &&
                         k < LLM_PROFILE_COUNT; k++) {
                        JsonNode *mn = json_array_get_element(ma, k);
                        const char *sv =
                            (mn != NULL && JSON_NODE_HOLDS_VALUE(mn) &&
                             json_node_get_value_type(mn) == G_TYPE_STRING)
                                ? json_node_get_string(mn) : NULL;

                        p->modes[k] = mode_from_wire(sv, p->modes[k]);
                    }
                } else if (json_object_has_member(o, "enabled")) {
                    gboolean en = json_object_get_boolean_member(o,
                                                                 "enabled");

                    for (guint k = 0; k < LLM_PROFILE_COUNT; k++)
                        p->modes[k] = en ? LLM_TOOL_ASK : LLM_TOOL_OFF;
                }

                if (g_strcmp0(p->name, "bash") == 0)
                    have_bash = TRUE;
                if (g_strcmp0(p->name, "read") == 0)
                    have_read = TRUE;
                if (g_strcmp0(p->name, "insert") == 0)
                    have_insert = TRUE;
                if (g_strcmp0(p->name, "remove") == 0)
                    have_remove = TRUE;
                if (g_strcmp0(p->name, "replace") == 0)
                    have_replace = TRUE;
                if (g_strcmp0(p->name, "create") == 0)
                    have_create = TRUE;
                if (g_strcmp0(p->name, "delete") == 0)
                    have_delete = TRUE;
                g_ptr_array_add(out, p);
            }
        }
    }
    g_object_unref(parser);
    g_free(path);

    if (!have_bash) {
        LlmToolPref *p = g_new0(LlmToolPref, 1);

        p->name = g_strdup("bash");
        tool_pref_apply_defaults(p);
        g_ptr_array_add(out, p);
    }
    if (!have_read) {
        LlmToolPref *p = g_new0(LlmToolPref, 1);

        p->name = g_strdup("read");
        tool_pref_apply_defaults(p);
        g_ptr_array_add(out, p);
    }
    /* Sans ce bloc, un outil jamais vu retombe dans llm_tool_pref_mode
     * sur NULL -> OFF : il ne serait jamais annonce au modele. */
    if (!have_insert) {
        LlmToolPref *p = g_new0(LlmToolPref, 1);

        p->name = g_strdup("insert");
        tool_pref_apply_defaults(p);
        g_ptr_array_add(out, p);
    }
    /* Miroir destructif de insert : meme place dans la liste, meme seed.
     * Sans ces six lignes, remove ne serait jamais annonce au modele. */
    if (!have_remove) {
        LlmToolPref *p = g_new0(LlmToolPref, 1);

        p->name = g_strdup("remove");
        tool_pref_apply_defaults(p);
        g_ptr_array_add(out, p);
    }
    if (!have_replace) {
        LlmToolPref *p = g_new0(LlmToolPref, 1);

        p->name = g_strdup("replace");
        tool_pref_apply_defaults(p);
        g_ptr_array_add(out, p);
    }
    /* Outils neufs : sans seed ici, llm_tools_pref_find renvoie NULL et
     * llm_tool_pref_mode rend OFF -> l'outil ne serait jamais annonce. */
    if (!have_create) {
        LlmToolPref *p = g_new0(LlmToolPref, 1);

        p->name = g_strdup("create");
        tool_pref_apply_defaults(p);
        g_ptr_array_add(out, p);
    }
    if (!have_delete) {
        LlmToolPref *p = g_new0(LlmToolPref, 1);

        p->name = g_strdup("delete");
        tool_pref_apply_defaults(p);
        g_ptr_array_add(out, p);
    }
    return out;
}

void
llm_tools_prefs_free(GPtrArray *prefs)
{
    if (prefs == NULL)
        return;
    for (guint i = 0; i < prefs->len; i++) {
        LlmToolPref *p = g_ptr_array_index(prefs, i);

        g_free(p->name);
        g_free(p);
    }
    g_ptr_array_unref(prefs);
}

/* Réécrit l'entrée complète d'un outil (tous ses modes) dans la liste. */
static void
tool_entry_to_array(JsonArray *arr, const LlmToolPref *p)
{
    JsonObject *o = json_object_new();
    JsonArray  *ma = json_array_new();

    json_object_set_string_member(o, "name", p->name);
    for (guint k = 0; k < LLM_PROFILE_COUNT; k++)
        json_array_add_string_element(ma,
                                      llm_tool_mode_name(p->modes[k]));
    json_object_set_array_member(o, "modes", ma);
    json_array_add_object_element(arr, o);
}

/* Écrit un mode pour (outil, profil) : on charge les préfs courantes
 * (défauts inclus), on mute la cible, puis on réécrit la liste EN ENTIER.
 * Simple et sans dérive : la liste persistée reflète toujours l'état
 * complet des trois modes de chaque outil. */
void
llm_config_save_tool_mode(const char *name, LlmToolProfile profile,
                          LlmToolMode mode)
{
    GPtrArray  *prefs;
    LlmToolPref *target;
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root;
    JsonNode   *work = NULL;

    if (name == NULL || profile < 0 || profile >= LLM_PROFILE_COUNT)
        return;

    prefs = llm_tools_prefs_load();
    /* La liste chargée peut ne PAS contenir l'outil (nom neuf) : on
     * l'ajoute avec ses défauts avant de muter. */
    target = (LlmToolPref *)llm_tools_pref_find(prefs, name);
    if (target == NULL) {
        target = g_new0(LlmToolPref, 1);
        target->name = g_strdup(name);
        tool_pref_apply_defaults(target);
        g_ptr_array_add(prefs, target);
    }
    target->modes[profile] = mode;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        work = json_node_copy(json_parser_get_root(parser));
        root = json_node_get_object(work);
    } else {
        root = json_object_new();
        work = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(work, root);
    }

    {
        JsonArray *arr = json_array_new();

        for (guint i = 0; i < prefs->len; i++)
            tool_entry_to_array(arr, g_ptr_array_index(prefs, i));
        json_object_set_array_member(root, "tools", arr);
    }

    {
        JsonGenerator *gen = json_generator_new();
        gchar         *text = json_to_string(work, TRUE);
        GError        *error = NULL;

        json_generator_set_root(gen, work);
        if (text != NULL &&
            !g_file_set_contents(path, text, -1, &error)) {
            if (error != NULL) {
                g_printerr(_("CDB: failed to write tool prefs: %s\n"),
                           error->message);
                g_error_free(error);
            }
        }
        g_free(text);
        g_object_unref(gen);
    }

    json_node_unref(work);
    g_object_unref(parser);
    g_free(path);
    llm_tools_prefs_free(prefs);
}

/* Profil actif : lu dans llm.json active.profile, défaut DEFAULT. */
LlmToolProfile
llm_config_active_profile(void)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    LlmToolProfile prof = LLM_PROFILE_DEFAULT;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *root =
            json_node_get_object(json_parser_get_root(parser));

        if (root != NULL && json_object_has_member(root, "active")) {
            JsonObject *a = json_object_get_object_member(root, "active");

            if (a != NULL && json_object_has_member(a, "profile")) {
                const char *s =
                    json_object_get_string_member(a, "profile");

                for (guint i = 0; i < LLM_PROFILE_COUNT; i++)
                    if (g_strcmp0(s, LLM_PROFILE_NAMES[i]) == 0) {
                        prof = (LlmToolProfile)i;
                        break;
                    }
            }
        }
    }
    g_object_unref(parser);
    g_free(path);
    return prof;
}

void
llm_config_set_active_profile(LlmToolProfile profile)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root, *active;
    JsonNode   *work = NULL;

    if (profile < 0 || profile >= LLM_PROFILE_COUNT)
        return;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        work = json_node_copy(json_parser_get_root(parser));
        root = json_node_get_object(work);
    } else {
        root = json_object_new();
        work = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(work, root);
    }

    /* active peut être absent (config jamais ouverte) ou NULL : on le
     * crée. S'il existe, on n'y touche PAS sauf le champ profile. */
    if (!json_object_has_member(root, "active") ||
        json_object_get_object_member(root, "active") == NULL)
        json_object_set_object_member(root, "active", json_object_new());
    active = json_object_get_object_member(root, "active");
    json_object_set_string_member(active, "profile",
                                  llm_profile_name(profile));

    {
        JsonGenerator *gen = json_generator_new();
        gchar         *text = json_to_string(work, TRUE);
        GError        *error = NULL;

        json_generator_set_root(gen, work);
        if (text != NULL && !g_file_set_contents(path, text, -1, &error)) {
            if (error != NULL) {
                g_printerr(_("CDB: failed to write active profile: %s\n"),
                           error->message);
                g_error_free(error);
            }
        }
        g_free(text);
        g_object_unref(gen);
    }

    json_node_unref(work);
    g_object_unref(parser);
    g_free(path);
}

LlmToolMode
llm_tools_effective_mode(const char *name)
{
    GPtrArray        *prefs = llm_tools_prefs_load();
    const LlmToolPref *p = llm_tools_pref_find(prefs, name);
    LlmToolMode       mode = llm_tool_pref_mode(p,
                                                llm_config_active_profile());

    llm_tools_prefs_free(prefs);
    return mode;
}
