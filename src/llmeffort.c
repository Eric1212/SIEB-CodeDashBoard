/* llmeffort.c — effort de raisonnement : tables, persistance, wire.
 *
 * L'effort (LlmEffort, llm.h) est une préférence GLOBALE de session :
 * elle vit dans llm.json active.effort, exactement comme active.profile
 * vit dans active.profile (llmtoolpref.c). Trois pièces, toutes calquées
 * sur le patron profil :
 *
 *   1. tables NAMES (clés persistées, jamais traduites) / LABELS
 *      (affichables, N_() à la définition, _() à l'usage) gardées par
 *      static_assert sur LLM_EFFORT_COUNT — un niveau ajouté à l'enum
 *      tombe dans les tables au compilateur, pas au silence ;
 *
 *   2. get/set persistant : mutation du SEUL champ active.effort — le
 *      reste du bloc active (provider, model, profile) n'est pas
 *      touché, une reconstruction à zéro l'effacerait en silence
 *      (même discipline que llm_config_set_active_profile) ;
 *
 *   3. le mot wire : champ plat OpenAI « reasoning_effort » — forme
 *      vérifiée sur le terrain chez HyperCharm (glm-5.3-flash : none
 *      respecté à 0 jeton, max appliqué ; qwen3.8-flash : champ avalé
 *      sans effet) et OpenRouter (glm-5.2:free : none → 0 jeton,
 *      xhigh → 238 jetons). DEFAULT renvoie NULL : l'appelant n'écrit
 *      alors AUCUN champ, le fournisseur applique le défaut du modèle —
 *      l'état d'origine de CDB avant cette fonctionnalité.
 *
 * La vérification du respect est du ressort de l'observation, pas
 * d'ici : le chunk usage déjà reçu porte completion_tokens_details.
 * reasoning_tokens, et les deltas reasoning s'affichent déjà dans le
 * fil. Aucune table de découverte n'est lue — les tables déclaratives
 * des fournisseurs sous-estiment le réel (glm-5.2 déclare [xhigh,high]
 * sans none et respecte pourtant none à la lettre). */
#include "llm.h"
#include "i18n.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* L'enum et ses tables ne peuvent pas diverger : un decalage d'un seul
 * rang lirait "max" la ou Eric a pose "none" — un choix de cout lu a
 * l'envers, sans le moindre message. La borne est verifiee par le
 * compilateur, pas par une relecture. (C23 : static_assert est un
 * mot-cle, pas une macro.) */
static_assert(G_N_ELEMENTS(LLM_EFFORT_NAMES) == LLM_EFFORT_COUNT,
              "LLM_EFFORT_NAMES doit avoir exactement une entree par niveau");
static_assert(G_N_ELEMENTS(LLM_EFFORT_LABELS) == LLM_EFFORT_COUNT,
              "LLM_EFFORT_LABELS doit avoir exactement une entree par niveau");
/* Clés persistées : comparer en code, ne jamais traduire — traduire
 * changerait la valeur relue au démarrage (loi MINIMAL/DEFAULT/YOLO). */
const char *const LLM_EFFORT_NAMES[LLM_EFFORT_COUNT] = {
    "DEFAULT", "NONE", "MINIMAL", "LOW", "MEDIUM", "HIGH", "XHIGH", "MAX",
};

const char *const LLM_EFFORT_LABELS[LLM_EFFORT_COUNT] = {
    /* TRANSLATORS: effort levels of reasoning models. "Default" means
     * CDB sends no field at all and the provider applies the model's
     * own default. N_() at definition, _() at use — tables statiques. */
    N_("Default"),
    N_("None"),
    N_("Minimal"),
    N_("Low"),
    N_("Medium"),
    N_("High"),
    N_("X-High"),
    N_("Max"),
};

const char *
llm_effort_label(LlmEffort e)
{
    return (e >= 0 && e < LLM_EFFORT_COUNT)
               ? _(LLM_EFFORT_LABELS[e]) : _("Default");
}

gboolean
llm_effort_is_default(LlmEffort e)
{
    return e == LLM_EFFORT_DEFAULT;
}

const char *
llm_effort_wire(LlmEffort e)
{
    switch (e) {
    case LLM_EFFORT_NONE:    return "none";
    case LLM_EFFORT_MINIMAL: return "minimal";
    case LLM_EFFORT_LOW:     return "low";
    case LLM_EFFORT_MEDIUM:  return "medium";
    case LLM_EFFORT_HIGH:    return "high";
    case LLM_EFFORT_XHIGH:   return "xhigh";
    case LLM_EFFORT_MAX:     return "max";
    case LLM_EFFORT_DEFAULT:
    default:                 return NULL;   /* n'écrit AUCUN champ */
    }
}

/* effortName(e) : le nom-clé d'un niveau, borne vérifiée. */
static const char *
effort_name(LlmEffort e)
{
    return (e >= 0 && e < LLM_EFFORT_COUNT) ? LLM_EFFORT_NAMES[e]
                                            : LLM_EFFORT_NAMES[LLM_EFFORT_DEFAULT];
}

LlmEffort
llm_config_active_effort(void)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    LlmEffort   effort = LLM_EFFORT_DEFAULT;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *root =
            json_node_get_object(json_parser_get_root(parser));

        if (root != NULL && json_object_has_member(root, "active")) {
            JsonObject *active =
                json_object_get_object_member(root, "active");

            if (active != NULL &&
                json_object_has_member(active, "effort")) {
                const char *name =
                    json_object_get_string_member(active, "effort");

                for (int i = 0; i < LLM_EFFORT_COUNT; i++) {
                    if (g_strcmp0(name, LLM_EFFORT_NAMES[i]) == 0) {
                        effort = (LlmEffort)i;
                        break;
                    }
                }
            }
        }
    }
    g_object_unref(parser);
    g_free(path);
    return effort;
}

void
llm_config_set_active_effort(LlmEffort effort)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root, *active;
    JsonNode   *work = NULL;

    if (effort < 0 || effort >= LLM_EFFORT_COUNT)
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
     * crée. S'il existe, on n'y touche PAS sauf le champ effort —
     * provider/model/profile survivent, c'est leur fichier aussi. */
    if (!json_object_has_member(root, "active") ||
        json_object_get_object_member(root, "active") == NULL)
        json_object_set_object_member(root, "active", json_object_new());
    active = json_object_get_object_member(root, "active");
    json_object_set_string_member(active, "effort", effort_name(effort));

    {
        JsonGenerator *gen = json_generator_new();
        gchar         *text = json_to_string(work, TRUE);
        GError        *error = NULL;

        json_generator_set_root(gen, work);
        if (text != NULL && !g_file_set_contents(path, text, -1, &error)) {
            if (error != NULL) {
                g_printerr(_("CDB: failed to write active effort: %s\n"),
                           error->message);
                g_error_free(error);
            }
        }
        g_free(text);
        g_object_unref(gen);
        json_node_unref(work);
    }
    g_object_unref(parser);
    g_free(path);
}
