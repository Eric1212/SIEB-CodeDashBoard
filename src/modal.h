/*
 * modal.h : fenêtres-modales — une pièce par fenêtre, max 4 simultanées.
 *
 * Générique : la titlebar ET le contenu sont fournis par l'appelant
 * (tuile vide à attribuer, futur Panneau de Contrôle…). Non bloquantes :
 * on circule entre les modales et la fenêtre principale. Transitoires :
 * fermées avec elle.
 */

#ifndef SIEB_MODAL_H
#define SIEB_MODAL_H

#include <gtk/gtk.h>

#define MODAL_MAX 4

/* Ouvre une fenêtre (transient sur parent). titlebar : widget complet
 * (GtkHeaderBar construit par l'appelant — il garde ses pointeurs pour
 * mettre titre/menu à jour). Renvoie FALSE si la limite de MODAL_MAX est
 * atteinte ; *win_out (optionnel) reçoit la fenêtre. */
gboolean modal_open(GtkWindow *parent, int *count, GtkWidget *titlebar,
                    GtkWidget *content, GtkWindow **win_out);

#endif /* SIEB_MODAL_H */