/* llmtrim.c — préférence active.trim : le budget de messages du fil.
 *
 * Pourquoi un fichier pour deux fonctions. La loi du trim vit au core
 * (llm_history_trim, llmcore.c) : c'est LUI qui décide où couper. Ce
 * fichier n'en est que le REGLAGE — lire et écrire un entier dans
 * llm.json active.trim, la préférence globale de session. llmeffort.c a
 * porté ces deux fonctions un instant, par ressemblance de forme (« même
 * patron que l'effort ») et non de fond : l'effort est une CLÉ DE TABLE
 * (huit niveaux, un enum, des tables gardées par static_assert), le trim
 * est un BUDGET SUR LE FIL. Ils ne partageaient que la danse json-glib
 * lire/écrire dans le bloc active. Le trim a donc son fichier, comme
 * llmlive.c, llmtoolpref.c et llmslots.c ont le leur — et comme mem.c
 * justifie en en-tête un fichier pour une seule ligne.
 *
 * La loi, qui est celle d'Éric et qui est MESURÉE (T1 = 3 messages,
 * T2 = 2, T3 = 5, len = 10) :
 *
 *   n = 0..5 vident T1-T2-T3, n = 6 et 7 gardent T3, n = 8, 9, 10 gardent
 *   T2+T3, n = 11 ne coupe rien.
 *
 * Unité = MESSAGE, frontière = TOUR, tour INDIVISIBLE. Le trim précède le
 * push du message qui ouvre le nouveau tour, et ce message réserve sa
 * place dans n — d'où budget = n - 1 au core. Ces deux lignes ne sont
 * qu'un ENTONNOIR ici : la décision est à llm_history_trim, qui lit cette
 * valeur une fois par play.
 *
 * Le défaut est 9999 et non 20 : l'absence du membre ne doit JAMAIS
 * couper un fil existant. 0 est une valeur comme une autre, la seule qui
 * vide le fil — c'est pour cela que la lecture, elle, doit être stricte
 * (voir trim_is_numeric). */
#include "llm.h"
#include "i18n.h"

#include <json-glib/json-glib.h>

#define LLM_TRIM_DEFAULT 9999
#define LLM_TRIM_MAX     9999

/* Un trim ne se lit que si le membre est un NOMBRE. json_object_get_int_member
 * rend 0 a « "10" » comme a null, et 1 a true — or 0 et 1 sont exactement les
 * deux valeurs qui vident le fil : une config mal typee effacerait la
 * conversation au prochain play, ce que la loi du defaut 9999 defend. Un
 * tableau ou un objet font de plus sauter une assertion CRITICAL dans
 * json-glib (sonde sur 12 documents). HOLDS_VALUE seul ne suffit pas : il
 * est vrai pour une chaine et pour un booleen. */
static gboolean
trim_is_numeric(JsonNode *nd)
{
    return nd != NULL && JSON_NODE_HOLDS_VALUE(nd) &&
           (json_node_get_value_type(nd) == G_TYPE_INT64 ||
            json_node_get_value_type(nd) == G_TYPE_DOUBLE);
}

int
llm_config_active_trim(void)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    int         trim = LLM_TRIM_DEFAULT;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *root =
            json_node_get_object(json_parser_get_root(parser));

        if (root != NULL && json_object_has_member(root, "active")) {
            JsonObject *active =
                json_object_get_object_member(root, "active");

            if (active != NULL &&
                json_object_has_member(active, "trim")) {
                JsonNode *nd = json_object_get_member(active, "trim");
                gint64 v = trim_is_numeric(nd) ? json_node_get_int(nd) : -1;

                if (v >= 0 && v <= LLM_TRIM_MAX)
                    trim = (int)v;
            }
        }
    }
    g_object_unref(parser);
    g_free(path);
    return trim;
}

void
llm_config_set_active_trim(int n)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root, *active;
    JsonNode   *work = NULL;

    if (n < 0 || n > LLM_TRIM_MAX)
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
     * crée. S'il existe, on n'y touche PAS sauf le champ trim —
     * provider/model/profile/effort survivent, c'est leur fichier
     * aussi. */
    if (!json_object_has_member(root, "active") ||
        json_object_get_object_member(root, "active") == NULL)
        json_object_set_object_member(root, "active", json_object_new());
    active = json_object_get_object_member(root, "active");
    json_object_set_int_member(active, "trim", n);

    {
        JsonGenerator *gen = json_generator_new();
        gchar         *text = json_to_string(work, TRUE);
        GError        *error = NULL;

        json_generator_set_root(gen, work);
        if (text != NULL && !g_file_set_contents(path, text, -1, &error)) {
            if (error != NULL) {
                g_printerr(_("CDB: failed to write active trim: %s\n"),
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
