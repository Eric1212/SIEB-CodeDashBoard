/*
 * bashpanel.c : panneau « Bash » — GtkNotebook de 1 à 10 terminaux VTE.
 *
 * Chaque onglet est un VteTerminal avec un vrai shell ($SHELL, fallback
 * /bin/bash) lancé dans $HOME. Le panneau est une vue fraîche : comme les
 * autres pièces, il est détruit/recréé au re-rendu du layout (les sessions
 * en cours ne survivent pas aux splits/removes — voir CLAUDE.md).
 */

#include "bashpanel.h"
#include <vte/vte.h>

#define BASH_TAB_MIN 1
#define BASH_TAB_MAX 10

typedef struct {
    GtkWidget *notebook;
    GtkWidget *add_btn;
    int        count;   /* onglets actifs */
    GListStore *roots;  /* pour résoudre le projet courant au spawn */
    GHashTable *multi_paths;
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

            g_snprintf(buf, sizeof(buf), "bash %d", i + 1);
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

/* Label d'onglet : « bash N » + bouton fermer (data = le terminal). */
static GtkWidget *
bash_tab_label(GtkWidget *term, int index)
{
    GtkWidget *box;
    GtkWidget *label;
    GtkWidget *close_btn;
    char       buf[16];

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_snprintf(buf, sizeof(buf), "bash %d", index);
    label = gtk_label_new(buf);
    gtk_box_append(GTK_BOX(box), label);

    close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close_btn, "flat");
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
        g_printerr("CDB: bash spawn échoué : %s\n", err->message);
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
    index = p->count + 1;
    term = vte_terminal_new();
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

GtkWidget *
bash_panel_new(GListStore *roots, GHashTable *multi_paths)
{
    BashPanel *p = g_new0(BashPanel, 1);

    p->notebook = gtk_notebook_new();
    p->roots = roots;
    p->multi_paths = multi_paths;
    g_object_set_data_full(G_OBJECT(p->notebook), "bash-panel", p, g_free);

    /* Bouton « + » : nouvel onglet (désactivé à la limite). */
    p->add_btn = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(p->add_btn, "flat");
    g_signal_connect(p->add_btn, "clicked", G_CALLBACK(on_add_tab_clicked), p);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(p->notebook), p->add_btn,
                                   GTK_PACK_START);

    bash_panel_add_tab(p); /* au moins un terminal */
    return p->notebook;
}