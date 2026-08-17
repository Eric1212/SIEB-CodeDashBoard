/*
 * SIEB - CodeDashBoard
 *
 * Fenêtre GTK4 + GtkSourceView 5 : HeaderBar avec ouverture de fichier,
 * panneau latéral "Dossiers" (roots de structure / roots de projet,
 * persistance JSON), éditeur avec coloration syntaxique, barre de statut.
 *
 * Compilation : make
 */

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <adwaita.h>
#include <glib/gstdio.h>
#include "roots.h"
#include "fslist.h"

typedef struct {
    GtkWindow         *win;
    GtkPaned          *paned;
    GtkSourceBuffer   *buffer;
    GListStore        *roots;
    GtkMultiSelection  *selection;
    GtkTreeListModel   *tree_model;
    GtkWidget          *explorer_scrolled;
    GdkModifierType    last_click_mods;
    guint              sel_anchor; /* dernière ancre (clic exclusif) */
    GtkLabel          *status_file;
    GtkLabel          *status_pos;
    GtkWidget         *statusbar;
    RootEntry         *pending_remove;
    RootKind           pending_kind;
    gboolean           centered;
} App;

/* ------------------------------------------------------------------ */
/* Statut                                                              */
/* ------------------------------------------------------------------ */

static void
update_status(App *app)
{
    GtkTextIter iter;
    gchar      *text;

    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(app->buffer), &iter,
                                     gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(app->buffer)));

    text = g_strdup_printf("%d:%d",
                           gtk_text_iter_get_line(&iter) + 1,
                           gtk_text_iter_get_line_offset(&iter) + 1);
    gtk_label_set_text(app->status_pos, text);
    g_free(text);
}

static void
on_cursor_notify(GObject G_GNUC_UNUSED *obj, GParamSpec G_GNUC_UNUSED *pspec, gpointer data)
{
    update_status((App *)data);
}

/* Prototypes (définitions plus bas dans le fichier). */
static void reveal_path(App *app, const char *file_path);
static void rebuild_explorer(App *app);
static void on_selection_changed(GtkSelectionModel *model, guint position,
                                 guint n_items, gpointer data);

/* ------------------------------------------------------------------ */
/* Chargement de fichier                                               */
/* ------------------------------------------------------------------ */

static void
load_file(App *app, const char *path)
{
    GtkSourceLanguageManager *lang_mgr;
    GtkSourceLanguage        *language;
    gchar                    *content = NULL;
    gsize                     len = 0;
    GError                   *error = NULL;

    if (!g_file_get_contents(path, &content, &len, &error)) {
        g_printerr("SIEB - CodeDashBoard: %s\n", error->message);
        g_error_free(error);
        return;
    }

    /* Fichier binaire ou encodage non UTF-8 : l'éditeur ne peut pas
     * l'afficher — on prévient au lieu de casser le buffer. */
    if (!g_utf8_validate(content, len, NULL)) {
        GtkAlertDialog *alert = gtk_alert_dialog_new(
            "Fichier binaire ou encodage non UTF-8 :\n%s", path);
        gtk_alert_dialog_show(alert, app->win);
        g_free(content);
        return;
    }

    /* Détection de la langue par extension (retombe sur C si inconnue). */
    lang_mgr = gtk_source_language_manager_get_default();
    language = gtk_source_language_manager_guess_language(lang_mgr, path, NULL);
    if (language == NULL)
        language = gtk_source_language_manager_get_language(lang_mgr, "c");
    gtk_source_buffer_set_language(app->buffer, language);

    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(app->buffer), content, (int)len);
    g_free(content);

    gtk_label_set_text(app->status_file, path);
    gtk_label_set_ellipsize(app->status_file, PANGO_ELLIPSIZE_MIDDLE);
    update_status(app);

    /* Mémorise le dernier fichier ouvert (réouverture au boot). */
    roots_write_last_file(path);

    /* L'explorateur suit l'éditeur : déplie jusqu'au fichier, sélectionne. */
    reveal_path(app, path);
}

/* ------------------------------------------------------------------ */
/* Dossiers : roots de structure et roots de projet                    */
/* ------------------------------------------------------------------ */

/* Un root de structure peut contenir des projets : le GtkTreeListModel
 * demande le modèle enfant d'un item via cette fonction. */
static GListModel *
roots_create_child(gpointer item, gpointer user_data)
{
    (void)user_data;

    /* Niveau roots : une structure expose ses projets ; un projet expose
     * son contenu filesystem (explorateur inline, scanné au dépliage). */
    if (g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)) {
        RootEntry *e = item;

        if (e->kind == ROOT_STRUCTURE)
            return e->children != NULL
                   ? G_LIST_MODEL(g_object_ref(e->children)) : NULL;
        /* Le tree model prend possession (transfer full) du modèle qu'on
         * lui retourne et le détruit au repliage : on garde notre propre
         * référence pour éviter le pointeur zombie au re-dépliage.
         * contents_dirty : re-scan après création/suppression/renommage. */
        if (e->contents == NULL || e->contents_dirty) {
            if (e->contents != NULL)
                g_object_unref(e->contents);
            e->contents = fs_scan_dir(e->path);
            e->contents_dirty = FALSE;
        }
        return G_LIST_MODEL(g_object_ref(e->contents));
    }

    /* Niveau explorateur : un dossier expose ses enfants (lazy). */
    {
        FileEntry *f = item;

        if (!f->is_dir)
            return NULL;
        if (f->children == NULL || f->children_dirty) {
            if (f->children != NULL)
                g_object_unref(f->children);
            f->children = fs_scan_dir(f->path);
            f->children_dirty = FALSE;
        }
        return G_LIST_MODEL(g_object_ref(f->children));
    }
}

static void on_row_pressed(GtkGestureClick *gesture, int n_press,
                           double x, double y, gpointer data);
static void on_remove_root_clicked(GtkButton *button, gpointer data);
static void on_add_project_to_structure(GtkButton *button, gpointer data);
static void on_delete_project_clicked(GtkButton *button, gpointer data);
static void on_row_activate(GtkListView *view, guint position, gpointer data);
static void reveal_path(App *app, const char *file_path);
static void on_new_file_clicked(GtkButton *button, gpointer data);
static void on_new_dir_clicked(GtkButton *button, gpointer data);
static void on_rename_clicked(GtkButton *button, gpointer data);
static void on_delete_file_clicked(GtkButton *button, gpointer data);
static void on_delete_multi_clicked(GtkButton *button, gpointer data);
static GPtrArray *selected_file_paths(App *app);

static void
on_row_setup(GtkListItemFactory G_GNUC_UNUSED *factory, GtkListItem *item,
             gpointer data)
{
    GtkWidget  *expander = gtk_tree_expander_new();
    GtkWidget  *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget  *icon = gtk_image_new();
    GtkWidget  *label = gtk_label_new(NULL);
    GtkGesture *gesture = gtk_gesture_click_new();

    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(item, expander);

    g_object_set_data(G_OBJECT(item), "icon", icon);
    g_object_set_data(G_OBJECT(item), "label", label);

    /* Clic droit sur la ligne → menu de suppression. */
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 3);
    g_signal_connect(gesture, "pressed", G_CALLBACK(on_row_pressed), data);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(gesture));

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: row setup (gesture bouton 3 attaché)\n");
}

static void
on_row_bind(GtkListItemFactory G_GNUC_UNUSED *factory, GtkListItem *item,
            gpointer G_GNUC_UNUSED data)
{
    GtkTreeListRow *row = gtk_list_item_get_item(item);
    GtkWidget      *icon = g_object_get_data(G_OBJECT(item), "icon");
    GtkWidget      *label = g_object_get_data(G_OBJECT(item), "label");
    GtkWidget      *expander = gtk_list_item_get_child(item);

    if (g_type_is_a(G_TYPE_FROM_INSTANCE(gtk_tree_list_row_get_item(row)),
                    ROOT_TYPE_ENTRY)) {
        RootEntry *entry = gtk_tree_list_row_get_item(row);

        gtk_image_set_from_icon_name(GTK_IMAGE(icon),
            entry->kind == ROOT_STRUCTURE ? "folder-symbolic"
                                          : "folder-documents-symbolic");
        gtk_label_set_text(GTK_LABEL(label), entry->basename);
    } else {
        FileEntry *f = gtk_tree_list_row_get_item(row);

        gtk_image_set_from_icon_name(GTK_IMAGE(icon),
            f->is_dir ? "folder-symbolic" : "text-x-generic-symbolic");
        gtk_label_set_text(GTK_LABEL(label), f->name);
    }
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
    /* pos+1 : l'index 0 est valide, GUINT_TO_POINTER(0) == NULL. */
    g_object_set_data(G_OBJECT(expander), "sieb-pos",
                      GUINT_TO_POINTER(gtk_list_item_get_position(item) + 1));
}

static void
on_remove_root_clicked(GtkButton *button, gpointer data)
{
    App *app = data;

    if (app->pending_remove != NULL) {
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: suppression de « %s »\n", app->pending_remove->path);
        roots_remove(app->roots, app->pending_remove);
        roots_save(app->roots);
        app->pending_remove = NULL;
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: MUTATION unselect_all from %s\n", G_STRFUNC);
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(app->selection));
    }
    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_parent(GTK_WIDGET(button))));
}

/* Popover créé à chaque clic : libération à la fermeture. */
static void
on_remove_popover_closed(GtkPopover *popover, gpointer G_GNUC_UNUSED data)
{
    gtk_widget_unparent(GTK_WIDGET(popover));
    g_object_unref(popover);
}

static void
on_row_pressed(GtkGestureClick *gesture, int G_GNUC_UNUSED n_press,
               double x, double y, gpointer data)
{
    App            *app = data;
    GtkWidget      *expander = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GtkTreeListRow *row = gtk_tree_expander_get_list_row(GTK_TREE_EXPANDER(expander));
    gpointer        item;
    gboolean        is_root;
    gboolean        is_dir;
    RootEntry      *entry = NULL;

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: clic droit capté (row=%p)\n", (void *)row);

    if (row == NULL)
        return;
    item = gtk_tree_list_row_get_item(row);
    is_root = g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY);
    is_dir = !is_root && ((FileEntry *)item)->is_dir;

    if (is_root)
        entry = item;
    app->pending_remove = entry;

    /* Sélection multiple : si le clic est sur une row sélectionnée et que
     * la sélection contient ≥ 2 fichiers/dossiers → suppression groupée. */
    {
        guint    n_sel = 0;
        gboolean row_sel = gtk_selection_model_is_selected(
            GTK_SELECTION_MODEL(app->selection),
            gtk_tree_list_row_get_position(row));

        if (row_sel) {
            GPtrArray *sel = selected_file_paths(app);

            n_sel = sel->len;
            g_ptr_array_free(sel, TRUE);
        }

    /* Popover neuf à chaque clic : positionné sur la ligne cliquée. */
    {
        GtkWidget *popover = gtk_popover_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *rm_button = NULL;
        char      *tmp_dir = NULL;
        const char *dir_path;

        if (is_root)
            dir_path = entry->path;
        else {
            FileEntry *f = item;

            if (f->is_dir)
                dir_path = f->path; /* créer DANS le dossier */
            else {
                tmp_dir = g_path_get_dirname(f->path);
                dir_path = tmp_dir; /* fichier : dans son dossier parent */
            }
        }

        if (row_sel && n_sel >= 2) {
            /* Suppression groupée (fichiers/dossiers sélectionnés). */
            GtkWidget *btn;
            char      *label = g_strdup_printf("Supprimer %u éléments", n_sel);

            btn = gtk_button_new_with_label(label);
            g_free(label);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_delete_multi_clicked), app);
            gtk_box_append(GTK_BOX(box), btn);
        } else {
        /* Création : projets, dossiers ET fichiers (dans le parent). */
        if (!is_root || entry->kind == ROOT_PROJECT) {
            GtkWidget *btn;

            btn = gtk_button_new_with_label("Nouveau fichier…");
            g_object_set_data_full(G_OBJECT(btn), "dir", g_strdup(dir_path),
                                   g_free);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_new_file_clicked), app);
            gtk_box_append(GTK_BOX(box), btn);

            btn = gtk_button_new_with_label("Nouveau dossier…");
            g_object_set_data_full(G_OBJECT(btn), "dir", g_strdup(dir_path),
                                   g_free);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_new_dir_clicked), app);
            gtk_box_append(GTK_BOX(box), btn);
        }

        /* Fichier : renommer + supprimer ; dossier : supprimer (récursif),
         * sans confirmation DELETE (ce n'est pas un projet complet). */
        if (!is_root) {
            GtkWidget *btn;

            btn = gtk_button_new_with_label("Renommer…");
            g_object_set_data_full(G_OBJECT(btn), "path",
                                   g_strdup(((FileEntry *)item)->path), g_free);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_rename_clicked), app);
            gtk_box_append(GTK_BOX(box), btn);

            btn = gtk_button_new_with_label(is_dir ? "Supprimer le dossier"
                                                   : "Supprimer");
            g_object_set_data_full(G_OBJECT(btn), "path",
                                   g_strdup(((FileEntry *)item)->path), g_free);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_delete_file_clicked), app);
            gtk_box_append(GTK_BOX(box), btn);
        }

        /* Suppressions / ajouts propres aux roots. */
        if (is_root) {
            if (entry->kind == ROOT_STRUCTURE) {
                GtkWidget *add_button = gtk_button_new_with_label("Ajouter un projet…");

                g_object_set_data(G_OBJECT(add_button), "structure", entry);
                g_signal_connect(add_button, "clicked",
                                 G_CALLBACK(on_add_project_to_structure), app);
                gtk_box_append(GTK_BOX(box), add_button);
            }
            if (entry->parent != NULL) {
                /* Projet enfant : suppression destructive, confirmation
                 * obligatoire (« Retape : DELETE »). */
                rm_button = gtk_button_new_with_label("Supprimer ce projet…");
                g_object_set_data(G_OBJECT(rm_button), "entry", entry);
                g_signal_connect(rm_button, "clicked",
                                 G_CALLBACK(on_delete_project_clicked), app);
            } else {
                /* Root : suppression directe (pas de confirmation). */
                rm_button = gtk_button_new_with_label("Supprimer ce root");
                g_signal_connect(rm_button, "clicked",
                                 G_CALLBACK(on_remove_root_clicked), app);
            }
            gtk_box_append(GTK_BOX(box), rm_button);
        }
        }
        g_free(tmp_dir);

        gtk_popover_set_child(GTK_POPOVER(popover), box);
        gtk_widget_set_parent(popover, expander);
        /* Pointe la bulle sur le curseur (coordonnées du gesture). */
        {
            GdkRectangle rect = { (int)x, (int)y, 1, 1 };
            gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
        }
        g_signal_connect(popover, "closed",
                         G_CALLBACK(on_remove_popover_closed), NULL);
        gtk_popover_popup(GTK_POPOVER(popover));

        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: popover popup demandé pour « %s »\n", dir_path);
    }
    } /* fin bloc sélection multiple */
}

/* --- Création d'un projet (dossier) au sein d'une structure ---------- */

typedef struct {
    App        *app;
    RootEntry  *structure;
    GtkWidget  *dialog;
    GtkWidget  *entry;
} CreateProjectData;

static void
on_create_project_clicked(GtkButton G_GNUC_UNUSED *button, gpointer data)
{
    CreateProjectData *d = data;
    const char *error_msg = NULL;
    char *name;
    char *full;

    name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->entry)));
    g_strstrip(name);

    if (name[0] == '\0') {
        error_msg = "Le nom du projet est vide.";
    } else if (strchr(name, '/') != NULL || g_strcmp0(name, ".") == 0 ||
               g_strcmp0(name, "..") == 0) {
        error_msg = "Nom invalide (pas de « / », ni « . », ni « .. »).";
    } else if (name[0] == '.') {
        error_msg = "Un projet caché (commençant par « . ») n'est pas autorisé.";
    } else {
        full = g_build_filename(d->structure->path, name, NULL);
        if (g_file_test(full, G_FILE_TEST_EXISTS)) {
            error_msg = "Ce dossier existe déjà.";
        } else if (g_mkdir(full, 0700) != 0) {
            error_msg = "Impossible de créer le dossier.";
        } else {
            roots_add(d->app->roots, d->structure, ROOT_PROJECT, full);
            roots_save(d->app->roots);
        }
        g_free(full);
    }

    g_free(name);
    if (error_msg != NULL) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("%s", error_msg);
        gtk_alert_dialog_show(alert, d->app->win);
        return;
    }
    gtk_window_destroy(GTK_WINDOW(d->dialog));
}

static void
on_create_project_cancel(GtkButton G_GNUC_UNUSED *button, gpointer data)
{
    CreateProjectData *d = data;

    gtk_window_destroy(GTK_WINDOW(d->dialog));
}

/* Clic droit sur une structure → « Ajouter un projet… » : un nom suffit,
 * le dossier est créé dans la structure. */
static void
on_add_project_to_structure(GtkButton *button, gpointer data)
{
    App              *app = data;
    RootEntry        *structure = g_object_get_data(G_OBJECT(button), "structure");
    CreateProjectData *d;
    GtkWidget        *dialog;
    GtkWidget        *content;
    GtkWidget        *label;
    GtkWidget        *entry;
    GtkWidget        *btn_row;
    GtkWidget        *cancel;
    GtkWidget        *create;
    char             *label_text;

    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_parent(GTK_WIDGET(button))));

    dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Nouveau projet");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), app->win);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), content);

    label_text = g_strdup_printf("Créer un dossier dans :\n%s",
                                 structure->path);
    label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    g_free(label_text);
    gtk_box_append(GTK_BOX(content), label);

    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "nom du projet");
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(content), entry);

    d = g_new0(CreateProjectData, 1);
    d->app = app;
    d->structure = structure;
    d->dialog = dialog;
    d->entry = entry;

    btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("Annuler");
    create = gtk_button_new_with_label("Créer");
    gtk_widget_add_css_class(create, "suggested-action");
    g_signal_connect(cancel, "clicked",
                     G_CALLBACK(on_create_project_cancel), d);
    g_signal_connect(create, "clicked",
                     G_CALLBACK(on_create_project_clicked), d);
    g_signal_connect(entry, "activate",
                     G_CALLBACK(on_create_project_clicked), d);
    g_signal_connect_swapped(dialog, "destroy",
                             G_CALLBACK(g_free), d);
    gtk_box_append(GTK_BOX(btn_row), cancel);
    gtk_box_append(GTK_BOX(btn_row), create);
    gtk_box_append(GTK_BOX(content), btn_row);

    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(entry);
}

/* --- Suppression d'un projet (dossier) avec confirmation « DELETE » --- */

typedef struct {
    App       *app;
    RootEntry *entry;
    GtkWidget *dialog;
    GtkWidget *entry_widget;
    GtkWidget *delete_button;
} DeleteProjectData;

static void
on_confirm_delete_changed(GtkEditable G_GNUC_UNUSED *editable, gpointer data)
{
    DeleteProjectData *d = data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(d->entry_widget));

    gtk_widget_set_sensitive(d->delete_button, g_strcmp0(text, "DELETE") == 0);
}

static void
on_confirm_delete_clicked(GtkButton G_GNUC_UNUSED *button, gpointer data)
{
    DeleteProjectData *d = data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(d->entry_widget));

    if (g_strcmp0(text, "DELETE") != 0)
        return; /* bouton inactif normalement ; double sécurité */

    if (!roots_delete_recursive(d->entry->path)) {
        GtkAlertDialog *alert = gtk_alert_dialog_new(
            "Impossible de supprimer le dossier (permissions ?).");
        gtk_alert_dialog_show(alert, d->app->win);
        return;
    }
    roots_remove(d->app->roots, d->entry);
    roots_save(d->app->roots);
    gtk_window_destroy(GTK_WINDOW(d->dialog));
}

static void
on_confirm_delete_cancel(GtkButton G_GNUC_UNUSED *button, gpointer data)
{
    DeleteProjectData *d = data;

    gtk_window_destroy(GTK_WINDOW(d->dialog));
}

/* Clic droit sur un projet enfant → « Supprimer ce projet… » : la
 * suppression du dossier est destructive, confirmation obligatoire. */
static void
on_delete_project_clicked(GtkButton *button, gpointer data)
{
    App              *app = data;
    RootEntry        *entry = g_object_get_data(G_OBJECT(button), "entry");
    DeleteProjectData *d;
    GtkWidget        *dialog;
    GtkWidget        *content;
    GtkWidget        *label;
    GtkWidget        *entry_widget;
    GtkWidget        *btn_row;
    GtkWidget        *cancel;
    GtkWidget        *del_button;
    char             *label_text;

    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_parent(GTK_WIDGET(button))));

    dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Supprimer le projet");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), app->win);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), content);

    label_text = g_strdup_printf(
        "Le dossier et tout son contenu seront définitivement supprimés :\n"
        "%s\n\nRetape : DELETE pour confirmer.",
        entry->path);
    label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    g_free(label_text);
    gtk_box_append(GTK_BOX(content), label);

    entry_widget = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_widget), "DELETE");
    gtk_widget_set_hexpand(entry_widget, TRUE);
    gtk_box_append(GTK_BOX(content), entry_widget);

    d = g_new0(DeleteProjectData, 1);
    d->app = app;
    d->entry = entry;
    d->dialog = dialog;
    d->entry_widget = entry_widget;

    btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("Annuler");
    del_button = gtk_button_new_with_label("Supprimer");
    gtk_widget_add_css_class(del_button, "destructive-action");
    gtk_widget_set_sensitive(del_button, FALSE);
    d->delete_button = del_button;

    g_signal_connect(cancel, "clicked",
                     G_CALLBACK(on_confirm_delete_cancel), d);
    g_signal_connect(del_button, "clicked",
                     G_CALLBACK(on_confirm_delete_clicked), d);
    g_signal_connect(entry_widget, "changed",
                     G_CALLBACK(on_confirm_delete_changed), d);
    g_signal_connect(entry_widget, "activate",
                     G_CALLBACK(on_confirm_delete_clicked), d);
    g_signal_connect_swapped(dialog, "destroy",
                             G_CALLBACK(g_free), d);
    gtk_box_append(GTK_BOX(btn_row), cancel);
    gtk_box_append(GTK_BOX(btn_row), del_button);
    gtk_box_append(GTK_BOX(content), btn_row);

    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(entry_widget);
}

/* --- Création d'un fichier ou d'un dossier dans l'explorateur ------- */

typedef struct {
    App       *app;
    char      *dir_path;
    char      *old_path;  /* renommage : chemin actuel, NULL si création */
    gboolean   is_dir;
    GtkWidget *dialog;
    GtkWidget *entry;
} NewEntryData;

/* Marque « à re-scanner » le cache du dossier dir_path : parcourt les
 * contenus scannés (racines, puis projets, puis dossiers récursifs). */
static gboolean
mark_store_dirty(GListStore *store, const char *dir_path)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));

    for (guint i = 0; i < n; i++) {
        FileEntry *f = g_list_model_get_item(G_LIST_MODEL(store), i);

        if (f->is_dir) {
            if (g_strcmp0(f->path, dir_path) == 0) {
                f->children_dirty = TRUE;
                g_object_unref(f);
                return TRUE;
            }
            if (f->children != NULL && mark_store_dirty(f->children, dir_path)) {
                g_object_unref(f);
                return TRUE;
            }
        }
        g_object_unref(f);
    }
    return FALSE;
}

static void
mark_parent_dirty(App *app, const char *dir_path)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->roots));

    for (guint i = 0; i < n; i++) {
        RootEntry *e = g_list_model_get_item(G_LIST_MODEL(app->roots), i);

        if (e->kind == ROOT_PROJECT) {
            if (g_strcmp0(e->path, dir_path) == 0)
                e->contents_dirty = TRUE;
            else if (e->contents != NULL)
                mark_store_dirty(e->contents, dir_path);
        } else {
            /* Structure : marquer aussi les projets enfants. */
            guint m = g_list_model_get_n_items(G_LIST_MODEL(e->children));

            for (guint j = 0; j < m; j++) {
                RootEntry *p = g_list_model_get_item(G_LIST_MODEL(e->children), j);

                if (g_strcmp0(p->path, dir_path) == 0)
                    p->contents_dirty = TRUE;
                else if (p->contents != NULL)
                    mark_store_dirty(p->contents, dir_path);
                g_object_unref(p);
            }
        }
        g_object_unref(e);
    }
}

static void
on_new_entry_clicked(GtkButton G_GNUC_UNUSED *button, gpointer data)
{
    NewEntryData *d = data;
    const char   *error_msg = NULL;
    char         *name;
    char         *full;

    name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->entry)));
    g_strstrip(name);

    if (name[0] == '\0') {
        error_msg = "Le nom est vide.";
    } else if (strchr(name, '/') != NULL || g_strcmp0(name, ".") == 0 ||
               g_strcmp0(name, "..") == 0) {
        error_msg = "Nom invalide (pas de « / », ni « . », ni « .. »).";
    } else {
        full = g_build_filename(d->dir_path, name, NULL);

        if (d->old_path != NULL) {
            /* Renommage. */
            if (g_strcmp0(full, d->old_path) == 0) {
                g_free(full);
                g_free(name);
                gtk_window_destroy(GTK_WINDOW(d->dialog));
                return; /* même nom : rien à faire */
            }
            if (g_file_test(full, G_FILE_TEST_EXISTS)) {
                error_msg = "Ce nom existe déjà.";
            } else if (g_rename(d->old_path, full) != 0) {
                error_msg = "Impossible de renommer.";
            }
        } else if (g_file_test(full, G_FILE_TEST_EXISTS)) {
            error_msg = "Ce nom existe déjà.";
        } else if (d->is_dir) {
            if (g_mkdir(full, 0700) != 0)
                error_msg = "Impossible de créer le dossier.";
        } else {
            GError *err = NULL;

            if (!g_file_set_contents(full, "", 0, &err)) {
                error_msg = "Impossible de créer le fichier.";
                g_error_free(err);
            }
        }
        if (error_msg == NULL) {
            if (g_getenv("SIEB_DEBUG") != NULL)
                g_printerr("SIEB: création « %s » dans « %s »\n",
                           name, d->dir_path);
            mark_parent_dirty(d->app, d->dir_path);
            rebuild_explorer(d->app);
            reveal_path(d->app, full);
        }
        g_free(full);
    }

    g_free(name);
    if (error_msg != NULL) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("%s", error_msg);
        gtk_alert_dialog_show(alert, d->app->win);
        return;
    }
    gtk_window_destroy(GTK_WINDOW(d->dialog));
}

static void
on_new_entry_cancel(GtkButton G_GNUC_UNUSED *button, gpointer data)
{
    NewEntryData *d = data;

    gtk_window_destroy(GTK_WINDOW(d->dialog));
}

/* Ouvre le dialog « Nouveau fichier / dossier » dans dir_path. */
static void
open_new_entry_dialog(App *app, const char *dir_path, gboolean is_dir)
{
    NewEntryData *d;
    GtkWidget    *dialog;
    GtkWidget    *content;
    GtkWidget    *label;
    GtkWidget    *entry;
    GtkWidget    *btn_row;
    GtkWidget    *cancel;
    GtkWidget    *create;
    char         *label_text;

    dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), is_dir ? "Nouveau dossier"
                                                    : "Nouveau fichier");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), app->win);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), content);

    label_text = g_strdup_printf("Créer %s dans :\n%s",
                                 is_dir ? "un dossier" : "un fichier",
                                 dir_path);
    label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    g_free(label_text);
    gtk_box_append(GTK_BOX(content), label);

    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                   is_dir ? "nom du dossier" : "nom du fichier");
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(content), entry);

    d = g_new0(NewEntryData, 1);
    d->app = app;
    d->dir_path = g_strdup(dir_path);
    d->is_dir = is_dir;
    d->dialog = dialog;
    d->entry = entry;

    btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("Annuler");
    create = gtk_button_new_with_label("Créer");
    gtk_widget_add_css_class(create, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_new_entry_cancel), d);
    g_signal_connect(create, "clicked", G_CALLBACK(on_new_entry_clicked), d);
    g_signal_connect(entry, "activate", G_CALLBACK(on_new_entry_clicked), d);
    g_signal_connect_swapped(dialog, "destroy", G_CALLBACK(g_free), d);
    gtk_box_append(GTK_BOX(btn_row), cancel);
    gtk_box_append(GTK_BOX(btn_row), create);
    gtk_box_append(GTK_BOX(content), btn_row);

    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(entry);
}

static void
on_new_file_clicked(GtkButton *button, gpointer data)
{
    App       *app = data;
    const char *dir = g_object_get_data(G_OBJECT(button), "dir");

    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_parent(GTK_WIDGET(button))));
    open_new_entry_dialog(app, dir, FALSE);
}

static void
on_new_dir_clicked(GtkButton *button, gpointer data)
{
    App       *app = data;
    const char *dir = g_object_get_data(G_OBJECT(button), "dir");

    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_parent(GTK_WIDGET(button))));
    open_new_entry_dialog(app, dir, TRUE);
}

/* Dialog « Renommer » : entrée pré-remplie avec le nom actuel. */
static void
open_rename_dialog(App *app, const char *old_path)
{
    NewEntryData *d;
    GtkWidget    *dialog;
    GtkWidget    *content;
    GtkWidget    *label;
    GtkWidget    *entry;
    GtkWidget    *btn_row;
    GtkWidget    *cancel;
    GtkWidget    *create;
    char         *label_text;
    char         *base;

    dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Renommer");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), app->win);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), content);

    label_text = g_strdup_printf("Renommer :\n%s", old_path);
    label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    g_free(label_text);
    gtk_box_append(GTK_BOX(content), label);

    entry = gtk_entry_new();
    base = g_path_get_basename(old_path);
    gtk_editable_set_text(GTK_EDITABLE(entry), base);
    g_free(base);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(content), entry);

    d = g_new0(NewEntryData, 1);
    d->app = app;
    d->dir_path = g_path_get_dirname(old_path);
    d->old_path = g_strdup(old_path);
    d->dialog = dialog;
    d->entry = entry;

    btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("Annuler");
    create = gtk_button_new_with_label("Renommer");
    gtk_widget_add_css_class(create, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_new_entry_cancel), d);
    g_signal_connect(create, "clicked", G_CALLBACK(on_new_entry_clicked), d);
    g_signal_connect(entry, "activate", G_CALLBACK(on_new_entry_clicked), d);
    g_signal_connect_swapped(dialog, "destroy", G_CALLBACK(g_free), d);
    gtk_box_append(GTK_BOX(btn_row), cancel);
    gtk_box_append(GTK_BOX(btn_row), create);
    gtk_box_append(GTK_BOX(content), btn_row);

    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(entry);
}

static void
on_rename_clicked(GtkButton *button, gpointer data)
{
    App       *app = data;
    const char *path = g_object_get_data(G_OBJECT(button), "path");

    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_parent(GTK_WIDGET(button))));
    open_rename_dialog(app, path);
}

/* Chemins des FileEntry sélectionnés (fichiers/dossiers de l'explorateur ;
 * les roots sont exclus). */
static GPtrArray *
selected_file_paths(App *app)
{
    GPtrArray    *paths = g_ptr_array_new_with_free_func(g_free);
    GtkBitset    *bits = gtk_selection_model_get_selection(
        GTK_SELECTION_MODEL(app->selection));
    GtkBitsetIter iter;
    guint          pos = 0;

    if (gtk_bitset_iter_init_first(&iter, bits, &pos)) {
        do {
            GtkTreeListRow *row = g_list_model_get_item(
                G_LIST_MODEL(app->tree_model), pos);

            if (row != NULL) {
                gpointer item = gtk_tree_list_row_get_item(row);

                if (!g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY))
                    g_ptr_array_add(paths, g_strdup(((FileEntry *)item)->path));
                g_object_unref(row);
            }
        } while (gtk_bitset_iter_next(&iter, &pos));
    }
    gtk_bitset_unref(bits);
    return paths;
}

/* Suppression groupée des fichiers/dossiers sélectionnés (directe, sans
 * confirmation : ce ne sont pas des projets complets). */
static void
on_delete_multi_clicked(GtkButton *button, gpointer data)
{
    App       *app = data;
    GPtrArray *paths;
    gboolean   failed = FALSE;

    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_parent(GTK_WIDGET(button))));

    paths = selected_file_paths(app);
    for (guint i = 0; i < paths->len; i++) {
        char *dir = g_path_get_dirname(paths->pdata[i]);

        if (!fs_remove_recursive(paths->pdata[i]))
            failed = TRUE;
        mark_parent_dirty(app, dir);
        g_free(dir);
    }
    rebuild_explorer(app);
    if (failed) {
        GtkAlertDialog *alert = gtk_alert_dialog_new(
            "Certains éléments n'ont pas pu être supprimés.");
        gtk_alert_dialog_show(alert, app->win);
    }
    g_ptr_array_free(paths, TRUE);
}

/* Suppression d'un fichier : directe, sans confirmation (ce n'est pas
 * un projet complet). */
static void
on_delete_file_clicked(GtkButton *button, gpointer data)
{
    App        *app = data;
    const char *path = g_object_get_data(G_OBJECT(button), "path");

    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_parent(GTK_WIDGET(button))));
    if (!fs_remove_recursive(path)) {
        GtkAlertDialog *alert = gtk_alert_dialog_new(
            "Impossible de supprimer.");
        gtk_alert_dialog_show(alert, app->win);
        return;
    }
    {
        char *dir = g_path_get_dirname(path);

        mark_parent_dirty(app, dir);
        g_free(dir);
    }
    rebuild_explorer(app);
}

/* --- Ajout de roots -------------------------------------------------- */

static void
on_pick_folder_finished(GObject *source, GAsyncResult *res, gpointer data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    App           *app = data;
    GError        *error = NULL;
    GFile         *file;
    char          *path;

    file = gtk_file_dialog_select_folder_finish(dialog, res, &error);
    if (error != NULL) {
        if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
            g_printerr("SIEB - CodeDashBoard: %s\n", error->message);
        g_error_free(error);
        return;
    }
    if (file == NULL)
        return;

    path = g_file_get_path(file);
    if (path != NULL) {
        /* Structure : scanne ses sous-dossiers en roots projet.
         * Projet : toujours isolé à la racine.
         * Refus si le chemin est déjà un root ou un projet de structure. */
        if (roots_conflict(app->roots, path)) {
            GtkAlertDialog *alert = gtk_alert_dialog_new(
                "Ce dossier est déjà un root ou un projet d'une structure.");
            gtk_alert_dialog_show(alert, app->win);
        } else if (app->pending_kind == ROOT_STRUCTURE) {
            roots_add_structure(app->roots, path);
            roots_save(app->roots);
        } else {
            roots_add(app->roots, NULL, ROOT_PROJECT, path);
            roots_save(app->roots);
        }
        g_free(path);
    }
    g_object_unref(file);
}

static void
pick_folder(App *app, RootKind kind)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();

    app->pending_kind = kind;
    gtk_file_dialog_set_title(dialog, kind == ROOT_STRUCTURE
        ? "Choisir un root de structure"
        : "Choisir un root de projet");
    gtk_file_dialog_select_folder(dialog, app->win, NULL,
                                  on_pick_folder_finished, app);
    g_object_unref(dialog);
}

static void
on_add_structure(GSimpleAction G_GNUC_UNUSED *action,
                 GVariant G_GNUC_UNUSED *param, gpointer data)
{
    pick_folder((App *)data, ROOT_STRUCTURE);
}

static void
on_add_project(GSimpleAction G_GNUC_UNUSED *action,
               GVariant G_GNUC_UNUSED *param, gpointer data)
{
    pick_folder((App *)data, ROOT_PROJECT);
}

static GtkWidget *
build_add_button(void)
{
    GMenu      *menu = g_menu_new();
    GtkWidget  *button;
    GtkWidget  *popover;

    g_menu_append(menu, "Root de structure…", "win.add-structure");
    g_menu_append(menu, "Root de projet…", "win.add-project");
    popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);

    button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "list-add-symbolic");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(button, "Ajouter un root");
    return button;
}

static GtkWidget *
build_roots_panel(App *app)
{
    GtkWidget *panel;
    GtkWidget *title_box;
    GtkWidget *title;
    GtkWidget *add_button;
    GtkWidget *scrolled;

    /* Titre + bouton d'ajout. */
    title = gtk_label_new("Explorateur");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_set_margin_start(title, 8);

    add_button = build_add_button();
    gtk_widget_set_margin_start(add_button, 6);
    gtk_widget_set_margin_end(add_button, 6);

    title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(title_box, 4);
    gtk_widget_set_margin_bottom(title_box, 2);
    gtk_box_append(GTK_BOX(title_box), title);
    gtk_box_append(GTK_BOX(title_box), add_button);

    /* Zone de l'arbre : autoexpand=FALSE, tout replié (compact) ;
     * l'expansion n'arrive qu'au clic de l'utilisateur, sans limite. */
    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    app->explorer_scrolled = scrolled;

    rebuild_explorer(app);

    panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(panel, 200, -1);
    gtk_box_append(GTK_BOX(panel), title_box);
    gtk_box_append(GTK_BOX(panel), scrolled);
    return panel;
}

static guint selection_changed_handler_id = 0;

/* Trace les changements de sélection (debug multi-sélection). */
static void
on_selection_changed(GtkSelectionModel *model, guint position, guint n_items,
                     gpointer G_GNUC_UNUSED data)
{
    if (g_getenv("SIEB_DEBUG") != NULL) {
        GtkBitset *bits = gtk_selection_model_get_selection(model);
        g_printerr("SIEB: sélection -> %lu éléments (changed pos=%u n=%u)\n",
                   (unsigned long)gtk_bitset_get_size(bits), position, n_items);
        gtk_bitset_unref(bits);
    }
}

/* Position d'une ligne : GtkTreeListRow de l'expander, sinon sieb-pos
 * (index + 1, car GUINT_TO_POINTER(0) == NULL). */
static guint
explorer_pos_from_widget(GtkWidget *w)
{
    GtkTreeListRow *row;
    gpointer        tagged;

    if (w == NULL)
        return GTK_INVALID_LIST_POSITION;

    if (GTK_IS_TREE_EXPANDER(w)) {
        row = gtk_tree_expander_get_list_row(GTK_TREE_EXPANDER(w));
        if (row != NULL)
            return gtk_tree_list_row_get_position(row);
    }

    tagged = g_object_get_data(G_OBJECT(w), "sieb-pos");
    if (tagged != NULL)
        return GPOINTER_TO_UINT(tagged) - 1;

    return GTK_INVALID_LIST_POSITION;
}

/* GtkListItem n'est pas un widget : le pick tombe sur le contenu factory
 * (label, box, expander). On remonte jusqu'à l'expander, ou on lit la
 * position posée au bind. */
static guint
explorer_pos_at(GtkWidget *view, double x, double y)
{
    GtkWidget *pick = gtk_widget_pick(view, x, y, GTK_PICK_DEFAULT);

    for (GtkWidget *w = pick; w != NULL; w = gtk_widget_get_parent(w)) {
        guint pos = explorer_pos_from_widget(w);

        if (g_getenv("SIEB_DEBUG") != NULL) {
            const char *parent_type = w != NULL ? G_OBJECT_TYPE_NAME(w) : "NULL";
            gpointer tagged = w != NULL
                ? g_object_get_data(G_OBJECT(w), "sieb-pos") : NULL;
            guint tagged_pos = tagged != NULL ? GPOINTER_TO_UINT(tagged) - 1 : GTK_INVALID_LIST_POSITION;
            g_printerr("SIEB: pick_walk widget=%s sieb-pos=%u pos=%u\n",
                       parent_type, tagged_pos, pos);
        }

        if (pos != GTK_INVALID_LIST_POSITION)
            return pos;

        /* Padding de la row : l'expander est l'enfant, pas un ancêtre. */
        if (g_strcmp0(gtk_widget_get_css_name(w), "row") == 0) {
            pos = explorer_pos_from_widget(gtk_widget_get_first_child(w));
            if (pos != GTK_INVALID_LIST_POSITION)
                return pos;
        }
    }

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: pick=%s — pas de pos\n",
                   pick != NULL ? G_OBJECT_TYPE_NAME(pick) : "NULL");
    return GTK_INVALID_LIST_POSITION;
}

/* Mémorise les modificateurs du dernier clic primaire : Ctrl+clic et
 * Shift+clic doivent sélectionner SANS ouvrir le fichier.
 * En phase CAPTURE : exécuté AVANT le gesture de la vue.
 * Si Ctrl/Shift : on CLAIM le press (la vue ne verra pas l'événement)
 * et on applique toggle (Ctrl) ou plage (Shift). */
static void
on_primary_pressed(GtkGestureClick *gesture,
                   int G_GNUC_UNUSED n_press,
                   double x, double y,
                   gpointer data)
{
    App       *app = data;
    GtkWidget *view = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GdkEvent  *event = gtk_gesture_get_last_event(GTK_GESTURE(gesture), NULL);
    GdkModifierType mods = event != NULL
                           ? gdk_event_get_modifier_state(event) : 0;
    guint pos;

    app->last_click_mods = mods;
    pos = explorer_pos_at(view, x, y);

    if ((mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == 0) {
        if (pos != GTK_INVALID_LIST_POSITION)
            app->sel_anchor = pos;
        return; /* clic simple : la vue gère normalement */
    }

    /* Ctrl/Shift : CLAIM le press — la vue ne fera PAS son select exclusif. */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    if (pos == GTK_INVALID_LIST_POSITION) {
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: Ctrl+clic hors item\n");
        return;
    }

    if (mods & GDK_SHIFT_MASK) {
        guint    anchor = app->sel_anchor;
        guint    lo;
        guint    n;
        gboolean unselect_rest = (mods & GDK_CONTROL_MASK) == 0;

        if (anchor == GTK_INVALID_LIST_POSITION)
            anchor = pos;
        lo = MIN(anchor, pos);
        n = MAX(anchor, pos) - lo + 1;
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: Shift+clic plage [%u, %u] (unselect_rest=%d)\n",
                       lo, lo + n - 1, unselect_rest);
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: MUTATION select_range lo=%u n=%u unselect_rest=%d from %s\n",
                       lo, n, unselect_rest, G_STRFUNC);
        gtk_selection_model_select_range(GTK_SELECTION_MODEL(app->selection),
                                         lo, n, unselect_rest);
        return;
    }

    /* Ctrl : toggle de la ligne, le reste de la sélection est conservé. */
    if (g_getenv("SIEB_DEBUG") != NULL) {
        GtkBitset *bits = gtk_selection_model_get_selection(
            GTK_SELECTION_MODEL(app->selection));
        gboolean was_selected = gtk_selection_model_is_selected(
            GTK_SELECTION_MODEL(app->selection), pos);
        g_printerr("SIEB: Ctrl+clic toggle pos=%u avant: size=%lu is_selected=%d",
                   pos, (unsigned long)gtk_bitset_get_size(bits), was_selected);
        gtk_bitset_unref(bits);
        if (app->tree_model != NULL) {
            GtkTreeListRow *row = gtk_tree_list_model_get_row(app->tree_model, pos);
            if (row != NULL) {
                gpointer item = gtk_tree_list_row_get_item(row);
                if (g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)) {
                    RootEntry *e = item;
                    g_printerr(" path=root:%s\n", e->path);
                } else {
                    FileEntry *f = item;
                    g_printerr(" path=%s%s\n", f->path,
                               f->is_dir ? "/" : "");
                }
                g_object_unref(row);
            } else {
                g_printerr(" path=<no row>\n");
            }
        } else {
            g_printerr(" path=<no tree_model>\n");
        }
        /* Dump complet du bitset: tous les chemins sélectionnés. */
        {
            GtkBitset *all = gtk_selection_model_get_selection(
                GTK_SELECTION_MODEL(app->selection));
            guint n = (guint)gtk_bitset_get_size(all);
            guint i = 0;
            GtkBitsetIter it;
            g_printerr("SIEB: Ctrl+clic bitset complet size=%lu:", (unsigned long)n);
            if (gtk_bitset_iter_init_first(&it, all, &i)) {
                do {
                    const char *label = "<unknown>";
                    if (app->tree_model != NULL) {
                        GtkTreeListRow *r = gtk_tree_list_model_get_row(app->tree_model, i);
                        if (r != NULL) {
                            gpointer item = gtk_tree_list_row_get_item(r);
                            if (g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)) {
                                RootEntry *e = item;
                                label = e->path;
                            } else {
                                FileEntry *f = item;
                                label = f->path;
                            }
                            g_object_unref(r);
                        }
                    }
                    g_printerr(" [%u]=%s", i, label);
                } while (gtk_bitset_iter_next(&it, &i));
            }
            g_printerr("\n");
            gtk_bitset_unref(all);
        }
    }

    if (gtk_selection_model_is_selected(GTK_SELECTION_MODEL(app->selection), pos)) {
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: MUTATION unselect_item pos=%u from %s\n",
                       pos, G_STRFUNC);
        gtk_selection_model_unselect_item(GTK_SELECTION_MODEL(app->selection), pos);
    } else {
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: MUTATION select_item pos=%u exclusive=%d from %s\n",
                       pos, FALSE, G_STRFUNC);
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(app->selection),
                                        pos, FALSE);
    }

    if (g_getenv("SIEB_DEBUG") != NULL) {
        GtkBitset *bits = gtk_selection_model_get_selection(
            GTK_SELECTION_MODEL(app->selection));
        g_printerr("SIEB: Ctrl+clic toggle pos=%u après: size=%lu\n",
                   pos, (unsigned long)gtk_bitset_get_size(bits));
        gtk_bitset_unref(bits);
    }
}

/* Clic modifié : le press claimé a déjà fait le toggle / la plage.
 * Sans claim du release, la vue re-sélectionne en exclusif et émet
 * activate (ouverture + perte de la multi). */
static void
on_primary_released(GtkGestureClick *gesture, int G_GNUC_UNUSED n_press,
                    double G_GNUC_UNUSED x, double G_GNUC_UNUSED y,
                    gpointer data)
{
    App *app = data;

    /* Se fier au press, pas à l'état actuel : Ctrl peut être relâché
     * avant le bouton, la vue ferait alors un select exclusif. */
    if (app->last_click_mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: release CLAIMÉ (mods=0x%x) — pas d'écrasement\n",
                       app->last_click_mods);
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }

    /* Réinitialise les modificateurs pour les activations clavier suivantes. */
    app->last_click_mods = 0;
}

/* Clic (activation) : un fichier de l'explorateur s'ouvre dans l'éditeur. */
static void
on_row_activate(GtkListView *view, guint position, gpointer data)
{
    App               *app = data;
    GtkSelectionModel *model = gtk_list_view_get_model(view);
    GtkTreeListRow    *row = g_list_model_get_item(G_LIST_MODEL(model), position);

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: activate pos=%u (mods=%u)\n", position,
                   app->last_click_mods);

    if (row != NULL && g_getenv("SIEB_DEBUG") != NULL) {
        gpointer item = gtk_tree_list_row_get_item(row);
        if (g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)) {
            RootEntry *e = item;
            g_printerr("SIEB: activate path=root:%s\n", e->path);
        } else {
            FileEntry *f = item;
            g_printerr("SIEB: activate path=%s%s\n", f->path,
                       f->is_dir ? "/" : "");
        }
    }

    /* Ctrl/Shift+clic : sélection uniquement, pas d'ouverture. Normalement
     * l'activate n'est plus émis (release claimé) : garde de sécurité. */
    if ((app->last_click_mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) != 0) {
        if (row != NULL)
            g_object_unref(row);
        return;
    }
    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: activate -> ouverture fichier\n");

    if (row == NULL)
        return;
    if (!g_type_is_a(G_TYPE_FROM_INSTANCE(gtk_tree_list_row_get_item(row)),
                     ROOT_TYPE_ENTRY)) {
        FileEntry *f = gtk_tree_list_row_get_item(row);

        if (!f->is_dir)
            load_file(app, f->path);
    }
    g_object_unref(row);
}

/* Le chemin est-il sous le dossier dir (égal ou immédiatement dedans) ? */
static gboolean
path_is_under(const char *path, const char *dir)
{
    size_t n = strlen(dir);

    return g_str_has_prefix(path, dir) && (path[n] == '\0' || path[n] == '/');
}

/* Sélectionne la row dans l'explorateur (recherche par position).
 * N'écrase PAS une sélection multiple existante : si la row est déjà
 * sélectionnée (ex: Ctrl+clic), on laisse la multi intacte. */
static void
select_row(App *app, GtkTreeListRow *row)
{
    GListModel *model = G_LIST_MODEL(gtk_multi_selection_get_model(app->selection));
    guint n = g_list_model_get_n_items(model);

    if (g_getenv("SIEB_DEBUG") != NULL && row != NULL) {
        gpointer item = gtk_tree_list_row_get_item(row);
        if (g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)) {
            RootEntry *e = item;
            g_printerr("SIEB: select_row target path=root:%s\n", e->path);
        } else {
            FileEntry *f = item;
            g_printerr("SIEB: select_row target path=%s%s\n", f->path,
                       f->is_dir ? "/" : "");
        }
    }

    for (guint i = 0; i < n; i++) {
        GtkTreeListRow *r = g_list_model_get_item(model, i);
        gboolean is = r == row;

        g_object_unref(r);
        if (is) {
            if (!gtk_selection_model_is_selected(
                    GTK_SELECTION_MODEL(app->selection), i)) {
                if (g_getenv("SIEB_DEBUG") != NULL)
                    g_printerr("SIEB: select_row pos=%u (exclusif)\n", i);
                if (g_getenv("SIEB_DEBUG") != NULL)
                    g_printerr("SIEB: MUTATION select_row pos=%u exclusive=1 from %s\n",
                               i, G_STRFUNC);
                gtk_selection_model_select_item(
                    GTK_SELECTION_MODEL(app->selection), i, TRUE);
            } else if (g_getenv("SIEB_DEBUG") != NULL) {
                g_printerr("SIEB: select_row pos=%u déjà sélectionnée (skip)\n", i);
            }
            return;
        }
    }
}

/* Trouve la row d'un enfant DIRECT de la row à parent_pos, par nom.
 * Retourne la row (référence prise) ou NULL. */
static GtkTreeListRow *
find_direct_child(App *app, GtkTreeListRow *parent, const char *name)
{
    GtkTreeListModel *tree = GTK_TREE_LIST_MODEL(
        gtk_multi_selection_get_model(app->selection));
    guint pos = gtk_tree_list_row_get_position(parent);
    guint pd = gtk_tree_list_row_get_depth(parent);

    for (guint k = pos + 1; ; k++) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(tree));
        GtkTreeListRow *r;
        guint           d;

        if (k >= n)
            return NULL;
        r = gtk_tree_list_model_get_row(tree, k);
        if (r == NULL)
            return NULL;
        d = gtk_tree_list_row_get_depth(r);
        if (d <= pd) { /* fin des descendants : plus d'enfant direct */
            g_object_unref(r);
            return NULL;
        }
        if (d == pd + 1) {
            gpointer item = gtk_tree_list_row_get_item(r);

            if (g_type_is_a(G_TYPE_FROM_INSTANCE(item), FS_TYPE_ENTRY) &&
                g_strcmp0(((FileEntry *)item)->name, name) == 0)
                return r; /* ref conservée */
        }
        g_object_unref(r);
    }
}

/* Descend dans l'arbre depuis la row d'un projet, par segments du chemin
 * relatif (rest commence par « / »). Déplie chaque dossier jusqu'au bout ;
 * le dernier segment est sélectionné (reveal) ou déplié (restauration). */
static void
descend_under(App *app, GtkTreeListRow *row, const char *rest,
              gboolean select_last)
{
    char           *slash;
    char           *seg;
    GtkTreeListRow *found;

    if (rest[0] != '/')
        return;
    rest++;
    slash = strchr(rest, '/');
    seg = slash != NULL ? g_strndup(rest, (gsize)(slash - rest)) : g_strdup(rest);

    gtk_tree_list_row_set_expanded(row, TRUE);
    found = find_direct_child(app, row, seg);

    if (found == NULL) {
        g_free(seg);
        return;
    }
    if (slash == NULL) {
        if (select_last)
            select_row(app, found);
        else
            gtk_tree_list_row_set_expanded(found, TRUE);
    } else {
        descend_under(app, found, slash, select_last);
    }
    g_object_unref(found);
    g_free(seg);
}

/* Descend dans l'arbre depuis la racine jusqu'à path : déplie les
 * dossiers ; le dernier segment est sélectionné (select_last=TRUE)
 * ou déplié (FALSE, restauration d'expansion). */
static void
descend_path(App *app, const char *path, gboolean select_last)
{
    GtkTreeListModel *tree = app->tree_model;
    GListModel *model = G_LIST_MODEL(tree);
    guint n = g_list_model_get_n_items(model);

    for (guint i = 0; i < n; i++) {
        GtkTreeListRow *row = g_list_model_get_item(model, i);
        gpointer item;

        /* Les racines de l'explorateur sont les roots (depth 0). */
        if (gtk_tree_list_row_get_depth(row) != 0) {
            g_object_unref(row);
            continue;
        }
        item = gtk_tree_list_row_get_item(row);
        if (!g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)) {
            g_object_unref(row);
            continue;
        }

        RootEntry *e = item;
        if (!path_is_under(path, e->path)) {
            g_object_unref(row);
            continue;
        }
        if (g_strcmp0(path, e->path) == 0) {
            /* Le root lui-même : simple dépliage. */
            gtk_tree_list_row_set_expanded(row, TRUE);
            g_object_unref(row);
            return;
        }
        if (e->kind == ROOT_PROJECT) {
            descend_under(app, row, path + strlen(e->path), select_last);
            g_object_unref(row);
            return;
        }
        if (e->kind == ROOT_STRUCTURE) {
            /* Trouver le projet enfant qui contient path. */
            gtk_tree_list_row_set_expanded(row, TRUE);
            {
                guint pd = gtk_tree_list_row_get_depth(row);
                guint pos = gtk_tree_list_row_get_position(row);

                for (guint k = pos + 1; ; k++) {
                    guint n = g_list_model_get_n_items(G_LIST_MODEL(tree));
                    GtkTreeListRow *crow;
                    guint           d;

                    if (k >= n)
                        break;
                    crow = gtk_tree_list_model_get_row(tree, k);
                    if (crow == NULL)
                        break;
                    d = gtk_tree_list_row_get_depth(crow);
                    if (d <= pd)
                        break;
                    if (d == pd + 1) {
                        RootEntry *ce = gtk_tree_list_row_get_item(crow);

                        if (g_type_is_a(G_TYPE_FROM_INSTANCE(ce), ROOT_TYPE_ENTRY) &&
                            ce->kind == ROOT_PROJECT &&
                            path_is_under(path, ce->path)) {
                            descend_under(app, crow, path + strlen(ce->path),
                                          select_last);
                            g_object_unref(crow);
                            break;
                        }
                    }
                    g_object_unref(crow);
                }
            }
            g_object_unref(row);
            return;
        }
        g_object_unref(row);
    }
}

/* Révèle un fichier : déplie jusqu'à lui, puis le sélectionne. */
static void
reveal_path(App *app, const char *file_path)
{
    descend_path(app, file_path, TRUE);
}

/* Déplie un dossier (restauration d'expansion après rebuild). */
static void
expand_path(App *app, const char *path)
{
    descend_path(app, path, FALSE);
}

/* Collecte les chemins des rows actuellement dépliées. */
static void
collect_expanded(GListModel *model, GPtrArray *paths)
{
    guint n = g_list_model_get_n_items(model);

    for (guint i = 0; i < n; i++) {
        GtkTreeListRow *row = g_list_model_get_item(model, i);

        if (gtk_tree_list_row_get_expanded(row)) {
            gpointer item = gtk_tree_list_row_get_item(row);
            const char *p = g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)
                            ? ((RootEntry *)item)->path
                            : ((FileEntry *)item)->path;

            g_ptr_array_add(paths, g_strdup(p));
        }
        g_object_unref(row);
    }
}

/* Reconstruit entièrement l'explorateur (après création/suppression/
 * renommage sur le disque) et restaure les dossiers ouverts. */
static void
rebuild_explorer(App *app)
{
    GPtrArray           *expanded = g_ptr_array_new_with_free_func(g_free);
    GtkTreeListModel    *tree_model;
    GtkListItemFactory  *factory;
    GtkWidget           *view;

    if (app->tree_model != NULL)
        collect_expanded(G_LIST_MODEL(app->tree_model), expanded);

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: rebuild_explorer (dossiers ouverts=%u)\n",
                   expanded->len);

    app->sel_anchor = GTK_INVALID_LIST_POSITION;

    tree_model = gtk_tree_list_model_new(G_LIST_MODEL(app->roots), FALSE, FALSE,
                                         roots_create_child, NULL, NULL);
    app->tree_model = tree_model;
    app->selection = gtk_multi_selection_new(G_LIST_MODEL(tree_model));
    selection_changed_handler_id = g_signal_connect(app->selection, "selection-changed",
                                                    G_CALLBACK(on_selection_changed), app);

    factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_row_setup), app);
    g_signal_connect(factory, "bind", G_CALLBACK(on_row_bind), app);

    view = gtk_list_view_new(GTK_SELECTION_MODEL(app->selection),
                             GTK_LIST_ITEM_FACTORY(factory));
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(view), TRUE);
    g_signal_connect(view, "activate", G_CALLBACK(on_row_activate), app);
    {
        GtkGesture *gesture = gtk_gesture_click_new();

        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                      GDK_BUTTON_PRIMARY);
        /* Phase CAPTURE : le press arrive AVANT le gesture de la vue,
         * on peut figer la sélection d'avant-clic. */
        gtk_event_controller_set_propagation_phase(
            GTK_EVENT_CONTROLLER(gesture), GTK_PHASE_CAPTURE);
        g_signal_connect(gesture, "pressed",
                         G_CALLBACK(on_primary_pressed), app);
        g_signal_connect(gesture, "released",
                         G_CALLBACK(on_primary_released), app);
        gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(gesture));
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(app->explorer_scrolled),
                                  view);

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: rebuild_explorer vue créée (n_items=%u)\n",
                   g_list_model_get_n_items(G_LIST_MODEL(tree_model)));

    /* Restaure l'état d'expansion. */
    for (guint i = 0; i < expanded->len; i++)
        expand_path(app, expanded->pdata[i]);
    g_ptr_array_free(expanded, TRUE);
}

/* ------------------------------------------------------------------ */
/* Thème clair/sombre système                                          */
/* ------------------------------------------------------------------ */

/*
 * Libadwaita gère tout : AdwStyleManager (initialisé par AdwApplication)
 * suit automatiquement le color-scheme du système (via le portal
 * org.freedesktop.appearance) — aucun code de détection nécessaire.
 * On ne fait que refléter son état sur le schéma de l'éditeur.
 */
static void
update_style_scheme(App *app)
{
    AdwStyleManager              *style_mgr;
    GtkSourceStyleSchemeManager  *scheme_mgr;
    GtkSourceStyleScheme         *scheme;
    const char                   *name;

    style_mgr = adw_style_manager_get_default();
    name = adw_style_manager_get_dark(style_mgr) ? "Adwaita-dark" : "Adwaita";
    scheme_mgr = gtk_source_style_scheme_manager_get_default();
    scheme = gtk_source_style_scheme_manager_get_scheme(scheme_mgr, name);
    if (scheme != NULL)
        gtk_source_buffer_set_style_scheme(app->buffer, scheme);

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: schéma = %s (dark=%d)\n", name,
                   adw_style_manager_get_dark(style_mgr));
}

static void
on_theme_notify(GObject G_GNUC_UNUSED *obj, GParamSpec G_GNUC_UNUSED *pspec, gpointer data)
{
    update_style_scheme((App *)data);
}

/* ------------------------------------------------------------------ */
/* Construction de l'UI                                                */
/* ------------------------------------------------------------------ */

/* Au premier affichage, moitié/moitié : l'éditeur prend toute la moitié droite. */
static gboolean
center_paned(GtkWidget *widget, GdkFrameClock G_GNUC_UNUSED *clock, gpointer data)
{
    App *app = data;
    int  width;

    /* Tick callback : tourne après le layout du frame — la largeur est réelle. */
    width = gtk_widget_get_width(widget);
    if (width <= 0)
        return G_SOURCE_CONTINUE;

    gtk_paned_set_position(app->paned, width / 2);
    return G_SOURCE_REMOVE; /* une seule fois */
}

/* Debug (SIEB_DEBUG=1) : répartition verticale des allocations. */
static gboolean
dump_allocations(gpointer data)
{
    App       *app = data;
    GtkWidget *root = gtk_window_get_child(app->win);
    GtkWidget *side = gtk_paned_get_start_child(app->paned);
    GtkWidget *editor = gtk_paned_get_end_child(app->paned);

    fprintf(stderr,
            "SIEB: win %dx%d | root %dx%d | paned %dx%d | side %dx%d | "
            "editor %dx%d | statusbar %dx%d\n",
            gtk_widget_get_width(GTK_WIDGET(app->win)),
            gtk_widget_get_height(GTK_WIDGET(app->win)),
            gtk_widget_get_width(root), gtk_widget_get_height(root),
            gtk_widget_get_width(GTK_WIDGET(app->paned)),
            gtk_widget_get_height(GTK_WIDGET(app->paned)),
            gtk_widget_get_width(side), gtk_widget_get_height(side),
            gtk_widget_get_width(editor), gtk_widget_get_height(editor),
            gtk_widget_get_width(app->statusbar),
            gtk_widget_get_height(app->statusbar));
    return G_SOURCE_REMOVE;
}

static void
on_first_map(GtkWidget G_GNUC_UNUSED *widget, gpointer data)
{
    App *app = data;

    if (!app->centered) {
        app->centered = TRUE;
        gtk_widget_add_tick_callback(GTK_WIDGET(app->win), center_paned, app, NULL);
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_timeout_add(500, dump_allocations, app);
    }
}

static GtkWidget *
build_editor(App *app)
{
    GtkSourceLanguageManager *lang_mgr;
    GtkSourceLanguage        *language;
    GtkWidget                *scrolled;
    GtkWidget                *view;
    const char               *demo =
        "/* demo.c — coloration GtkSourceView */\n"
        "#include <stdio.h>\n"
        "\n"
        "static int fib(int n)\n"
        "{\n"
        "    if (n < 2)\n"
        "        return n;\n"
        "    return fib(n - 1) + fib(n - 2);\n"
        "}\n"
        "\n"
        "int main(void)\n"
        "{\n"
        "    for (int i = 0; i < 10; i++)\n"
        "        printf(\"fib(%d) = %d\\n\", i, fib(i));\n"
        "    return 0;\n"
        "}\n";

    lang_mgr = gtk_source_language_manager_get_default();
    language = gtk_source_language_manager_get_language(lang_mgr, "c");

    app->buffer = gtk_source_buffer_new(NULL);
    gtk_source_buffer_set_language(app->buffer, language);
    gtk_source_buffer_set_highlight_syntax(app->buffer, TRUE);

    view = gtk_source_view_new_with_buffer(app->buffer);
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(view), 4);
    gtk_source_view_set_auto_indent(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_insert_spaces_instead_of_tabs(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_show_right_margin(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_right_margin_position(GTK_SOURCE_VIEW(view), 80);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_NONE);

    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(app->buffer), demo, -1);

    g_signal_connect(app->buffer, "notify::cursor-position",
                     G_CALLBACK(on_cursor_notify), app);

    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);
    return scrolled;
}

static void
on_activate(GtkApplication *gtk_app, gpointer data)
{
    App      *app = data;
    GtkWidget *header;
    GtkWidget *paned;
    GtkWidget *side;
    GtkWidget *editor;
    GtkWidget *statusbar;
    GtkWidget *sep;
    AdwStyleManager *style_mgr;

    app->win = GTK_WINDOW(gtk_application_window_new(gtk_app));
    gtk_window_set_title(app->win, "SIEB - CodeDashBoard");
    gtk_window_set_default_size(app->win, 1280, 800);

    /* HeaderBar (sans bouton Ouvrir : l'explorateur suffit). */
    header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(app->win, header);

    /* Panneau latéral : Explorateur (roots + contenu des projets). */
    app->roots = roots_load();
    side = build_roots_panel(app);

    /* Actions du menu "+" du panneau Explorateur. */
    {
        const GActionEntry win_actions[] = {
            { "add-structure", on_add_structure, NULL, NULL, NULL, { 0 } },
            { "add-project",   on_add_project,   NULL, NULL, NULL, { 0 } },
        };
        g_action_map_add_action_entries(G_ACTION_MAP(app->win), win_actions,
                                        G_N_ELEMENTS(win_actions), app);
    }

    /* Éditeur */
    editor = build_editor(app);

    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), side);
    gtk_paned_set_end_child(GTK_PANED(paned), editor);
    /* Le paned doit occuper toute la hauteur restante (sinon ~80px écrasés). */
    gtk_widget_set_vexpand(paned, TRUE);
    app->paned = GTK_PANED(paned);

    /* Moitié/moitié au premier affichage, puis ajustable à la poignée. */
    g_signal_connect(app->win, "map", G_CALLBACK(on_first_map), app);

    /* Suivi du thème clair/sombre système : AdwStyleManager suit
     * automatiquement color-scheme (via le portal), on écoute son état. */
    style_mgr = adw_style_manager_get_default();
    g_signal_connect(style_mgr, "notify::dark",
                     G_CALLBACK(on_theme_notify), app);
    update_style_scheme(app);

    /* Barre de statut */
    app->status_file = GTK_LABEL(gtk_label_new("demo.c"));
    gtk_label_set_xalign(app->status_file, 0.0);
    gtk_widget_set_hexpand(GTK_WIDGET(app->status_file), TRUE);

    app->status_pos = GTK_LABEL(gtk_label_new("1:1"));
    gtk_label_set_xalign(app->status_pos, 1.0);

    statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    app->statusbar = statusbar;
    gtk_widget_set_margin_start(statusbar, 8);
    gtk_widget_set_margin_end(statusbar, 8);
    gtk_widget_set_margin_top(statusbar, 2);
    gtk_widget_set_margin_bottom(statusbar, 2);
    sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_start(sep, 8);
    gtk_widget_set_margin_end(sep, 8);
    gtk_box_append(GTK_BOX(statusbar), GTK_WIDGET(app->status_file));
    gtk_box_append(GTK_BOX(statusbar), sep);
    gtk_box_append(GTK_BOX(statusbar), GTK_WIDGET(app->status_pos));

    /* Assemblage final */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), paned);
    gtk_box_append(GTK_BOX(root), statusbar);

    gtk_window_set_child(app->win, root);

    update_status(app);

    /* Boot sur le dernier fichier ouvert, s'il existe encore. */
    {
        char *last = roots_read_last_file();

        if (last != NULL) {
            if (g_file_test(last, G_FILE_TEST_IS_REGULAR))
                load_file(app, last);
            g_free(last);
        }
    }

    gtk_window_present(app->win);
}

int
main(int argc, char **argv)
{
    GtkApplication *gtk_app;
    App            *app;
    int             status;

    app = g_new0(App, 1);
    app->sel_anchor = GTK_INVALID_LIST_POSITION;

    gtk_app = GTK_APPLICATION(adw_application_new("org.sieb.code-dashboard",
                                                  G_APPLICATION_DEFAULT_FLAGS));
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), app);

    status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    g_object_unref(gtk_app);
    g_free(app);

    return status;
}
