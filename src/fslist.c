#include "fslist.h"
#include <glib/gstdio.h>

/* Dossiers lourds/générés exclus de l'explorateur (comme tous les IDE).
 * Les dotfiles (nom commençant par « . ») sont exclus par ailleurs. */
static const char *ignored_dirs[] = {
    "node_modules", "target", "build", "dist", "out",
    "__pycache__", "venv", "vendor",
    NULL
};

static void fs_entry_finalize(GObject *object);

G_DEFINE_TYPE(FileEntry, fs_entry, G_TYPE_OBJECT)

static void
fs_entry_class_init(FileEntryClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = fs_entry_finalize;
}

static void
fs_entry_init(FileEntry *e)
{
    e->children = NULL;
}

static void
fs_entry_finalize(GObject *object)
{
    FileEntry *e = FS_ENTRY(object);

    if (e->children != NULL) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(e->children));
        for (guint i = 0; i < n; i++) {
            FileEntry *c = g_list_model_get_item(G_LIST_MODEL(e->children), i);
            g_object_unref(c);
        }
        g_object_unref(e->children);
    }
    g_free(e->name);
    g_free(e->path);
    G_OBJECT_CLASS(fs_entry_parent_class)->finalize(object);
}

FileEntry *
fs_entry_new(const char *path)
{
    FileEntry *e = g_object_new(FS_TYPE_ENTRY, NULL);

    e->path = g_strdup(path);
    e->name = g_path_get_basename(path);
    e->is_dir = g_file_test(path, G_FILE_TEST_IS_DIR);
    e->children_dirty = FALSE;
    return e;
}

/* Tri : dossiers d'abord, puis fichiers ; alphabétique dans chaque groupe. */
static gint
entry_compare(gconstpointer a, gconstpointer b)
{
    FileEntry *ea = *(FileEntry **)a;
    FileEntry *eb = *(FileEntry **)b;

    if (ea->is_dir != eb->is_dir)
        return ea->is_dir ? -1 : 1;
    return g_ascii_strcasecmp(ea->name, eb->name);
}

GListStore *
fs_scan_dir(const char *path)
{
    GListStore *store = g_list_store_new(FS_TYPE_ENTRY);
    GDir       *dir = g_dir_open(path, 0, NULL);
    const char *name;
    GPtrArray  *items = g_ptr_array_new_with_free_func(g_object_unref);

    if (dir == NULL)
        return store;

    while ((name = g_dir_read_name(dir)) != NULL) {
        FileEntry *e;
        char      *full;

        if (name[0] == '.')
            continue; /* dotfile : caché */
        for (int i = 0; ignored_dirs[i] != NULL; i++) {
            if (g_strcmp0(name, ignored_dirs[i]) == 0)
                goto skip;
        }
        full = g_build_filename(path, name, NULL);
        e = fs_entry_new(full);
        g_free(full);
        g_ptr_array_add(items, e);
    skip:
        ;
    }
    g_dir_close(dir);

    g_ptr_array_sort(items, entry_compare);
    for (guint i = 0; i < items->len; i++)
        g_list_store_append(store, items->pdata[i]);

    g_ptr_array_free(items, TRUE);
    return store;
}

/* Suppression récursive (fichier, dossier, symlink). ENOENT n'est pas
 * une erreur (élément déjà supprimé par un parent récursif). */
gboolean
fs_remove_recursive(const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK))
        return g_remove(path) == 0 || errno == ENOENT;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir        *dir = g_dir_open(path, 0, NULL);
        const char  *name;
        gboolean     ok = TRUE;

        if (dir == NULL)
            return FALSE;
        while ((name = g_dir_read_name(dir)) != NULL) {
            char *full = g_build_filename(path, name, NULL);

            if (!fs_remove_recursive(full))
                ok = FALSE;
            g_free(full);
        }
        g_dir_close(dir);
        if (!ok)
            return FALSE;
        return g_rmdir(path) == 0;
    }
    return g_remove(path) == 0 || errno == ENOENT;
}