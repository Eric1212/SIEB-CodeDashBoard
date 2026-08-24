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

#ifndef SIEB_MDVIEW_H
#define SIEB_MDVIEW_H

#include <gtk/gtk.h>

/* Insere md rendu a iter (les tags sont crees/reutilises par nom dans
 * la tag table du buffer). */
void md_insert(GtkTextBuffer *buf, GtkTextIter *iter, const char *md);

/* Declare la vue dans laquelle ancre les boutons des blocs thinking.
 * Sans attach, les balises thinking restent du texte ordinaire. */
void md_thinking_attach(GtkTextBuffer *buf, GtkWidget *view);

/* Frontiere de generation : a appeler en debut de CHAQUE nouvelle
 * reponse (au moment ou reply_mark est pose). Les etats ouvert/ferme
 * repartent des defauts ; les blocs des messages anterieurs restent
 * independemment cliquables. */
void md_thinking_reset(GtkTextBuffer *buf);

#endif /* SIEB_MDVIEW_H */
