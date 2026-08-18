/*
 * Dirty : état des fichiers non sauvegardés (témoin + contenu en attente).
 *
 * Un fichier "sale" a des modifications dans CDB non écrites sur disque.
 * On garde le contenu en attente (pour le restaurer à la ré-ouverture) et
 * on le persiste dans dirty.json, robuste aux crash (écriture en debounce).
 */

#ifndef SIEB_DIRTY_H
#define SIEB_DIRTY_H

#include <glib.h>

typedef struct {
    GHashTable *store;  /* path (g_strdup) -> contenu (g_free) ; présence = sale */
    char       *file;   /* chemin du dirty.json */
    guint       persist_timer; /* id du debounce d'écriture */
} DirtyStore;

/* Charge dirty.json (si présent) et initialise le store. */
DirtyStore *dirty_store_new(void);

/* Libère le store (n'écrit pas ; appeler dirty_persist_now avant). */
void dirty_store_free(DirtyStore *ds);

/* Marque path comme sale (contenu conservé). */
void dirty_mark(DirtyStore *ds, const char *path, const char *content);

/* Retire path du store (fichier propre / sauvegardé). */
void dirty_clear(DirtyStore *ds, const char *path);

gboolean dirty_contains(DirtyStore *ds, const char *path);

/* Contenu en attente (NULL si pas sale). */
const char *dirty_content(DirtyStore *ds, const char *path);

/* TRUE si un fichier sale est sous dir (égal ou descendant). */
gboolean dirty_under(DirtyStore *ds, const char *dir);

/* Écrit dirty.json immédiatement (toute la table). */
void dirty_persist_now(DirtyStore *ds);

/* Écrit dirty.json après ~1 s d'inactivité (debounce, robuste crash). */
void dirty_schedule_persist(DirtyStore *ds);

#endif /* SIEB_DIRTY_H */
