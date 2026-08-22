/*
 * mdview.h : rendu Markdown minimal dans un GtkTextBuffer.
 *
 * V1 pragmatique pour la tuile LLM : blocs de code (```), **gras**,
 * *italique* / _italique_, `code inline`. Tolérant au markdown incomplet
 * (streaming : fence non fermée = code jusqu'à la fin).
 */

#ifndef SIEB_MDVIEW_H
#define SIEB_MDVIEW_H

#include <gtk/gtk.h>

/* Insère md rendu à iter (les tags sont créés/réutilisés par nom dans
 * la tag table du buffer). */
void md_insert(GtkTextBuffer *buf, GtkTextIter *iter, const char *md);

#endif /* SIEB_MDVIEW_H */