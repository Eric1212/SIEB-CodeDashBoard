/*
 * DiffBar : barre transparente sur la scrollbar montrant les changements
 * dirty (carte de tout le fichier). Rendu cairo (snapshot GTK4).
 *
 * Classification intra-ligne : ajout = vert, retrait = rouge, remplacement
 * = orange (via test de sous-séquence entre l'ancienne et la nouvelle
 * ligne).
 */

#include "diffbar.h"
#include <string.h>

#define BAR_WIDTH   10.0
#define COLOR_ADD   0.25, 0.85, 0.30, 0.75   /* vert   : ajout  */
#define COLOR_DEL   0.90, 0.30, 0.28, 0.75   /* rouge  : retrait */
#define COLOR_MOD   0.96, 0.62, 0.18, 0.80   /* orange : remplacement */

/* Une opération de l'alignement (après inversion). a_idx/b_idx sont les
 * indices absolus dans les tableaux de lignes de la référence / du buffer. */
typedef struct {
    guchar type;  /* 'k' garde (lignes égales) / 'a' ajout / 'r' retrait */
    guint  a_idx; /* ligne dans la référence (utile pour 'k' et 'r') */
    guint  b_idx; /* ligne dans le buffer (utile pour 'k' et 'a') */
} Op;

/* ------------------------------------------------------------------ */
/* Diff ligne par ligne + classification caractère                     */
/* ------------------------------------------------------------------ */

/* Découpe en lignes, ignore un \n final. Renvoie NULL + *n=0 si vide. */
static gchar **
split_lines(const char *s, gsize *n)
{
    gchar  *dup;
    gsize   len;
    gchar **lines;

    if (s == NULL || s[0] == '\0') {
        *n = 0;
        return NULL;
    }
    dup = g_strdup(s);
    len = strlen(dup);
    if (len > 0 && dup[len - 1] == '\n')
        dup[len - 1] = '\0';
    lines = g_strsplit(dup, "\n", -1);
    g_free(dup);
    *n = g_strv_length(lines);
    return lines;
}

/* a est-il une sous-séquence de b ? (on peut obtenir a en supprimant des
 * caractères de b). */
static gboolean
is_subsequence(const char *a, const char *b)
{
    const char *pa = a;
    const char *pb;

    for (pb = b; *pb != '\0'; pb++)
        if (*pa == *pb)
            pa++;
    return *pa == '\0';
}

/* Classe une ligne remplacée : ajout (vert), retrait (rouge), ou
 * remplacement (orange). Renvoie DIFF_ADD / DIFF_DEL / DIFF_MOD. */
static int
classify_line(const char *old, const char *new)
{
    if (strcmp(old, new) == 0)
        return -1; /* identique : pas de marqueur */
    if (is_subsequence(old, new))
        return DIFF_ADD;
    if (is_subsequence(new, old))
        return DIFF_DEL;
    return DIFF_MOD;
}

static void
add_range(GPtrArray *out, guint line_start, guint count,
          guint total, int type)
{
    SiebdDiffRange *r;

    if (count == 0 || total == 0)
        return;
    r = g_malloc(sizeof(*r));
    r->pos = (double)line_start / (double)total;
    r->size = (double)count / (double)total;
    r->type = type;
    g_ptr_array_add(out, r);
}

/* Trop de lignes au milieu pour le DP LCS : repli grossier mais sûr
 * (tout le milieu = remplacements orange). */
#define LCS_MAX_CELLS 5000000u

void
cdb_diff_compute(const char *saved, const char *current,
                   GPtrArray *out, guint *total_lines)
{
    gchar **a, **b;          /* a = référence, b = buffer */
    gsize   na, nb;
    gsize   p, s;            /* préfixe / suffixe communs */
    gsize   la, lb;          /* taille du milieu (à différencier) */
    gint   *dp;
    GArray *ops;             /* suite d'Op */
    gsize   i, j, k;

    a = split_lines(saved, &na);
    b = split_lines(current, &nb);
    *total_lines = nb;

    /* Préfixe commun. */
    p = 0;
    while (p < na && p < nb && strcmp(a[p], b[p]) == 0)
        p++;

    /* Suffixe commun (sans chevaucher le préfixe). */
    s = 0;
    while (s < na - p && s < nb - p
           && strcmp(a[na - 1 - s], b[nb - 1 - s]) == 0)
        s++;

    la = na - p - s;
    lb = nb - p - s;

    /* Rien à différencier. */
    if (la == 0 && lb == 0)
        goto done;

    /* Repli sur gros milieux (évite l'explosion mémoire du DP). */
    if ((la + 1) * (lb + 1) > LCS_MAX_CELLS) {
        add_range(out, p, lb, nb, DIFF_MOD);
        add_range(out, p, la, nb, DIFF_MOD);
        goto done;
    }

    dp = g_new0(gint, (la + 1) * (lb + 1));

    for (i = 1; i <= la; i++)
        for (j = 1; j <= lb; j++) {
            if (strcmp(a[p + i - 1], b[p + j - 1]) == 0)
                dp[i * (lb + 1) + j] = dp[(i - 1) * (lb + 1) + (j - 1)] + 1;
            else if (dp[(i - 1) * (lb + 1) + j] > dp[i * (lb + 1) + (j - 1)])
                dp[i * (lb + 1) + j] = dp[(i - 1) * (lb + 1) + j];
            else
                dp[i * (lb + 1) + j] = dp[i * (lb + 1) + (j - 1)];
        }

    /* Reconstruction (en ordre inverse) des opérations. */
    ops = g_array_new(FALSE, FALSE, sizeof(Op));
    i = la;
    j = lb;
    while (i > 0 && j > 0) {
        if (strcmp(a[p + i - 1], b[p + j - 1]) == 0
            && dp[i * (lb + 1) + j] == dp[(i - 1) * (lb + 1) + (j - 1)] + 1) {
            Op op = { 'k', p + i - 1, p + j - 1 };
            g_array_append_val(ops, op);
            i--; j--;
        } else if (dp[i * (lb + 1) + j] == dp[(i - 1) * (lb + 1) + j]) {
            Op op = { 'r', p + i - 1, 0 };
            g_array_append_val(ops, op);
            i--;
        } else {
            Op op = { 'a', 0, p + j - 1 };
            g_array_append_val(ops, op);
            j--;
        }
    }
    while (i > 0) {
        Op op = { 'r', p + i - 1, 0 };
        g_array_append_val(ops, op);
        i--;
    }
    while (j > 0) {
        Op op = { 'a', 0, p + j - 1 };
        g_array_append_val(ops, op);
        j--;
    }
    g_free(dp);

    /* Inversion. */
    for (k = 0; k < ops->len / 2; k++) {
        Op tmp = g_array_index(ops, Op, k);

        g_array_index(ops, Op, k) = g_array_index(ops, Op, ops->len - 1 - k);
        g_array_index(ops, Op, ops->len - 1 - k) = tmp;
    }

    /* Émission. On parcourt les runs. Une run 'r' (ou 'a') suivie
     * immédiatement de la run opposée = un bloc remplacé (l'utilisateur a
     * modifié la ligne, pas juste ajouté/retiré) -> classification
     * caractère par caractère. */
    {
        guint cur_b = p;
        gsize  oi = 0;

        while (oi < ops->len) {
            Op     op = g_array_index(ops, Op, oi);
            guchar t = op.type;
            gsize  cnt, nxt;

            if (t == 'k') {
                cur_b++;
                oi++;
                continue;
            }

            /* Longueur de la run courante. */
            cnt = 1;
            while (oi + cnt < ops->len
                   && g_array_index(ops, Op, oi + cnt).type == t)
                cnt++;
            nxt = oi + cnt; /* début de la run suivante */

            if (nxt < ops->len && g_array_index(ops, Op, nxt).type != 'k'
                && g_array_index(ops, Op, nxt).type != t) {
                /* La run suivante est opposée -> bloc remplacé. */
                gsize  on = nxt;
                guchar nt = g_array_index(ops, Op, nxt).type;
                gsize  ocnt = 1;
                Op    *r_ops, *a_ops; /* pointeurs vers les runs */
                gsize  r_cnt, a_cnt;
                gsize  kk;

                while (on + ocnt < ops->len
                       && g_array_index(ops, Op, on + ocnt).type == nt)
                    ocnt++;

                if (t == 'r') {
                    r_ops = &g_array_index(ops, Op, oi);
                    a_ops = &g_array_index(ops, Op, on);
                    r_cnt = cnt;
                    a_cnt = ocnt;
                } else {
                    a_ops = &g_array_index(ops, Op, oi);
                    r_ops = &g_array_index(ops, Op, on);
                    a_cnt = cnt;
                    r_cnt = ocnt;
                }

                /* Pairing : classification des lignes remplacées. */
                for (kk = 0; kk < a_cnt; kk++) {
                    int cat;

                    if (kk < r_cnt) {
                        cat = classify_line(a[r_ops[kk].a_idx],
                                            b[a_ops[kk].b_idx]);
                    } else {
                        cat = DIFF_ADD; /* surplus de lignes nouvelles */
                    }
                    if (cat >= 0)
                        add_range(out, a_ops[kk].b_idx, 1, nb, cat);
                }
                /* Surplus de lignes retirées (non appariées) : rouge. */
                if (r_cnt > a_cnt)
                    add_range(out, cur_b, r_cnt - a_cnt, nb, DIFF_DEL);

                cur_b += a_cnt;
                oi = on + ocnt;
            } else if (t == 'a') {
                /* Ajout pur -> vert. */
                add_range(out, cur_b, cnt, nb, DIFF_ADD);
                cur_b += cnt;
                oi += cnt;
            } else {
                /* Retrait pur -> rouge. */
                add_range(out, cur_b, cnt, nb, DIFF_DEL);
                oi += cnt;
            }
        }
    }
    g_array_free(ops, TRUE);

done:
    g_strfreev(a);
    g_strfreev(b);
}

/* ------------------------------------------------------------------ */
/* Widget                                                              */
/* ------------------------------------------------------------------ */

struct _CdbDiffBar {
    GtkWidget  parent_instance;
    GPtrArray *ranges;   /* SiebdDiffRange* (g_free) */
    guint      total;
};

G_DEFINE_TYPE(CdbDiffBar, cdb_diff_bar, GTK_TYPE_WIDGET)

static void
cdb_diff_bar_finalize(GObject *object)
{
    CdbDiffBar *bar = CDB_DIFF_BAR(object);

    g_ptr_array_free(bar->ranges, TRUE);
    G_OBJECT_CLASS(cdb_diff_bar_parent_class)->finalize(object);
}

static void
cairo_round_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    double rad = MIN(r, MIN(w / 2.0, h / 2.0));

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + rad, y + rad, rad, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + w - rad, y + rad, rad, 1.5 * G_PI, 2 * G_PI);
    cairo_arc(cr, x + w - rad, y + h - rad, rad, 0, 0.5 * G_PI);
    cairo_arc(cr, x + rad, y + h - rad, rad, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

static void
cdb_diff_bar_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    CdbDiffBar *bar = CDB_DIFF_BAR(widget);
    double        w = gtk_widget_get_width(widget);
    double        h = gtk_widget_get_height(widget);
    cairo_t      *cr;
    guint         i;

    if (h <= 0 || bar->total == 0 || bar->ranges->len == 0)
        return;

    {
        graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, w, h);

        cr = gtk_snapshot_append_cairo(snapshot, &bounds);
    }

    for (i = 0; i < bar->ranges->len; i++) {
        SiebdDiffRange *r = g_ptr_array_index(bar->ranges, i);
        double y = r->pos * h;
        double rh = MAX(1.0, r->size * h);

        if (r->type == DIFF_DEL)
            cairo_set_source_rgba(cr, COLOR_DEL);
        else if (r->type == DIFF_MOD)
            cairo_set_source_rgba(cr, COLOR_MOD);
        else
            cairo_set_source_rgba(cr, COLOR_ADD);
        cairo_round_rect(cr, 0.5, y, w - 1.0, rh, w / 2.0);
        cairo_fill(cr);
    }

    cairo_destroy(cr);
}

static void
cdb_diff_bar_class_init(CdbDiffBarClass *klass)
{
    GObjectClass   *gobject_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gobject_class->finalize = cdb_diff_bar_finalize;
    widget_class->snapshot = cdb_diff_bar_snapshot;
}

static void
cdb_diff_bar_init(CdbDiffBar *bar)
{
    bar->ranges = g_ptr_array_new_with_free_func(g_free);
    bar->total = 0;
    gtk_widget_set_size_request(GTK_WIDGET(bar), BAR_WIDTH, -1);
    /* Ne pas intercepter les clics : la scrollbar reste opérationnelle. */
    gtk_widget_set_can_target(GTK_WIDGET(bar), FALSE);
}

GtkWidget *
cdb_diff_bar_new(void)
{
    return GTK_WIDGET(g_object_new(CDB_TYPE_DIFF_BAR, NULL));
}

void
cdb_diff_bar_set_ranges(CdbDiffBar *bar, GPtrArray *ranges, guint total_lines)
{
    g_ptr_array_set_size(bar->ranges, 0);
    for (guint i = 0; i < ranges->len; i++)
        g_ptr_array_add(bar->ranges, g_memdup2(g_ptr_array_index(ranges, i),
                                               sizeof(SiebdDiffRange)));
    bar->total = total_lines;
    gtk_widget_queue_draw(GTK_WIDGET(bar));
}
