/*
 * bashpanel.h : panneau « Bash » — un GtkNotebook de 1 à 10 terminaux
 * VTE (vrais shells interactifs). Pièce du layout, id "bash".
 */

#ifndef CDB_BASHPANEL_H
#define CDB_BASHPANEL_H

#include <gtk/gtk.h>
#include "roots.h"

/* Crée le panneau (notebook + 1er onglet lancé). Chaque NOUVEAU terminal
 * est lancé dans le projet actuellement sélectionné (résolu au moment du
 * spawn via roots/multi_paths), sinon $HOME. */
GtkWidget *bash_panel_new(GListStore *roots, GHashTable *multi_paths);

#endif /* CDB_BASHPANEL_H */