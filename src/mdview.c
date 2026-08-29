/*
 * mdview.c : rendu Markdown minimal dans un GtkTextBuffer — voir mdview.h.
 *
 * Approche : styles = GtkTextTag orthogonaux reutilises par nom (bold,
 * italic, code…) ; l'inline combine plusieurs tags via
 * insert_with_tags_by_name — pas d'explosion combinatoire. Les blocs de
 * code priment sur tout inline. Tolerant au markdown incomplet (streaming).
 *
 * Blocs thinking : trois couches d'etat.
 *  1. Instances (marks referencees + bouton) : meurent quand leur zone
 *     de texte est effacee — purge paresseuse via gtk_text_mark_get_deleted().
 *  2. Etats par generation (expanded/touched par index d'occurrence) :
 *     survivent aux re-rendus du meme message (streaming).
 *  3. md_thinking_reset() incremente la generation : frontiere entre
 *     messages ; anti-collision par compteur de generation.
 *
 * Gravites de marks : start = left, end = right → le mark de fin suit
 * automatiquement la croissance du contenu pendant le stream. Le tag
 * invisible etant herite a l'insertion, un bloc replie en cours de
 * stream reste replie.
 */

#include "mdview.h"
#include "i18n.h"

#include <string.h>

#define THINK_CTX_KEY  "md-think-ctx"
#define THINK_LABEL    N_("Thinking")
#define THINK_TAG_BODY "md-think-body"
#define THINK_TAG_HIDE "md-think-hidden"
#define THINK_TAG_CODE "md-think-codeblock"
/* Balises posees par sed depuis llm.c (voir mdview.h). */
#define THINK_OPEN     "〔thinking〕"
#define THINK_CLOSE    "〔/thinking〕"
/* Meme chose, autre vocabulaire : certains modeles mettent leur
 * raisonnement dans le contenu courant, encadre de marques ASCII.
 * Elles designent les memes blocs et sont reconnues a la lecture. */
#define RAW_THINK_OPEN   "<think>"
#define RAW_THINK_CLOSE  "</think>"

/* ------------------------------------------------------------------ */
/* Tags                                                                */
/* ------------------------------------------------------------------ */

/* Cree le tag s'il n'existe pas deja dans la tag table du buffer. */
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
    else if (strcmp(name, THINK_TAG_BODY) == 0)
        tag = gtk_text_buffer_create_tag(buf, name, "foreground-rgba",
                                         &(GdkRGBA){ 0.55, 0.55, 0.6, 1.0 },
                                         NULL);
    else if (strcmp(name, THINK_TAG_HIDE) == 0)
        tag = gtk_text_buffer_create_tag(buf, name, "invisible", TRUE,
                                         NULL);
    else if (strcmp(name, THINK_TAG_CODE) == 0)
        tag = gtk_text_buffer_create_tag(
            buf, name, "family", "monospace", "background-rgba",
            &(GdkRGBA){ 0.5, 0.5, 0.5, 0.25 }, "foreground-rgba",
            &(GdkRGBA){ 0.55, 0.55, 0.6, 1.0 }, NULL);
    return tag;
}

/* Emission avec la pile courante (flags) + un tag de bloc optionnel. */
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

/* Ce caractere ouvre-t-il du markup dans l'etat courant ? */
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

/* Rendu inline recursif : `code`, gras, *italique*. Le souligné « _ »
 * est volontairement ignore (snake_case des identifiants : trop de faux
 * positifs dans les reponses techniques). */
static void
md_inline(GtkTextBuffer *buf, GtkTextIter *it, const char *s,
          gboolean bold, gboolean italic, gboolean code, const char *extra)
{
    while (*s != '\0') {
        /* Code inline : `…` (litteral, prioritaire, non imbricable). */
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

        /* Italique : *…* (un seul asterisque). */
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
            if (s == run) { /* marqueur sans fermeture : litteral */
                s++;
                continue;
            }
            md_emit(buf, it, run, (int)(s - run), bold, italic, code,
                    extra);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Blocs thinking                                                      */
/* ------------------------------------------------------------------ */

/* Une instance de bloc thinking vivante dans le buffer. */
typedef struct {
    GtkTextBuffer *buf;      /* reference                             */
    GtkTextMark   *start;    /* debut contenu, gravite gauche (ref.)  */
    GtkTextMark   *end;      /* fin contenu, gravite droite (ref.)    */
    GtkWidget     *btn;      /* bouton header (chevron + label)       */
    GtkWidget     *img;      /* icone chevron (swap pan-up/down)      */
    gboolean       open;     /* balise fermante pas encore rencontree */
    gboolean       expanded; /* etat visuel courant                   */
    guint          gen;      /* generation d'appartenance             */
    int            idx;      /* index dans la generation              */
} ThinkBlock;

/* Contexte par buffer (attache via g_object_set_data_full). */
typedef struct {
    GPtrArray *blocks;       /* ThinkBlock* (purge paresseuse)         */
    GArray    *gen_expanded; /* gboolean par index de bloc             */
    GArray    *gen_touched;  /* l'utilisateur a clique sur ce bloc ?   */
    GtkWidget *view;         /* ou ancre les boutons (non referencee)  */
    guint      gen;          /* generation courante                    */
    int        gen_count;    /* nb de blocs vus de la generation       */
    gboolean   in_fence;     /* etat de rendu : fence ``` ouverte      */
    gboolean   in_think;     /* etat de rendu : thinking ouvert        */
} ThinkCtx;

static void think_ctx_free(gpointer data); /* requis avant ctx_get (C23) */

static ThinkCtx *
think_ctx_get(GtkTextBuffer *buf)
{
    ThinkCtx *ctx = g_object_get_data(G_OBJECT(buf), THINK_CTX_KEY);

    if (ctx == NULL) {
        ctx = g_new0(ThinkCtx, 1);
        ctx->blocks = g_ptr_array_new();
        ctx->gen_expanded = g_array_new(FALSE, FALSE, sizeof(gboolean));
        ctx->gen_touched = g_array_new(FALSE, FALSE, sizeof(gboolean));
        g_object_set_data_full(G_OBJECT(buf), THINK_CTX_KEY, ctx,
                               (GDestroyNotify)think_ctx_free);
    }
    return ctx;
}

/* Libere une instance (marks et buffer references). */
static void
think_block_free(gpointer data)
{
    ThinkBlock *blk = data;

    g_object_unref(blk->start);
    g_object_unref(blk->end);
    g_object_unref(blk->buf);
    g_free(blk);
}

/* Retire les instances dont la zone de texte a ete effacee (leurs marks
 * sont deleted). Les boutons correspondants ont ete detruits par GTK
 * avec leur ancre. */
static void
think_purge(ThinkCtx *ctx)
{
    for (guint i = ctx->blocks->len; i > 0; i--) {
        ThinkBlock *blk = ctx->blocks->pdata[i - 1];

        if (gtk_text_mark_get_deleted(blk->start) ||
            gtk_text_mark_get_deleted(blk->end)) {
            think_block_free(blk);
            g_ptr_array_remove_index(ctx->blocks, i - 1);
        }
    }
}

static void
think_ctx_free(gpointer data)
{
    ThinkCtx *ctx = data;

    for (guint i = 0; i < ctx->blocks->len; i++)
        think_block_free(ctx->blocks->pdata[i]);
    g_ptr_array_free(ctx->blocks, TRUE);
    g_array_free(ctx->gen_expanded, TRUE);
    g_array_free(ctx->gen_touched, TRUE);
    g_free(ctx);
}

/* Applique ou retire le tag invisible sur la zone du bloc. */
static void
think_set_hidden(ThinkBlock *blk, gboolean hidden)
{
    GtkTextIter s, e;

    if (gtk_text_mark_get_deleted(blk->start) ||
        gtk_text_mark_get_deleted(blk->end))
        return;
    gtk_text_buffer_get_iter_at_mark(blk->buf, &s, blk->start);
    md_ensure_tag(blk->buf, THINK_TAG_HIDE);
    gtk_text_buffer_get_iter_at_mark(blk->buf, &e, blk->end);
    if (hidden)
        gtk_text_buffer_apply_tag_by_name(blk->buf, THINK_TAG_HIDE, &s, &e);
    else
        gtk_text_buffer_remove_tag_by_name(blk->buf, THINK_TAG_HIDE, &s,
                                           &e);
}

/* Synchronise l'icone du chevron sur l'etat (memes icones que le
 * selecteur de modele). */
static void
think_icon_update(ThinkBlock *blk)
{
    if (blk->img != NULL)
        gtk_image_set_from_icon_name(GTK_IMAGE(blk->img),
                                     blk->expanded ? "pan-up-symbolic"
                                                   : "pan-down-symbolic");
}

/* Clic sur le header : bascule ouvert/ferme. */
static void
think_toggle_cb(GtkButton *btn, gpointer data)
{
    ThinkBlock *blk = data;
    ThinkCtx   *ctx;

    (void)btn;
    ctx = g_object_get_data(G_OBJECT(blk->buf), THINK_CTX_KEY);
    if (ctx == NULL)
        return;
    think_purge(ctx);
    /* blk a pu etre purge (zone effacee entre-temps) : verifier qu'il
     * est toujours parmi les vivants. */
    for (guint i = 0; i < ctx->blocks->len; i++) {
        if (ctx->blocks->pdata[i] == blk) {
            blk->expanded = !blk->expanded;
            /* Persister pour les re-rendus de la meme generation. */
            if (blk->gen == ctx->gen && blk->idx >= 0 &&
                (guint)blk->idx < ctx->gen_expanded->len) {
                g_array_index(ctx->gen_expanded, gboolean,
                              (guint)blk->idx) = blk->expanded;
                g_array_index(ctx->gen_touched, gboolean,
                              (guint)blk->idx) = TRUE;
            }
            think_set_hidden(blk, !blk->expanded);
            think_icon_update(blk);
            return;
        }
    }
}

/* Construit le bouton header : chevron + label, flat, discret. */
static GtkWidget *
think_header_new(ThinkBlock *blk)
{
    GtkWidget *btn = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *img = gtk_image_new_from_icon_name("pan-down-symbolic");
    GtkWidget *lbl = gtk_label_new(_(THINK_LABEL));

    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_add_css_class(lbl, "dim-label");
    gtk_box_append(GTK_BOX(box), img);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_button_set_child(GTK_BUTTON(btn), box);
    blk->img = img;
    g_signal_connect(btn, "clicked", G_CALLBACK(think_toggle_cb), blk);
    return btn;
}

/* Rencontre de la balise ouvrante : ligne neuve si besoin, ancre +
 * bouton, puis marks de contenu (start left / end right, cote a cote :
 * tout le contenu a venir s'insere entre eux et le mark de fin suit la
 * croissance). Reutilise l'etat de generation existant s'il y a deja eu
 * un re-rendu de ce bloc. */
static void
think_open(GtkTextBuffer *buf, GtkTextIter *it, ThinkCtx *ctx)
{
    GtkTextChildAnchor *anchor;
    ThinkBlock         *blk;
    gboolean            untouched = FALSE;

    if (!gtk_text_iter_starts_line(it))
        gtk_text_buffer_insert(buf, it, "\n", 1);

    anchor = gtk_text_buffer_create_child_anchor(buf, it);
    gtk_text_buffer_insert(buf, it, "\n", 1);

    blk = g_new0(ThinkBlock, 1);
    blk->buf = g_object_ref(buf);
    blk->start = gtk_text_buffer_create_mark(buf, NULL, it, TRUE);
    blk->end = gtk_text_buffer_create_mark(buf, NULL, it, FALSE);
    g_object_ref(blk->start);
    g_object_ref(blk->end);
    blk->open = TRUE;
    blk->expanded = TRUE; /* ouvert pendant le stream */
    blk->gen = ctx->gen;
    blk->idx = ctx->gen_count++;

    if ((guint)blk->idx < ctx->gen_expanded->len) {
        /* Case existante : respecter l'etat persiste (re-rendu). */
        blk->expanded = g_array_index(ctx->gen_expanded, gboolean,
                                      (guint)blk->idx);
    } else {
        g_array_append_val(ctx->gen_expanded, blk->expanded);
        g_array_append_val(ctx->gen_touched, untouched);
    }

    blk->btn = think_header_new(blk);
    g_ptr_array_add(ctx->blocks, blk);
    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(ctx->view), blk->btn,
                                      anchor);
    /* Si l'etat persiste dit replie : poser le masque des maintenant,
     * les insertions suivantes heriteront du tag invisible. */
    think_set_hidden(blk, !blk->expanded);
    think_icon_update(blk);
}

/* Rencontre de la balise fermante : le bloc ouvert le plus recent se
 * ferme. Auto-repli si l'utilisateur n'a jamais touche ce bloc. */
static void
think_close(ThinkCtx *ctx)
{
    for (guint i = ctx->blocks->len; i > 0; i--) {
        ThinkBlock *blk = ctx->blocks->pdata[i - 1];

        if (!blk->open || blk->gen != ctx->gen)
            continue;
        blk->open = FALSE;
        /* Figer la borne de fin : un mark right-gravity suit le flux
         * d'insertion jusqu'au bout du buffer (chaque emission du
         * rendu arrive exactement a sa position). On le remplace par
         * un mark left-gravity pose ici : immobile pour toujours. */
        {
            GtkTextIter e;

            gtk_text_buffer_get_iter_at_mark(blk->buf, &e, blk->end);
            /* delete_mark : retire le mark de la table du buffer ET
             * libere notre reference — un simple g_object_unref
             * laisserait un mark orphelin trainer dans la table. */
            gtk_text_buffer_delete_mark(blk->buf, blk->end);
            g_object_unref(blk->end);
            blk->end = gtk_text_buffer_create_mark(blk->buf, NULL, &e,
                                                   TRUE);
            g_object_ref(blk->end);
        }
        if ((guint)blk->idx < ctx->gen_expanded->len) {
            if (!g_array_index(ctx->gen_touched, gboolean,
                               (guint)blk->idx)) {
                g_array_index(ctx->gen_expanded, gboolean,
                              (guint)blk->idx) = FALSE;
                blk->expanded = FALSE;
            } else {
                blk->expanded = g_array_index(ctx->gen_expanded, gboolean,
                                              (guint)blk->idx);
            }
        } else {
            blk->expanded = FALSE;
        }
        think_set_hidden(blk, !blk->expanded);
        think_icon_update(blk);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Rendu                                                               */
/* ------------------------------------------------------------------ */

/* Premiere occurrence, a partir de p, de l'une des deux formes d'une
 * meme balise. Rend aussi la longueur de la forme trouvee : les deux
 * n'ont pas la meme taille et l'appelant doit avaler exactement la
 * marque rencontree, pas une longueur supposee. */
static char *
find_mark(const char *p, const char *m1, const char *m2, gsize *len)
{
    char *a = strstr(p, m1);
    char *b = strstr(p, m2);

    if (a == NULL && b == NULL)
        return NULL;
    if (b == NULL || (a != NULL && a <= b)) {
        *len = strlen(m1);
        return a;
    }
    *len = strlen(m2);
    return b;
}

/* Rendu d'une ligne (hors fence) avec detection des balises thinking.
 * L'etat in_think vit dans ctx (survit d'un appel a l'autre : rendu
 * incrementel). Astuce zero-copy : terminaison temporaire des segments
 * (les chaines viennent de g_strsplit, donc modifiables) pour garder le
 * rendu inline complet autour des balises. */
static void
md_line(GtkTextBuffer *buf, GtkTextIter *it, ThinkCtx *ctx, char *line)
{
    char *p = line;

    while (*p != '\0') {
        char  *open, *close;
        gsize  olen = 0, clen = 0;

        open  = find_mark(p, THINK_OPEN, RAW_THINK_OPEN, &olen);
        close = find_mark(p, THINK_CLOSE, RAW_THINK_CLOSE, &clen);

        /* Pas de vue attachee ou pas de balise : inline ordinaire. */
        if (ctx->view == NULL || (open == NULL && close == NULL)) {
            md_inline(buf, it, p, FALSE, FALSE, FALSE,
                      ctx->in_think ? THINK_TAG_BODY : NULL);
            return;
        }

        if (open != NULL && (close == NULL || open < close)) {
            /* Segment avant l'ouverture. */
            if (open > p) {
                char saved = *open;

                *open = '\0';
                md_inline(buf, it, p, FALSE, FALSE, FALSE,
                          ctx->in_think ? THINK_TAG_BODY : NULL);
                *open = saved;
            }
            p = open + olen;
            if (*p == ' ')
                p++;
            think_open(buf, it, ctx);
            ctx->in_think = TRUE;
            continue;
        }

        /* Fermeture : avaler aussi l'espace colle devant. */
        {
            char *seg_end = close;
            char  saved;

            if (seg_end > p && seg_end[-1] == ' ')
                seg_end--;
            if (seg_end > p) {
                saved = *seg_end;
                *seg_end = '\0';
                md_inline(buf, it, p, FALSE, FALSE, FALSE,
                          ctx->in_think ? THINK_TAG_BODY : NULL);
                *seg_end = saved;
            }
            p = close + clen;
            think_close(ctx);
            ctx->in_think = FALSE;
            continue;
        }
    }
}

/* Traitement d'un lot de texte : decoupe en lignes, applique l'etat
 * persistant (fence/thinking) du contexte. Les elements completes de
 * la decomposition sont toujours rendus ; le fragment final (sans \n)
 * ne l'est que si flush (fin de stream). */
static void
md_feed(GtkTextBuffer *buf, GtkTextIter *iter, ThinkCtx *ctx,
        const char *text, gboolean flush)
{
    char   **lines = g_strsplit(text, "\n", 0);
    guint    n = lines != NULL ? g_strv_length(lines) : 0;
    guint    complete = n > 0 ? n - 1 : 0; /* le dernier = fragment */

    if (flush && n > 0 && lines[n - 1][0] != '\0')
        complete = n; /* fin de stream : le fragment aussi */

    for (guint i = 0; i < complete; i++) {
        char *line = lines[i];
        int   len = (int)strlen(line);

        /* Fence de bloc de code (non fermee = jusqu'a la fin — cas
         * streaming). La ligne fence elle-meme est avalee.
         * Un thinking DANS une fence reste du code litteral. */
        if (len >= 3 && strncmp(line, "```", 3) == 0) {
            ctx->in_fence = !ctx->in_fence;
            continue;
        }
        if (ctx->in_fence) {
            md_emit(buf, iter, line, len, FALSE, FALSE, TRUE,
                    ctx->in_think ? THINK_TAG_CODE : "md-codeblock");
            gtk_text_buffer_insert(buf, iter, "\n", 1);
            continue;
        }

        /* En plein thinking : prose attenue ; ni titres ni quotes ni
         * bullets speciaux (de la prose, comme ZED). */
        if (ctx->in_think) {
            md_line(buf, iter, ctx, line);
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

        /* Liste a puce : "- ", "* ", "+ ". */
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

        /* Paragraphe ordinaire (peut contenir la balise ouvrante). */
        md_line(buf, iter, ctx, line);
        gtk_text_buffer_insert(buf, iter, "\n", 1);
    }
    g_strfreev(lines);
}

void
md_insert(GtkTextBuffer *buf, GtkTextIter *iter, const char *md)
{
    ThinkCtx *ctx = think_ctx_get(buf);

    /* Rendu COMPLET : etat et indexation repartent de zero (les blocs
     * detruits avec l'ancien texte seront purges ci-dessous). */
    think_purge(ctx);
    ctx->gen_count = 0;
    ctx->in_fence = FALSE;
    ctx->in_think = FALSE;
    md_feed(buf, iter, ctx, md, TRUE);
}

void
md_insert_append(GtkTextBuffer *buf, GtkTextIter *iter,
                 const char *text, gsize len, gboolean flush)
{
    ThinkCtx *ctx = think_ctx_get(buf);
    char     *copy;

    /* Rendu INCREMENTAL : l'etat (fence/thinking) et la numerotation
     * des blocs continuent exactement ou ils en etaient. Le texte n'est
     * pas nul-terminal : copie bornee. */
    if (len == 0)
        return;
    copy = g_strndup(text, len);
    md_feed(buf, iter, ctx, copy, flush);
    g_free(copy);
}

void
md_thinking_attach(GtkTextBuffer *buf, GtkWidget *view)
{
    ThinkCtx *ctx = think_ctx_get(buf);

    ctx->view = view;
}

void
md_thinking_reset(GtkTextBuffer *buf)
{
    ThinkCtx *ctx = think_ctx_get(buf);

    ctx->gen++;
    ctx->gen_count = 0; /* nouvelle numerotation pour la generation */
    ctx->in_fence = FALSE;
    ctx->in_think = FALSE;
    g_array_set_size(ctx->gen_expanded, 0);
    g_array_set_size(ctx->gen_touched, 0);
    think_purge(ctx);
}
