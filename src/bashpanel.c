/*
 * bashpanel.c : panneau « Bash » — GtkNotebook de 1 à 10 terminaux VTE.
 *
 * Chaque onglet est un VteTerminal avec un vrai shell ($SHELL, fallback
 * /bin/bash) lancé dans le projet courant (sinon $HOME).
 *
 * DECOUPLAGE (loi : le layout ne fait QUE rendre). Le notebook et ses
 * terminaux sont un backend PERMANENT (cdb_first_panel, ref forte) cree une
 * fois par process par bash_panel_init() : les outils cdb_bash executent
 * sans aucune tuile « bash » affichée. bash_panel_new() n'est qu'une VUE qui
 * EMPRUNTE ce notebook ; le retirer du layout le dé-arente mais ne tue rien
 * (sonde VTE : un terminal unrealized garde son PTY et sa sortie). Seuls les
 * ACTES humains ferment un terminal : le « x » et le reset ALLOW+ passent
 * par gtk_notebook_remove_page(), qui finalise le VteTerminal et ferme son
 * PTY (SIGHUP).
 */
#include "bashpanel.h"
#include "i18n.h"
#include <string.h>
#include <vte/vte.h>

#define BASH_TAB_MIN 1
#define BASH_TAB_MAX 10

typedef struct {
    GtkWidget *notebook;
    GtkWidget *add_btn;
    int        count;   /* onglets actifs */
    GListStore *roots;  /* pour résoudre le projet courant au spawn */
    GHashTable *multi_paths;
    guint      first_spawn_idle; /* id du différé du 1er spawn (0 = aucun) */
} BashPanel;

/* ------------------------------------------------ */
/* Onglets                                           */
/* ------------------------------------------------ */

static void bash_panel_add_tab(BashPanel *p);

static void
bash_panel_update(BashPanel *p)
{
    gtk_widget_set_sensitive(p->add_btn, p->count < BASH_TAB_MAX);
}


/* Renumérote les titres « bash N » après fermeture. */
static void
bash_panel_renumber(BashPanel *p)
{
    GtkNotebook *nb = GTK_NOTEBOOK(p->notebook);

    for (int i = 0; i < p->count; i++) {
        GtkWidget *tab;
        GtkWidget *label;

        tab = gtk_notebook_get_tab_label(nb, gtk_notebook_get_nth_page(nb, i));
        label = gtk_widget_get_first_child(tab);
        if (GTK_IS_LABEL(label)) {
            char buf[16];

            g_snprintf(buf, sizeof(buf), "%d", i);
            gtk_label_set_text(GTK_LABEL(label), buf);
        }
    }
}

static void
on_close_tab(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    GtkWidget   *term = data;
    GtkNotebook *nb;
    BashPanel   *p;
    int          page;
    gboolean     was_last;

    nb = GTK_NOTEBOOK(gtk_widget_get_ancestor(term, GTK_TYPE_NOTEBOOK));
    if (nb == NULL)
        return;
    p = g_object_get_data(G_OBJECT(nb), "bash-panel");
    if (p == NULL || p->count <= 0)
        return;

    was_last = (p->count <= BASH_TAB_MIN);
    page = gtk_notebook_page_num(nb, term);
    if (page < 0)
        return;
    /* Le terminal meurt avec la page : le PTY est fermé (SIGHUP). */
    gtk_notebook_remove_page(nb, page);
    p->count--;

    if (was_last) {
        /* Dernier onglet fermé : on en recrée un FRAIS immédiatement —
         * il résout le projet courant au spawn (comme un nouvel onglet
         * après avoir changé la sélection). */
        bash_panel_add_tab(p);
        return;
    }
    bash_panel_renumber(p);
    bash_panel_update(p);
}

/* Label d'onglet : le n° de terminal (0-based, tel que l'outil le reçoit)
 * + bouton fermer (data = le terminal). */
static GtkWidget *
bash_tab_label(GtkWidget *term, int index)
{
    GtkWidget *box;
    GtkWidget *label;
    GtkWidget *close_btn;
    char       buf[16];

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_snprintf(buf, sizeof(buf), "%d", index);
    label = gtk_label_new(buf);
    gtk_box_append(GTK_BOX(box), label);

    close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close_btn, "flat");
    gtk_widget_add_css_class(close_btn, "cdb-flat");
    gtk_widget_add_css_class(close_btn, "tile-menu");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_tab), term);
    gtk_box_append(GTK_BOX(box), close_btn);
    return box;
}


/* ------------------------------------------------ */
/* Terminal                                          */
/* ------------------------------------------------ */

static void
on_spawn_finished(VteTerminal G_GNUC_UNUSED *term, GPid G_GNUC_UNUSED pid,
                  GError *err, gpointer G_GNUC_UNUSED user_data)
{
    if (err != NULL) {
        g_printerr(_("CDB: bash spawn failed: %s\n"), err->message);
        g_error_free(err);
    }
}

static void
bash_tab_spawn(BashPanel *p, VteTerminal *term)
{
    const char *shell = g_getenv("SHELL");
    char       *argv[2];
    char       *proj = roots_current_project(p->roots, p->multi_paths);
    const char *dir = (proj != NULL && proj[0] != '\0') ? proj
                                                        : g_get_home_dir();

    if (shell == NULL || shell[0] == '\0')
        shell = "/bin/bash";
    argv[0] = (char *)shell;
    argv[1] = NULL;

    vte_terminal_spawn_async(term, VTE_PTY_DEFAULT, dir, argv,
                             NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, -1,
                             NULL, on_spawn_finished, NULL);
    g_free(proj);
}

static void
bash_panel_add_tab(BashPanel *p)
{
    GtkWidget *term;
    int        index;

    if (p->count >= BASH_TAB_MAX)
        return;
    index = p->count;
    term = vte_terminal_new();
    /* Réservoir paginable pour la boucle agentique : 100k lignes. */
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(term), 100000);
    bash_tab_spawn(p, VTE_TERMINAL(term));
    gtk_notebook_append_page(GTK_NOTEBOOK(p->notebook), term,
                             bash_tab_label(term, index));
    p->count++;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(p->notebook), -1);
    bash_panel_update(p);
}

static void
on_add_tab_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    bash_panel_add_tab(data);
}

/* ------------------------------------------------ */
/* Panneau                                           */
/* ------------------------------------------------ */

static GtkWidget *cdb_first_panel = NULL; /* backend PERMANENT (ref forte) */

/* Un panneau bash est-il disponible pour un appel d'outil ? */
gboolean
bash_panel_exec_tab_possible(void)
{
    return cdb_first_panel != NULL &&
        g_object_get_data(G_OBJECT(cdb_first_panel), "bash-panel") != NULL;
}

/* Reset d'un onglet : remplace le terminal par un FRAIS au même index —
 * l'équivalent programmatique du clic « x » suivi d'un nouvel onglet,
 * résolu dans le projet courant au spawn. Usager : mode AllowPlus, où
 * chaque commande doit repartir d'un shell propre. */
void
bash_panel_reset_tab(guint index)
{
    BashPanel   *p;
    GtkNotebook *nb;
    GtkWidget   *old_term;
    GtkWidget   *new_term;
    int          pos;

    if (cdb_first_panel == NULL)
        return;
    p = g_object_get_data(G_OBJECT(cdb_first_panel), "bash-panel");
    if (p == NULL || index >= (guint)p->count)
        return;

    nb = GTK_NOTEBOOK(p->notebook);
    pos = (int)index;
    old_term = gtk_notebook_get_nth_page(nb, pos);
    if (old_term == NULL)
        return;

    /* Onglet neuf inséré À LA MÊME PLACE, puis l'ancien retiré : le
     * PTY de l'ancien meurt avec sa page (SIGHUP), comme sous « x ». */
    new_term = vte_terminal_new();
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(new_term), 100000);
    bash_tab_spawn(p, VTE_TERMINAL(new_term));
    gtk_notebook_insert_page(nb, new_term,
                             bash_tab_label(new_term, pos), pos);
    p->count++;
    gtk_notebook_remove_page(nb, pos + 1);
    p->count--;

    bash_panel_renumber(p);
    bash_panel_update(p);
}

gboolean
bash_panel_exec_tab(guint index, const char *command)
{
    BashPanel *p;
    GtkWidget *page;
    char      *line;

    if (cdb_first_panel == NULL)
        return FALSE; /* aucun panneau bash à l'écran */
    p = g_object_get_data(G_OBJECT(cdb_first_panel), "bash-panel");
    if (p == NULL || index >= (guint)p->count)
        return FALSE;

    page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(cdb_first_panel),
                                     (int)index);
    if (page == NULL || !VTE_IS_TERMINAL(page))
        return FALSE;

    /* La commande EXACTE, rien qu'elle : tapée au clavier du shell,
     * comme si Éric la saisissait lui-même. Zéro plomberie. */
    line = g_strdup_printf("%s\n", command != NULL ? command : "true");
    vte_terminal_feed_child(VTE_TERMINAL(page), line, -1);
    g_free(line);
    return TRUE;
}

/* Point orange sur l'onglet N : une commande d'outil y tourne encore.
 * Inséré APRÈS le label « bash N » (avant le bouton fermer) ; retiré à
 * la fin du poll, ou à la fermeture de l'onglet (le label meurt avec). */
void
bash_panel_set_busy(guint index, gboolean busy)
{
    BashPanel *p;
    GtkWidget *page;
    GtkWidget *tab;
    GtkWidget *w;

    if (cdb_first_panel == NULL)
        return;
    p = g_object_get_data(G_OBJECT(cdb_first_panel), "bash-panel");
    if (p == NULL)
        return;
    page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(cdb_first_panel),
                                     (int)index);
    if (page == NULL)
        return;
    tab = gtk_notebook_get_tab_label(GTK_NOTEBOOK(cdb_first_panel), page);
    if (tab == NULL)
        return;

    for (w = gtk_widget_get_first_child(tab); w != NULL;
         w = gtk_widget_get_next_sibling(w)) {
        if (g_strcmp0(gtk_widget_get_name(w), "cdb-busy-dot") == 0) {
            if (!busy)
                gtk_box_remove(GTK_BOX(tab), w);
            return;
        }
    }
    if (busy) {
        GtkWidget *dot = gtk_label_new("●");
        GtkWidget *label = gtk_widget_get_first_child(tab);

        gtk_widget_set_name(dot, "cdb-busy-dot");
        gtk_widget_add_css_class(dot, "cdb-busy-dot");
        /* Entre le label « bash N » et le bouton fermer. */
        gtk_box_insert_child_after(GTK_BOX(tab), dot, label);
    }
}

/* Terminal de l'onglet N, ou NULL. */
static VteTerminal *
bash_panel_term(guint index)
{
    BashPanel *p;
    GtkWidget *page;

    if (cdb_first_panel == NULL)
        return NULL;
    p = g_object_get_data(G_OBJECT(cdb_first_panel), "bash-panel");
    if (p == NULL || index >= (guint)p->count)
        return NULL;
    page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(cdb_first_panel),
                                     (int)index);
    return (page != NULL && VTE_IS_TERMINAL(page))
        ? VTE_TERMINAL(page) : NULL;
}

gboolean
bash_panel_term_alive(guint index)
{
    return bash_panel_term(index) != NULL;
}

/* Le shell de l'onglet N a-t-il TERMINÉ son spawn ? vte_terminal_spawn_async
 * est asynchrone : entre add_tab() et le callback, get_pty() retourne NULL —
 * toute commande feed_child pendant cette fenêtre part dans le vide.
 * Prêt = terminal existant ET PTY attaché. */
gboolean
bash_panel_term_ready(guint index)
{
    VteTerminal *term = bash_panel_term(index);

    return term != NULL && vte_terminal_get_pty(term) != NULL;
}

/* Ligne du BAS du document selon le curseur : coordonnée ABSOLUE dans
 * le buffer (mesuré : row=5001 après 5000 lignes générées ; le curseur
 * ne dépend ni du contenu affiché ni du scroll — idée d'Éric). */
static glong
bash_panel_bottom(guint index)
{
    VteTerminal *term = bash_panel_term(index);
    glong        col = 0, row = 0;

    if (term == NULL)
        return -1;
    vte_terminal_get_cursor_position(term, &col, &row);
    return row;
}

/* Plage du buffer en coordonnées ABSOLUES (mesuré empiriquement : même
 * référentiel que get_cursor_position ; négatif = clamp VTE). Fin de
 * plage = bas du document (curseur) ; lignes paddées aux colonnes. */
static gchar *
bash_panel_range_text(guint index, glong start_row)
{
    VteTerminal *term = bash_panel_term(index);
    gsize        len = 0;
    glong        bottom;

    if (term == NULL)
        return NULL;
    bottom = bash_panel_bottom(index);
    if (bottom < 0)
        return NULL;
    return vte_terminal_get_text_range_format(
        term, VTE_FORMAT_TEXT,
        start_row, 0,
        bottom + 1, /* inclusif : jusqu'au prompt final */
        (long)vte_terminal_get_column_count(term),
        &len);
}

/* Texte intégral du buffer (recette d'Éric, mesurée) : début = -1,
 * clampé par VTE au début réelle du buffer ; fin = curseur. Capte tout
 * l'historique SANS les 100k lignes de padding du scrollback configuré.
 * get_text_format, lui, ne rend que l'écran VISIBLE. */
static gchar *
bash_panel_full_text(guint index)
{
    return bash_panel_range_text(index, -1);
}

/* Les n dernières lignes du BUFFER (indépendant du scroll affiché),
 * lecture légère : seule la plage demandée est extraite. */
static gchar *
bash_panel_tail_text(guint index, glong n)
{
    VteTerminal *term = bash_panel_term(index);
    glong        bottom;

    if (term == NULL)
        return NULL;
    bottom = bash_panel_bottom(index);
    return bash_panel_range_text(index, bottom - n + 1);
}

glong
bash_panel_line_count(guint index)
{
    gchar *text;
    glong  n;

    text = bash_panel_full_text(index);
    if (text == NULL)
        return -1;
    n = 0;
    for (const char *c = text; *c != '\0'; c++)
        if (*c == '\n')
            n++;
    if (text[0] != '\0' && !g_str_has_suffix(text, "\n"))
        n++; /* dernière ligne sans \n final */
    g_free(text);
    return n;
}

gchar *
bash_panel_last_line(guint index)
{
    gchar      *text;
    const char *end;

    /* Lecture légère : 5 dernières lignes du buffer suffisent. */
    text = bash_panel_tail_text(index, 5);
    if (text == NULL)
        return NULL;
    end = text + strlen(text);
    while (end > text && (end[-1] == '\n' || end[-1] == '\r'))
        end--; /* saute les lignes vides finales */
    {
        const char *start = end;

        while (start > text && start[-1] != '\n')
            start--;
        /* PAS de trim des espaces : « $ » suivi d'espaces est le
         * signal du prompt (le padding VTE les fournit). */
        {
            gchar *last = g_strndup(start, (gsize)(end - start));

            g_free(text); /* l'extraction VTE : copie rendue, original jeté */
            return last;
        }
    }
}

gchar *
bash_panel_text(guint index)
{
    return bash_panel_full_text(index);
}

gchar *
bash_panel_slice(guint index, glong first, glong last)
{
    gchar   *text;
    gchar  **lines;
    guint    n;
    GString *acc;

    text = bash_panel_full_text(index);
    if (text == NULL)
        return NULL;
    lines = g_strsplit(text, "\n", -1);
    n = g_strv_length(lines);

    if (first < 0)
        first = 0;
    if (last > (glong)n - 1)
        last = (glong)n - 1;
    acc = g_string_new(NULL);
    for (glong i = first; i <= last; i++) {
        gsize len = strlen(lines[i]);

        if (len > 0 && lines[i][len - 1] == '\r')
            lines[i][len - 1] = '\0'; /* normalise CRLF éventuel */
        g_string_append_printf(acc, "%s\n", lines[i]);
    }
    g_strfreev(lines);
    g_free(text);
    return g_string_free(acc, FALSE);
}

void
bash_panel_ensure_tabs(guint count)
{
    BashPanel *p;

    if (cdb_first_panel == NULL)
        return;
    p = g_object_get_data(G_OBJECT(cdb_first_panel), "bash-panel");
    if (p == NULL)
        return;
    while ((guint)p->count < count && p->count < BASH_TAB_MAX)
        bash_panel_add_tab(p);
}

/* Premier spawn DIFFÉRÉ d'un tick idle (fix race au boot) : au moment
 * où on_activate crée les tuiles, multi_paths est encore vide — le boot
 * ne remplit la sélection (dernier fichier ouvert) qu'APRÈS render_layout.
 * Spawner tout de suite = shell dans $HOME (prompt « ~$ »). L'idle passe
 * après la fin de on_activate : le projet courant est alors résolu. */
static gboolean
bash_first_spawn_idle(gpointer data)
{
    BashPanel *p = data;

    p->first_spawn_idle = 0;
    if (p->count == 0) /* un onglet manuel n'aurait rien changé */
        bash_panel_add_tab(p);
    bash_panel_update(p);
    return G_SOURCE_REMOVE;
}

/* À la destruction du notebook : annule le spawn différé (sinon l'idle
 * toucherait un BashPanel déjà libéré par set_data_full). */
static void
bash_panel_destroy(GtkWidget G_GNUC_UNUSED *w, gpointer data)
{
    BashPanel *p = data;

    if (p->first_spawn_idle != 0) {
        g_source_remove(p->first_spawn_idle);
        p->first_spawn_idle = 0;
    }
}

/* Crée UNE fois le backend bash permanent : le notebook singleton et son
 * BashPanel, tenus par une ref FORTE. Appelé au démarrage (bash_panel_init)
 * et, par sûreté, à chaque création de vue. Dès ici cdb_first_panel ne
 * repart plus à NULL (plus de pointeur faible) : les outils peuvent exécuter
 * sans qu'aucune tuile « bash » ne soit affichée. roots/multi_paths sont
 * re-fixés à chaque appel — la sélection du projet se résout au spawn. */
static void
bash_backend_ensure(GListStore *roots, GHashTable *multi_paths)
{
    BashPanel *p;

    if (cdb_first_panel != NULL) {
        p = g_object_get_data(G_OBJECT(cdb_first_panel), "bash-panel");
        if (p != NULL) {
            p->roots = roots;
            p->multi_paths = multi_paths;
        }
        return;
    }

    p = g_new0(BashPanel, 1);
    p->notebook = gtk_notebook_new();
    p->roots = roots;
    p->multi_paths = multi_paths;
    /* Ref FORTE : le notebook survit au démontage de l'arbre de layout.
     * render_layout() fait un gtk_widget_unparent() qui, SANS cette ref,
     * finaliserait le notebook -> ses pages -> la fermeture des PTY. Une
     * vue ne fait qu'EMPRUNTER le notebook ; la dernière ref n'est lâchée
     * qu'à bash_panel_shutdown(). C'est le découplage rendu/état. */
    g_object_ref_sink(p->notebook);
    g_object_set_data_full(G_OBJECT(p->notebook), "bash-panel", p, g_free);
    cdb_first_panel = p->notebook;
    g_signal_connect(p->notebook, "destroy", G_CALLBACK(bash_panel_destroy), p);

    /* Bouton « + » : nouvel onglet (désactivé à la limite). */
    p->add_btn = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(p->add_btn, "flat");
    gtk_widget_add_css_class(p->add_btn, "cdb-flat");
    g_signal_connect(p->add_btn, "clicked", G_CALLBACK(on_add_tab_clicked), p);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(p->notebook), p->add_btn,
                                   GTK_PACK_START);
}

/* Point d'entrée du démarrage : rend le backend bash disponible pour les
 * outils AVANT toute tuile visible, sans créer d'onglet (création lazy). */
void
bash_panel_init(GListStore *roots, GHashTable *multi_paths)
{
    bash_backend_ensure(roots, multi_paths);
}

/* Fabrique de VUE. La tuile « bash » n'est qu'un support d'affichage du
 * notebook singleton. S'il est encore accroché à une vue précédente
 * (modale « tuile autonome »), on le détache avant de le re-parenter : la
 * ref forte du backend le maintient vivant entre les deux, et AUCUN onglet
 * existant n'est perdu — les shells survivent désormais aux re-rendus. */
GtkWidget *
bash_panel_new(GListStore *roots, GHashTable *multi_paths)
{
    BashPanel *p;

    bash_backend_ensure(roots, multi_paths);
    p = g_object_get_data(G_OBJECT(cdb_first_panel), "bash-panel");

    /* Premier onglet d'une vue fraîche — mais SPAWN DIFFÉRÉ au boot pour
     * résoudre le projet courant : voir bash_first_spawn_idle. */
    if (p != NULL && p->count == 0 && p->first_spawn_idle == 0)
        p->first_spawn_idle = g_idle_add(bash_first_spawn_idle, p);

    /* Emprunt : en re-rendu l'ancien arbre est déjà démonté (parent NULL) ;
     * seule une modale ouverte peut encore tenir le notebook. */
    if (gtk_widget_get_parent(cdb_first_panel) != NULL)
        gtk_widget_unparent(cdb_first_panel);

    return cdb_first_panel;
}

/* Fin de process : lâche la ref forte du singleton. Le notebook se
 * finalise alors (s'il n'est plus dans un arbre), et avec lui ses onglets
 * vivants — leurs PTY ferment, comme un « x » sur chaque terminal au quit. */
void
bash_panel_shutdown(void)
{
    if (cdb_first_panel == NULL)
        return;
    g_object_unref(cdb_first_panel);
    cdb_first_panel = NULL;
}
