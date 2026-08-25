/*
 * mdview.h : rendu Markdown minimal dans un GtkTextBuffer.
 *
 * V1 pragmatique pour la tuile LLM : blocs de code (fences), gras,
 * italique, code inline. Tolerant au markdown incomplet
 * (streaming : fence non fermee = code jusqu'a la fin).
 *
 * Blocs thinking : balises ouvrante/fermante rendues en sections
 * repliables façon ZED — header chevron + label, contenu attenue,
 * auto-repli a la fermeture. Necessite md_thinking_attach() (la vue ou
 * ancre les boutons) et md_thinking_reset() en debut de message.
 * Note : les balises ne transitent JAMAIS par le canal clavier —
 * elles sont extraites de llm.c par coordonnees d'octets puis
 * injectees ici par sed (placeholders @OPEN@ / @CLOSE@).
 */

#ifndef CDB_MDVIEW_H
#define CDB_MDVIEW_H

#include <gtk/gtk.h>

/* Insere md rendu a iter (les tags sont crees/reutilises par nom dans
 * la tag table du buffer). Rendu COMPLET : etat de parsing et
 * numerotation des blocs thinking repartent de zero. */
void md_insert(GtkTextBuffer *buf, GtkTextIter *iter, const char *md);

/* Rendu INCREMENTAL (streaming) : enchaine sur le rendu precedent —
 * etat fence/thinking et index des blocs preserves. `text` n'est pas
 * nul-terminal : `len` octets sont traites. Les lignes completes sont
 * rendues ; le fragment final sans \n ne l'est que si flush (fin de
 * stream), car il peut encore changer au chunk suivant. */
void md_insert_append(GtkTextBuffer *buf, GtkTextIter *iter,
                      const char *text, gsize len, gboolean flush);

/* Declare la vue dans laquelle ancre les boutons des blocs thinking.
 * Sans attach, les balises thinking restent du texte ordinaire. */
void md_thinking_attach(GtkTextBuffer *buf, GtkWidget *view);

/* Frontiere de generation : a appeler en debut de CHAQUE nouvelle
 * reponse (au moment ou reply_mark est pose). Les etats ouvert/ferme
 * repartent des defauts ; les blocs des messages anterieurs restent
 * independemment cliquables. */
void md_thinking_reset(GtkTextBuffer *buf);

#endif /* CDB_MDVIEW_H */
