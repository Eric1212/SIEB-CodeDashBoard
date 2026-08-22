/*
 * mdview.c : rendu Markdown minimal dans un GtkTextBuffer — voir mdview.h.
 *
 * Approche : styles = GtkTextTag orthogonaux réutilisés par nom (bold,
 * italic, code…) ; l'inline combine plusieurs tags via
 * insert_with_tags_by_name — pas d'explosion combinatoire. Les blocs de
 * code priment sur tout inline. Tolérant au markdown incomplet (streaming).
 */

#include "mdview.h"

/* Crée le tag s'il n'existe pas déjà dans la tag table du buffer. */
static GtkTextTag *
md_ensure_tag(GtkTextBuffer *buf, const char *name)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag      *tag = gtk_text_tag_table_lookup(table, name);

    if (tag != NULL)
        return tag;

    if (strcmp(name, "md-bold") == 0)
        tag = gtk_text_buffer_create_tag(buf, name, "weight",
                                         PANGO_WEIGHT_BOLD, NULL);
    else if (strcmp(name, "md-italic") == 0)
        tag = gtk_text_buffer_create_tag(buf, name, "style",
                                         PANGO_STYLE_ITALIC, NULL);
    else if (strcmp(name, "md-code") == 0 ||
             strcmp(name, "md-codeblock") == 0)
        tag = gtk_text_buffer_create_tag(
            buf, name, "family", "monospace", "background-rgba",
            &(GdkRGBA){ 0.5, 0.5, 0.5, 0.25 }, NULL);
    else if (strcmp(name, "md-h1") == 0)
        tag = gtk_text_buffer_create_tag(buf, name, "scale", 1.4,
                                         "weight", PANGO_WEIGHT_BOLD, NULL);
    else if (strcmp(name, "md-h2") == 0)
        tag = gtk_text_buffer_create_tag(buf, name, "scale", 1.25,
                                         "weight", PANGO_WEIGHT_BOLD, NULL);
    else if (strcmp(name, "md-h3") == 0)
        tag = gtk_text_buffer_create_tag(buf, name, "scale", 1.1,
                                         "weight", PANGO_WEIGHT_BOLD, NULL);
    else if (strcmp(name, "md-quote") == 0)
        tag = gtk_text_buffer_create_tag(buf, name, "style",
                                         PANGO_STYLE_ITALIC, "indent", 12,
                                         "foreground-rgba",
                                         &(GdkRGBA){ 0.55, 0.55, 0.6, 1.0 },
                                         NULL);
    return tag;
}

/* Émission avec la pile courante (flags) + un tag de bloc optionnel. */
static void
md_emit(GtkTextBuffer *buf, GtkTextIter *it, const char *text, int len,
        gboolean bold, gboolean italic, gboolean code, const char *extra)
{
    const char *names[6] = { NULL };
    int         n = 0;

    if (extra != NULL)
        names[n++] = extra;
    if (bold)
        names[n++] = "md-bold";
    if (italic)
        names[n++] = "md-italic";
    if (code)
        names[n++] = "md-code";
    names[n] = NULL;

    if (n == 0) {
        gtk_text_buffer_insert(buf, it, text, len);
        return;
    }
    for (int i = 0; i < n; i++)
        md_ensure_tag(buf, names[i]);
    gtk_text_buffer_insert_with_tags_by_name(buf, it, text, len, names[0],
                                             names[1], names[2], names[3],
                                             names[4], NULL);
}

/* Ce caractère ouvre-t-il du markup dans l'état courant ? */
static gboolean
md_is_marker(const char *s, gboolean bold, gboolean italic, gboolean code)
{
    if (*s == '\0')
        return FALSE;
    if (!code && s[0] == '`')
        return TRUE;
    if (!code && !bold && s[0] == '*' && s[1] == '*')
        return TRUE;
    if (!code && !italic && s[0] == '*')
        return TRUE;
    return FALSE;
}

/* Rendu inline récursif : `code`, **gras**, *italique*. Le souligné « _ »
 * est volontairement ignoré (snake_case des identifiants : trop de faux
 * positifs dans les réponses techniques). */
static void
md_inline(GtkTextBuffer *buf, GtkTextIter *it, const char *s,
          gboolean bold, gboolean italic, gboolean code, const char *extra)
{
    while (*s != '\0') {
        /* Code inline : `…` (littéral, prioritaire, non imbricable). */
        if (!code && s[0] == '`') {
            const char *end = strchr(s + 1, '`');

            if (end != NULL) {
                md_emit(buf, it, s + 1, (int)(end - s - 1), bold, italic,
                        TRUE, extra);
                s = end + 1;
                continue;
            }
        }

        /* Gras : **…** */
        if (!code && !bold && s[0] == '*' && s[1] == '*') {
            const char *end = strstr(s + 2, "**");

            if (end != NULL) {
                const char *inner = s + 2;

                md_emit(buf, it, inner, (int)(end - inner), TRUE,
                        italic, code, extra);
                s = end + 2;
                continue;
            }
        }

        /* Italique : *…* (un seul astérisque). */
        if (!code && !italic && !bold && s[0] == '*') {
            const char *end = strchr(s + 1, '*');

            if (end != NULL && end > s + 1) {
                const char *inner = s + 1;

                md_emit(buf, it, inner, (int)(end - inner), bold, TRUE,
                        code, extra);
                s = end + 1;
                continue;
            }
        }

        /* Course ordinaire : avance jusqu'au prochain marqueur possible. */
        {
            const char *run = s;

            while (*s != '\0' && !md_is_marker(s, bold, italic, code))
                s++;
            if (s == run) { /* marqueur sans fermeture : littéral */
                s++;
                continue;
            }
            md_emit(buf, it, run, (int)(s - run), bold, italic, code,
                    extra);
        }
    }
}

void
md_insert(GtkTextBuffer *buf, GtkTextIter *iter, const char *md)
{
    char   **lines = g_strsplit(md, "\n", 0);
    gboolean in_fence = FALSE;

    for (char **l = lines; *l != NULL; l++) {
        const char *line = *l;
        int         len = (int)strlen(line);

        /* Fence de bloc de code : ``` … ``` (non fermée = jusqu'à la
         * fin — cas streaming). La ligne fence elle-même est avalée. */
        if (len >= 3 && strncmp(line, "```", 3) == 0) {
            in_fence = !in_fence;
            continue;
        }
        if (in_fence) {
            md_emit(buf, iter, line, len, FALSE, FALSE, TRUE,
                    "md-codeblock");
            gtk_text_buffer_insert(buf, iter, "\n", 1);
            continue;
        }

        /* Titres : # ## ### (+ espace). */
        {
            int lvl = 0;

            while (lvl < len && lvl < 3 && line[lvl] == '#')
                lvl++;
            if (lvl > 0 && lvl < len && line[lvl] == ' ') {
                static const char *htags[] = { NULL, "md-h1", "md-h2",
                                               "md-h3" };

                md_inline(buf, iter, line + lvl + 1, FALSE, FALSE, FALSE,
                          htags[lvl]);
                gtk_text_buffer_insert(buf, iter, "\n", 1);
                continue;
            }
        }

        /* Citation : "> …". */
        if (len >= 2 && line[0] == '>' && line[1] == ' ') {
            md_inline(buf, iter, line + 2, FALSE, FALSE, FALSE,
                      "md-quote");
            gtk_text_buffer_insert(buf, iter, "\n", 1);
            continue;
        }

        /* Liste à puce : "- ", "* ", "+ ". */
        if (len >= 2 && (line[0] == '-' || line[0] == '*' ||
                         line[0] == '+') && line[1] == ' ') {
            gtk_text_buffer_insert(buf, iter, "\u2022  ", -1);
            md_inline(buf, iter, line + 2, FALSE, FALSE, FALSE, NULL);
            gtk_text_buffer_insert(buf, iter, "\n", 1);
            continue;
        }

        /* Ligne vide : respiration de paragraphe. */
        if (len == 0) {
            gtk_text_buffer_insert(buf, iter, "\n", 1);
            continue;
        }

        /* Paragraphe ordinaire. */
        md_inline(buf, iter, line, FALSE, FALSE, FALSE, NULL);
        gtk_text_buffer_insert(buf, iter, "\n", 1);
    }
    g_strfreev(lines);
}