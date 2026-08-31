/*
 * llmlive.c : persistance « dirty » de la conversation — voir llmlive.h.
 */

#include "llmlive.h"
#include "session.h"
#include "i18n.h"
#include "mem.h"

#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define LIVE_FILE "llm_live.json"

static char *
live_path(void)
{
    return session_config_path(LIVE_FILE);
}

static const char *
kind_wire(LlmMsgKind kind)
{
    switch (kind) {
    case LLM_MSG_ASSISTANT_TOOL_CALLS:
        return "assistant_tool_calls";
    case LLM_MSG_TOOL_RESULT:
        return "tool_result";
    case LLM_MSG_TEXT:
    default:
        return "text";
    }
}

void
llm_live_save(LlmCore *c)
{
    char       *path;
    char       *data;
    JsonNode   *root;
    JsonObject *obj;
    JsonArray  *arr;

    if (c == NULL || c->history == NULL)
        return;

    path = live_path();

    if (c->history->len == 0) {
        g_remove(path);
        g_free(path);
        return;
    }

    arr = json_array_new();
    for (guint i = 0; i < c->history->len; i++) {
        LlmMsg     *m = &g_array_index(c->history, LlmMsg, i);
        JsonObject *mo = json_object_new();

        json_object_set_string_member(mo, "actor",
                                      m->actor == LLMACTOR_USER ? "user" :
                                      m->actor == LLMACTOR_LLM  ? "llm" :
                                      "cdb");
        json_object_set_string_member(mo, "kind", kind_wire(m->kind));
        json_object_set_boolean_member(mo, "local", m->local);

        if (m->content != NULL)
            json_object_set_string_member(mo, "content", m->content);
        else
            json_object_set_null_member(mo, "content");

        if (m->tool_call_id != NULL)
            json_object_set_string_member(mo, "tool_call_id",
                                          m->tool_call_id);

        if (m->tool_calls != NULL && m->tool_calls->len > 0) {
            JsonArray *ta = json_array_new();

            for (guint k = 0; k < m->tool_calls->len; k++) {
                LlmToolCall *tc = g_ptr_array_index(m->tool_calls, k);
                JsonObject  *to = json_object_new();

                json_object_set_string_member(to, "id",
                                              tc->id != NULL ? tc->id : "");
                json_object_set_string_member(to, "name",
                                              tc->name != NULL
                                                  ? tc->name : "");
                json_object_set_string_member(
                    to, "arguments",
                    tc->arguments_json != NULL ? tc->arguments_json : "");
                json_array_add_object_element(ta, to);
            }
            json_object_set_array_member(mo, "tool_calls", ta);
        }

        if (m->images != NULL && m->images->len > 0) {
            JsonArray *ia = json_array_new();

            for (guint k = 0; k < m->images->len; k++)
                json_array_add_string_element(ia,
                    (const char *)g_ptr_array_index(m->images, k));
            json_object_set_array_member(mo, "images", ia);
        }
        json_array_add_object_element(arr, mo);
    }

    obj = json_object_new();
    json_object_set_array_member(obj, "messages", arr);
    root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, obj);
    data = json_to_string(root, TRUE);

    g_file_set_contents(path, data, -1, NULL);

    g_free(data);
    g_free(path);
    json_node_unref(root);
    /* Écrire, libérer, rendre les pages : trois gestes, et le troisième ne
     * suit pas tout seul. free() rend des objets, pas des pages — donc une
     * sauvegarde « propre » laisse son haut-pic cartographié. Mesuré en
     * rejouant ce geste hors de l'arbre, sur des fils réels : sans trim, dix
     * sauvegardes d'un fil de 275 messages (747 Ko) font monter RssAnon de
     * +904 Ko, par à-coups de 0,5 à 2,5 Mo, pendant que le tas alloué reste
     * plat à 782 Ko — rien ne fuit, ça reste collé. Avec trim, la courbe est
     * plate, solde final -420 Ko ; sur un fil de 2,7 Mo, 9,6 Mo au lieu de
     * 17,3 Mo. Coût : 0,06 à 0,3 ms sur 747 Ko, 0,2 à 1 ms sur 2,7 Mo —
     * quelques millisecondes par tour d'outil, qui en paie un de plusieurs
     * secondes. Le trim n'est pas un filet anti-fuite : un objet vivant
     * n'est jamais concerné (voir mem.c). */
    cdb_mem_trim();
}

static LlmMsgKind
kind_parse(const char *s)
{
    if (g_strcmp0(s, "assistant_tool_calls") == 0)
        return LLM_MSG_ASSISTANT_TOOL_CALLS;
    if (g_strcmp0(s, "tool_result") == 0)
        return LLM_MSG_TOOL_RESULT;
    return LLM_MSG_TEXT;
}

static void
llm_live_repair_open_tool_calls(LlmCore *c)
{
    guint n;

    if (c == NULL || c->history == NULL)
        return;

    n = c->history->len;
    for (guint i = 0; i < n; i++) {
        LlmMsg *m = &g_array_index(c->history, LlmMsg, i);

        if (m->kind == LLM_MSG_TOOL_RESULT && m->tool_call_id != NULL &&
            c->answered_tools != NULL)
            g_hash_table_add(c->answered_tools,
                             g_strdup(m->tool_call_id));

        if (m->kind != LLM_MSG_ASSISTANT_TOOL_CALLS ||
            m->tool_calls == NULL)
            continue;

        for (guint k = 0; k < m->tool_calls->len; k++) {
            LlmToolCall *tc = g_ptr_array_index(m->tool_calls, k);
            gboolean     answered = FALSE;
            LlmMsg       repair;

            if (tc->id == NULL || tc->id[0] == '\0')
                continue;

            for (guint j = i + 1; j < c->history->len && !answered; j++) {
                LlmMsg *r = &g_array_index(c->history, LlmMsg, j);

                answered = r->kind == LLM_MSG_TOOL_RESULT &&
                           g_strcmp0(r->tool_call_id, tc->id) == 0;
            }
            if (answered)
                continue;

            memset(&repair, 0, sizeof(repair));
            repair.actor = LLMACTOR_CDB;
            repair.local = FALSE;
            repair.kind = LLM_MSG_TOOL_RESULT;
            repair.content = g_strdup(
                _("Interrupted by the CDB restart."));
            repair.tool_call_id = g_strdup(tc->id);
            g_array_append_vals(c->history, &repair, 1);

            if (c->answered_tools != NULL)
                g_hash_table_add(c->answered_tools,
                                 g_strdup(tc->id));
        }
    }
}

void
llm_live_load(LlmCore *c)
{
    char       *path;
    char       *data = NULL;
    JsonNode   *root;
    JsonObject *obj;
    JsonArray  *arr;

    if (c == NULL || c->history == NULL)
        return;

    path = live_path();
    if (!g_file_get_contents(path, &data, NULL, NULL)) {
        g_free(path);
        return;
    }
    g_free(path);

    root = json_from_string(data, NULL);
    g_free(data);
    if (root == NULL)
        return;
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        json_node_unref(root);
        return;
    }
    obj = json_node_get_object(root);
    if (!json_object_has_member(obj, "messages")) {
        json_node_unref(root);
        return;
    }
    arr = json_object_get_array_member(obj, "messages");

    for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonNode   *mn = json_array_get_element(arr, i);
        JsonObject *mo;
        LlmMsg      m = { 0 };
        JsonNode   *node;
        const char *actor;
        const char *kind;

        if (mn == NULL || !JSON_NODE_HOLDS_OBJECT(mn))
            continue;
        mo = json_node_get_object(mn);

        actor = json_object_has_member(mo, "actor")
              ? json_object_get_string_member(mo, "actor") : "cdb";
        kind = json_object_has_member(mo, "kind")
             ? json_object_get_string_member(mo, "kind") : "text";

        m.actor = g_strcmp0(actor, "user") == 0 ? LLMACTOR_USER :
                  g_strcmp0(actor, "llm")  == 0 ? LLMACTOR_LLM :
                  LLMACTOR_CDB;
        m.kind = kind_parse(kind);
        m.local = json_object_has_member(mo, "local")
              ? json_object_get_boolean_member(mo, "local") : FALSE;

        if (json_object_has_member(mo, "content")) {
            node = json_object_get_member(mo, "content");

            if (JSON_NODE_HOLDS_NULL(node))
                m.content = NULL;
            else if (JSON_NODE_HOLDS_VALUE(node) &&
                     json_node_get_value_type(node) == G_TYPE_STRING)
                m.content = g_strdup(json_node_get_string(node));
        }

        if (json_object_has_member(mo, "tool_call_id")) {
            node = json_object_get_member(mo, "tool_call_id");

            if (!JSON_NODE_HOLDS_NULL(node) &&
                JSON_NODE_HOLDS_VALUE(node) &&
                json_node_get_value_type(node) == G_TYPE_STRING)
                m.tool_call_id = g_strdup(json_node_get_string(node));
        }

        if (json_object_has_member(mo, "tool_calls")) {
            JsonArray *ta = json_object_get_array_member(mo, "tool_calls");

            if (json_array_get_length(ta) > 0) {
                m.tool_calls = llm_tool_calls_new();
                for (guint k = 0; k < json_array_get_length(ta); k++) {
                    JsonNode    *tn = json_array_get_element(ta, k);
                    JsonObject  *to;
                    LlmToolCall *tc;

                    if (tn == NULL || !JSON_NODE_HOLDS_OBJECT(tn))
                        continue;
                    to = json_node_get_object(tn);
                    tc = g_new0(LlmToolCall, 1);

                    if (json_object_has_member(to, "id"))
                        tc->id = g_strdup(
                            json_object_get_string_member(to, "id"));
                    if (json_object_has_member(to, "name"))
                        tc->name = g_strdup(
                            json_object_get_string_member(to, "name"));
                    if (json_object_has_member(to, "arguments"))
                        tc->arguments_json = g_strdup(
                            json_object_get_string_member(to,
                                                          "arguments"));
                    g_ptr_array_add(m.tool_calls, tc);
                }
            }
        }

        if (json_object_has_member(mo, "images")) {
            JsonArray *ia = json_object_get_array_member(mo, "images");

            if (json_array_get_length(ia) > 0) {
                m.images = g_ptr_array_new_with_free_func(g_free);
                for (guint k = 0; k < json_array_get_length(ia); k++)
                    g_ptr_array_add(m.images,
                        g_strdup(json_array_get_string_element(ia, k)));
            }
        }
        g_array_append_vals(c->history, &m, 1);
    }

    /* Si CDB a quitté entre un assistant.tool_calls et ses résultats,
     * complète les réponses manquantes : l'historique reste valide. */
    llm_live_repair_open_tool_calls(c);
    json_node_unref(root);
}

void
llm_live_wipe(void)
{
    char *path = live_path();

    g_remove(path);
    g_free(path);
}
