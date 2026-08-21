/*
 * modal.c : fenêtres-modales (max 4) — voir modal.h.
 */

#include "modal.h"

static gboolean
on_modal_close(GtkWindow G_GNUC_UNUSED *win, gpointer data)
{
    int *count = data;

    (*count)--; /* libère une place */
    return FALSE; /* la fermeture continue */
}

gboolean
modal_open(GtkWindow *parent, int *count, GtkWidget *titlebar,
           GtkWidget *content, GtkWindow **win_out)
{
    GtkWidget *win;

    if (win_out != NULL)
        *win_out = NULL;
    if (*count >= MODAL_MAX)
        return FALSE;

    win = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 480, 360);
    if (titlebar != NULL)
        gtk_window_set_titlebar(GTK_WINDOW(win), titlebar);

    gtk_window_set_child(GTK_WINDOW(win), content);

    (*count)++;
    g_signal_connect(win, "close-request", G_CALLBACK(on_modal_close), count);
    gtk_window_present(GTK_WINDOW(win));
    if (win_out != NULL)
        *win_out = GTK_WINDOW(win);
    return TRUE;
}