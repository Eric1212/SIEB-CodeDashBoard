/*
 * Dirty : état des fichiers non sauvegardés (témoin + contenu en attente).
 *
 * Persistance : ~/.config/cdb/dirty.json
 * Format : {"dirty":[{"path": "...", "content": "...", "baseline": "..."}]}
 */

#include "dirty.h"
#include "session.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>

#define CDB_DIRTY_FILE "dirty.json"

#define DIRTY_PERSIST_DELAY_MS 1000

static char *
dirty_config_path(void)
{
    return session_config_path(CDB_DIRTY_FILE);
}

static DirtyEntry *
dirty_entry_new(const char *content, const char *baseline)
{
    DirtyEntry *e = g_new0(DirtyEntry, 1);

    e->content = g_strdup(content != NULL ? content : "");
    e->baseline = g_strdup(baseline != NULL ? baseline : "");
    return e;
}

static void
dirty_entry_free(gpointer ptr)
{
    DirtyEntry *e = ptr;

    if (e == NULL)
        return;
    g_free(e->content);
    g_free(e->baseline);
    g_free(e);
}

DirtyStore *
dirty_store_new(void)
{
    DirtyStore   *ds = g_new0(DirtyStore, 1);
    JsonParser   *parser;
    JsonNode     *root;
    GError       *error = NULL;

    ds->store = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      dirty_entry_free);
    ds->file = dirty_config_path();

    parser = json_parser_new();
    if (!json_parser_load_from_file(parser, ds->file, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_printerr("CDB: dirty.json : %s\n", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return ds;
    }

    root = json_parser_get_root(parser);
    if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *obj = json_node_get_object(root);
        JsonArray  *arr = json_object_get_array_member(obj, "dirty");

        if (arr != NULL) {
            guint n = json_array_get_length(arr);

            for (guint i = 0; i < n; i++) {
                JsonObject *e = json_array_get_object_element(arr, i);
                const char *path = json_object_get_string_member(e, "path");
                const char *content = json_object_get_string_member(e, "content");
                const char *baseline;

                /* Baseline absent (ancien format) : on retombe sur le
                 * contenu pour ne pas créer de diff fantôme. */
                if (json_object_has_member(e, "baseline"))
                    baseline = json_object_get_string_member(e, "baseline");
                else
                    baseline = content;

                /* Règle : un dirty dont le fichier a disparu n'est
                 * atteignable par personne — il n'y a plus de ligne à
                 * cliquer dans l'explorateur — et il allume un ● sans
                 * chemin sur tous ses ancêtres. Le garder ne protège rien
                 * de récupérable ; c'est même ce qui permet, si le chemin
                 * réapparaît un jour (git checkout, fichier recréé sous le
                 * même nom), à load_file de restaurer ce contenu périmé
                 * par-dessus le disque, sans un mot. On l'abandonne donc
                 * ici, et on le dit : c'est du travail jeté, il ne doit
                 * pas disparaître en silence. */
                if (path == NULL || content == NULL)
                    continue;
                if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
                    g_printerr("CDB: dirty abandonné, fichier absent : %s\n",
                               path);
                    continue;
                }
                g_hash_table_insert(ds->store, g_strdup(path),
                                    dirty_entry_new(content, baseline));
            }
        }
    }
    g_object_unref(parser);
    return ds;
}

void
dirty_store_free(DirtyStore *ds)
{
    if (ds == NULL)
        return;
    if (ds->persist_timer != 0)
        g_source_remove(ds->persist_timer);
    g_hash_table_destroy(ds->store);
    g_free(ds->file);
    g_free(ds);
}

void
dirty_mark(DirtyStore *ds, const char *path, const char *content,
           const char *baseline)
{
    DirtyEntry *e = g_hash_table_lookup(ds->store, path);

    if (e != NULL) {
        /* Déjà sale : on rafraîchit le contenu, on garde le baseline. */
        g_free(e->content);
        e->content = g_strdup(content);
        return;
    }
    g_hash_table_insert(ds->store, g_strdup(path),
                        dirty_entry_new(content, baseline));
}

void
dirty_clear(DirtyStore *ds, const char *path)
{
    g_hash_table_remove(ds->store, path);
}

gboolean
dirty_contains(DirtyStore *ds, const char *path)
{
    return g_hash_table_contains(ds->store, path);
}

const char *
dirty_content(DirtyStore *ds, const char *path)
{
    DirtyEntry *e = g_hash_table_lookup(ds->store, path);

    return e != NULL ? e->content : NULL;
}

const char *
dirty_baseline(DirtyStore *ds, const char *path)
{
    DirtyEntry *e = g_hash_table_lookup(ds->store, path);

    return e != NULL ? e->baseline : NULL;
}

static gboolean
path_is_under(const char *path, const char *dir)
{
    size_t n = strlen(dir);

    return g_str_has_prefix(path, dir) && (path[n] == '\0' || path[n] == '/');
}

gboolean
dirty_under(DirtyStore *ds, const char *dir)
{
    GHashTableIter iter;
    gpointer       key;

    g_hash_table_iter_init(&iter, ds->store);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (path_is_under(key, dir))
            return TRUE;
    }
    return FALSE;
}

static gboolean
dirty_persist_timeout(gpointer data)
{
    DirtyStore *ds = data;

    ds->persist_timer = 0;
    dirty_persist_now(ds);
    return G_SOURCE_REMOVE;
}

void
dirty_schedule_persist(DirtyStore *ds)
{
    if (ds->persist_timer != 0)
        g_source_remove(ds->persist_timer);
    ds->persist_timer = g_timeout_add(DIRTY_PERSIST_DELAY_MS,
                                      dirty_persist_timeout, ds);
}

void
dirty_persist_now(DirtyStore *ds)
{
    JsonBuilder *builder;
    JsonNode    *root;
    gchar       *text;
    GError      *error = NULL;
    GHashTableIter iter;
    gpointer     key, value;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "dirty");
    json_builder_begin_array(builder);

    g_hash_table_iter_init(&iter, ds->store);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        DirtyEntry *e = value;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "path");
        json_builder_add_string_value(builder, key);
        json_builder_set_member_name(builder, "content");
        json_builder_add_string_value(builder, e->content);
        json_builder_set_member_name(builder, "baseline");
        json_builder_add_string_value(builder, e->baseline);
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    text = json_to_string(root, TRUE);
    if (!g_file_set_contents(ds->file, text, -1, &error)) {
        g_printerr("CDB: écriture dirty.json : %s\n",
                   error->message);
        g_error_free(error);
    }
    g_free(text);
    json_node_unref(root);
    g_object_unref(builder);
}