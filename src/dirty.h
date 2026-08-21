/*
 * Dirty : état des fichiers non sauvegardés (témoin + contenu en attente).
 *
 * Un fichier "sale" a des modifications dans CDB non écrites sur disque. On
 * garde :
 *  - le contenu EN ATTENTE (les modifications non sauvegardées),
 *  - le BASELINE (le contenu « propre » dont découlent ces modifications),
 *    pour ne jamais confondre un changement externe du fichier avec le dirty.
 * On persiste le tout dans dirty.json, robuste aux crash (debounce).
 */

#ifndef CDB_DIRTY_H
#define CDB_DIRTY_H

#include <glib.h>

/* Une entrée sale. */
typedef struct {
    char *content;   /* contenu en attente (non sauvegardé) */
    char *baseline;  /* contenu « propre » de référence (avant modifications) */
} DirtyEntry;

typedef struct {
    GHashTable *store;  /* path (g_strdup) -> DirtyEntry* (g_free) */
    char       *file;   /* chemin du dirty.json */
    guint       persist_timer; /* id du debounce d'écriture */
} DirtyStore;

/* Charge dirty.json (si présent) et initialise le store. */
DirtyStore *dirty_store_new(void);

/* Libère le store (n'écrit pas ; appeler dirty_persist_now avant). */
void dirty_store_free(DirtyStore *ds);

/* Marque path comme sale. baseline = contenu propre dont les modifications
 * découlent ; s'il est déjà marqué, on ne met à jour que le contenu en
 * attente (le baseline d'origine est conservé). */
void dirty_mark(DirtyStore *ds, const char *path, const char *content,
                const char *baseline);

/* Retire path du store (fichier propre / sauvegardé). */
void dirty_clear(DirtyStore *ds, const char *path);

gboolean dirty_contains(DirtyStore *ds, const char *path);

/* Contenu en attente (NULL si pas sale). */
const char *dirty_content(DirtyStore *ds, const char *path);

/* Baseline d'origine (contenu propre de référence, NULL si absent). */
const char *dirty_baseline(DirtyStore *ds, const char *path);

/* TRUE si un fichier sale est sous dir (égal ou descendant). */
gboolean dirty_under(DirtyStore *ds, const char *dir);

/* Écrit dirty.json immédiatement (toute la table). */
void dirty_persist_now(DirtyStore *ds);

/* Écrit dirty.json après ~1 s d'inactivité (debounce, robuste crash). */
void dirty_schedule_persist(DirtyStore *ds);

#endif /* CDB_DIRTY_H */