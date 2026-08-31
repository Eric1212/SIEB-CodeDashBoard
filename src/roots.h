/*
 * Roots : modèle des "Roots de structure" et "Roots de projet".
 *
 * Un Root de structure est un dossier qui contient des projets
 * (ex: /home/eric/dev) — on n'y travaille jamais directement.
 * Un Root de projet est un dossier de travail ouvert dans l'IDE
 * (ex: /home/eric/dev/alvalllm), comme dans Zed.
 *
 * Persistance : ~/.config/cdb/roots.json
 */

#ifndef CDB_ROOTS_H
#define CDB_ROOTS_H

#include <gtk/gtk.h>

typedef enum {
    ROOT_STRUCTURE,
    ROOT_PROJECT,
} RootKind;

typedef struct _RootEntry RootEntry;

typedef struct _RootEntry {
    GObject      parent_instance;
    char        *path;      /* chemin absolu du root */
    RootKind     kind;
    char        *basename;  /* nom affiché (dernier segment) */
    GListStore  *children;  /* projets si STRUCTURE, NULL si PROJECT */
    RootEntry   *parent;    /* structure d'accueil, NULL si à la racine */
    GListStore  *contents;  /* explorateur du projet (FileEntry), lazy */
    gboolean     contents_dirty; /* re-scan requis après modif FS */
    gboolean     dirty;      /* contient un fichier non sauvegardé (propagé) */
    GtkWidget   *indicator;  /* widget témoin de la row (row visible), NULL */
} RootEntry;

typedef struct _RootEntryClass {
    GObjectClass parent_class;
} RootEntryClass;

#define ROOT_TYPE_ENTRY (root_entry_get_type())
#define ROOT_ENTRY(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), ROOT_TYPE_ENTRY, RootEntry))

GType root_entry_get_type(void);

/* Charge les racines depuis le membre "roots" de llm.json (appelé une fois au
 * démarrage ; un ancien roots.json est migré, relu, puis supprimé).
 * Retourne un GListStore non-référencé de RootEntry. */
GListStore *roots_load(void);

/* Sauvegarde le store complet dans le membre "roots" de llm.json (à chaque
 * modification). Fusion, et non réécriture : "last_file", providers et
 * api_key survivent — ce que l'ancien roots.json, écrit en entier, ne
 * garantissait pas. */
void roots_save(GListStore *roots);

/* Ajoute un root. parent = structure d'accueil, ou NULL pour la racine. */
RootEntry *roots_add(GListStore *roots, RootEntry *parent,
                     RootKind kind, const char *path);

/* Ajoute un root de structure, puis scanne ses sous-dossiers directs :
 * chacun devient un root projet enfant (exclus : fichiers, cachés,
 * doublons). */
RootEntry *roots_add_structure(GListStore *roots, const char *path);

/* Re-scan des structures : un sous-dossier direct non encore connu devient
 * un root projet. Un projet n'est pas persisté, il est découvert — c'est
 * ce qui rend un projet créé hors de CDB visible sans redémarrer. Les
 * projets disparus du disque ne sont pas retirés. */
void roots_rescan_structures(GListStore *roots);

/* TRUE si le chemin est déjà un root (racine) ou un projet d'une
 * structure : empêche les doublons à l'ajout. */
gboolean roots_conflict(GListStore *roots, const char *path);

/* Dernier fichier ouvert : lu depuis roots.json (clé "last_file"). */
char *roots_read_last_file(void);
void roots_write_last_file(const char *path);

/* Retire un root (et ses enfants) du store, ou un projet de sa structure. */
void roots_remove(GListStore *roots, RootEntry *entry);

/* Suppression récursive d'un dossier (ne suit pas les symlinks).
 * Retourne FALSE en cas d'échec. */
gboolean roots_delete_recursive(const char *path);

/* Le projet courant d'après la sélection (multi_paths : chemins choisis
 * dans l'explorateur) : un projet sélectionné directement est prioritaire
 * sur un fichier DANS un projet ; les structures ne comptent jamais.
 * Retourne un chemin g_strdup (à libérer) ou NULL si rien de résoluble. */
char *roots_current_project(GListStore *roots, GHashTable *multi_paths);

/* Chemin court « basename-du-projet/relatif » d'un chemin absolu, pour le nom
 * de la fenêtre. Projet contenant path, sinon path tel quel ; NULL si path
 * NULL. Renvoie une chaîne g_strdup (à libérer). */
char *roots_project_path(GListStore *roots, const char *path);

#endif /* CDB_ROOTS_H */