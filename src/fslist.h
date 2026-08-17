/*
 * Explorateur de fichiers : contenu d'un projet (arborescence).
 *
 * FileEntry : nœud de l'arbre (dossier ou fichier). Les enfants d'un
 * dossier sont scannés paresseusement, au dépliage (GtkTreeListModel).
 */

#ifndef SIEB_FSLIST_H
#define SIEB_FSLIST_H

#include <gtk/gtk.h>

typedef struct _FileEntry {
    GObject     parent_instance;
    char       *name;      /* nom affiché */
    char       *path;      /* chemin absolu */
    gboolean    is_dir;
    GListStore *children;  /* scanné au premier dépliage, NULL sinon */
    gboolean    children_dirty; /* re-scan requis après modif FS */
} FileEntry;

typedef struct _FileEntryClass {
    GObjectClass parent_class;
} FileEntryClass;

#define FS_TYPE_ENTRY (fs_entry_get_type())
#define FS_ENTRY(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), FS_TYPE_ENTRY, FileEntry))

GType fs_entry_get_type(void);

/* Crée une FileEntry pour un chemin donné (scanne le type sur disque). */
FileEntry *fs_entry_new(const char *path);

/* Scanne un dossier : retourne un GListStore non-référencé de FileEntry
 * (enfants directs). Exclus : dotfiles et dossiers lourds.
 * Tri : dossiers d'abord, puis fichiers, alphabétique. */
GListStore *fs_scan_dir(const char *path);

/* Suppression récursive (fichier, dossier, symlink). ENOENT ignoré. */
gboolean fs_remove_recursive(const char *path);

#endif /* SIEB_FSLIST_H */