/*
 * llmlive.c : persistance « dirty » de la conversation — voir llmlive.h.
 */

#include "llmlive.h"
#include "session.h"

#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define LIVE_FILE "llm_live.json"

static char *
live_path(void)
{
    return session_config_path(LIVE_FILE);
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
        json_object_set_boolean_member(mo, "local", m->local);
        json_object_set_string_member(mo, "content",
                                      m->content != NULL ? m->content : "");
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
        LlmMsg      m;
        const char *actor;
        const char *content;

        if (mn == NULL || !JSON_NODE_HOLDS_OBJECT(mn))
            continue;
        mo = json_node_get_object(mn);

        actor = json_object_has_member(mo, "actor")
              ? json_object_get_string_member(mo, "actor") : "cdb";
        content = json_object_has_member(mo, "content")
              ? json_object_get_string_member(mo, "content") : "";

        m.actor = g_strcmp0(actor, "user") == 0 ? LLMACTOR_USER :
                  g_strcmp0(actor, "llm")  == 0 ? LLMACTOR_LLM :
                  LLMACTOR_CDB;
        m.local = json_object_has_member(mo, "local")
              ? json_object_get_boolean_member(mo, "local") : FALSE;
        m.content = g_strdup(content);
        m.images = NULL;

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
    json_node_unref(root);
}

void
llm_live_wipe(void)
{
    char *path = live_path();

    g_remove(path);
    g_free(path);
}
