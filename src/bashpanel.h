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

/* Initialise le backend bash permanent au demarrage, independamment de
 * toute tuile visible : des cet appel, les outils peuvent executer sans
 * panneau affiche. Idempotent (ne cree le notebook qu'une seule fois). */
void bash_panel_init(GListStore *roots, GHashTable *multi_paths);

/* Injecte STRICTEMENT la commande dans l'onglet N (au clavier du shell).
 * Aucune plomberie : pas de redirection, pas de sentinelle. */
gboolean bash_panel_exec_tab(guint index, const char *command);

/* Garantit l'existence d'au moins count onglets. */
void bash_panel_ensure_tabs(guint count);

/* Un panneau bash est-il disponible (pour un appel d'outil) ? */
gboolean bash_panel_exec_tab_possible(void);

/* Nombre de lignes du buffer (scrollback inclus), -1 si indisponible. */
glong bash_panel_line_count(guint index);

/* Dernière ligne non vide du buffer (sans le \n), NULL si indisponible. */
gchar *bash_panel_last_line(guint index);

/* Lignes [first..last] du buffer (inclusives, bornées au contenu),
 * jointes par \n. NULL si indisponible. */
gchar *bash_panel_slice(guint index, glong first, glong last);

/* Texte intégral du buffer (scrollback inclus). NULL si indisponible. */
gchar *bash_panel_text(guint index);

/* Le terminal de l'onglet N existe-t-il encore ? */
gboolean bash_panel_term_alive(guint index);

/* Le shell de l'onglet N a-t-il terminé son spawn (PTY attaché) ?
 * feed_child avant ce point = commande perdue. */
gboolean bash_panel_term_ready(guint index);

/* Point orange sur l'étiquette de l'onglet N tant qu'une commande
 * d'outil y tourne : l'humain voit d'un coup d'œil quel bash attend
 * (TUI bloquante, prompt sale…) et va donner la touche attendue. */
void bash_panel_set_busy(guint index, gboolean busy);

/* Remplace l'onglet N par un terminal FRAIS (même index, même projet) :
 * équivalent du clic « x » + nouvel onglet. No-op si absent. */
void bash_panel_reset_tab(guint index);

/* Libere le backend bash permanent en fin de process. Le notebook se
 * finalise alors (s'il n'est plus accroche a une vue) et avec lui les
 * onglets encore vivants : leurs PTY ferment, comme un « x » sur chaque
 * terminal au moment de quitter. */
void bash_panel_shutdown(void);

#endif /* CDB_BASHPANEL_H */
