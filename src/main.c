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
#include <gdk/gdkkeysyms.h>
#include <gtksourceview/gtksource.h>
#include <adwaita.h>
#include <glib/gstdio.h>
#include "roots.h"
#include "fslist.h"
#include "dirty.h"
#include "diffbar.h"
#include "bashpanel.h"
#include "layout.h"

/* Un fichier ouvert garde son propre buffer (l'historique undo survit à la
 * navigation) et son propre baseline « propre ». */
typedef struct {
    GtkSourceBuffer *buffer;
    char            *saved_content; /* contenu « propre » de référence */
} PerFile;

typedef struct {
    GtkWindow         *win;
    GtkPaned          *paned;
    GtkSourceBuffer   *buffer;    /* buffer courant (dérivé de files[]) */
    GtkWidget         *source_view; /* la GtkSourceView (change de buffer) */
    GHashTable        *files;     /* chemin -> PerFile* (buffer + baseline) */
    Layout            *layout;    /* modèle du tiling (source de vérité) */
    GtkWidget         *layout_holder; /* conteneur du rendu paned */
    GtkWidget         *layout_root;   /* arbre GtkPaned rendu */
    GListStore        *roots;
    GtkMultiSelection  *selection;
    GtkTreeListModel   *tree_model;
    GtkWidget          *explorer_scrolled;
    GdkModifierType    last_click_mods;
    guint              sel_anchor; /* dernière ancre (clic exclusif) */
    GtkLabel          *status_file;
    GtkLabel          *status_pos;
    GtkLabel          *status_mod; /* témoin non sauvegardé (« ● ») */
    GtkWidget         *statusbar;
    RootEntry         *pending_remove;
    RootKind           pending_kind;
    gboolean           centered;
    /* Chemin du fichier courant de l'éditeur (NULL = demo, rien à sauver). */
    char              *current_file;
    /* Contenu « propre » de référence du fichier courant (celui sur disque
     * au dernier chargement/sauvegarde). Le sale = buffer ≠ ce contenu. */
    char              *saved_content;
    /* Fichiers non sauvegardés (témoin par fichier + contenu en attente). */
    DirtyStore        *dirty;
    /* Barre de diff (vert/rouge) sur la scrollbar du fichier courant. */
    GtkWidget         *diffbar;
    guint              diff_timer; /* debounce du recalcul de diff */
    guint              render_idle; /* re-rendu différé (destruction sûre) */
    /* Ignore les signaux pendant un chargement (évite un sale transitoire). */
    gboolean           suppress_dirty;
    /* Source de vérité de la multi-sélection : set de chemins (clés
     * g_strdup/g_free). GTK ne fait qu'afficher. */
    GHashTable        *multi_paths;
    /* Évite la récursion selection-changed pendant nos propres mutations. */
    gboolean           selection_guard;
} App;

/* Libère un PerFile (buffer + baseline). */
static void
per_file_free(gpointer data)
{
    PerFile *pf = data;

    if (pf == NULL)
        return;
    g_free(pf->saved_content);
    g_object_unref(pf->buffer);
    g_free(pf);
}

/* ------------------------------------------------------------------ */
/* Statut                                                              */
/* ------------------------------------------------------------------ */

static void
update_status(App *app)
{
    GtkTextIter iter;
    gchar      *text;

    /* Aucune vue éditeur n'existe (layout sans tuile editor) : rien à
     * afficher, l'état (buffer) peut quand même exister. */
    if (app->buffer == NULL)
        return;
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
static void trace_destroy(GtkWidget *w, gpointer data);
static void rebuild_explorer(App *app);
static void on_selection_changed(GtkSelectionModel *model, guint position,
                                 guint n_items, gpointer data);
static char *explorer_path_at_pos(App *app, guint pos);
static void selection_apply_from_paths(App *app);
static void selection_sync_from_model(App *app);
static void on_save_activated(GSimpleAction *action, GVariant *param, gpointer data);
static void update_modified_indicator(App *app);
static void sync_current_dirty(App *app);
static void schedule_diff(App *app);
static void update_diff(App *app);
static gboolean item_is_dirty(App *app, gpointer item);
static void create_roots_state(App *app);
static GtkWidget *build_roots_view(App *app);
static gboolean test_split_idle(gpointer data);
static gboolean test_grid_idle(gpointer data);
static gboolean test_quit_idle(gpointer data);
static void recompute_dirty(App *app);
static void on_buffer_changed(GtkTextBuffer *buffer, gpointer data);
static void update_style_scheme(App *app);
static void render_layout(App *app);
static void set_paned_positions(App *app);
static void on_paned_position(GObject *paned, GParamSpec *pspec,
                              gpointer data);
static GtkWidget *build_editor(App *app);
static GtkWidget *build_roots_panel(App *app);

/* ------------------------------------------------------------------ */
/* Chargement de fichier                                               */
/* ------------------------------------------------------------------ */

static void
load_file(App *app, const char *path)
{
    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: load_file path=%s\n", path);

    GtkSourceLanguageManager *lang_mgr;
    GtkSourceLanguage        *language;
    PerFile                  *pf;

    /* Avant de quitter le buffer, on persiste le fichier précédent s'il
     * était sale (son contenu est déjà frais dans dirty_store). */
    if (app->current_file != NULL
        && dirty_contains(app->dirty, app->current_file))
        dirty_persist_now(app->dirty);

    pf = g_hash_table_lookup(app->files, path);

    if (pf == NULL) {
        /* Premier chargement de ce fichier dans la session : on crée son
         * propre buffer (historique undo) et on le remplit. */
        const char *cached;
        gboolean    restore_dirty;
        gchar      *content = NULL;
        gsize       len = 0;
        GError     *error = NULL;

        pf = g_new0(PerFile, 1);
        pf->buffer = gtk_source_buffer_new(NULL);
        gtk_source_buffer_set_highlight_syntax(pf->buffer, TRUE);
        /* Signaux éditeur (statut curseur + détection de dirty). */
        g_signal_connect(pf->buffer, "notify::cursor-position",
                         G_CALLBACK(on_cursor_notify), app);
        g_signal_connect(pf->buffer, "changed",
                         G_CALLBACK(on_buffer_changed), app);

        /* Contenu en attente éventuel : on restaure, pas de re-lecture
         * disque. */
        cached = dirty_content(app->dirty, path);
        restore_dirty = cached != NULL;

        if (restore_dirty) {
            /* Référence « propre » = BASELINE d'origine (celui dont
             * découlent les modifications), et non le fichier disque
             * courant : un changement externe du fichier source ne doit
             * pas apparaître comme un nouveau dirty. */
            const char *bl = dirty_baseline(app->dirty, path);

            content = g_strdup(bl != NULL ? bl : "");
            len = strlen(content);
        } else {
            /* Chargement propre : la référence est le contenu disque. */
            if (!g_file_get_contents(path, &content, &len, &error)) {
                g_printerr("SIEB - CodeDashBoard: %s\n", error->message);
                g_error_free(error);
                g_object_unref(pf->buffer);
                g_free(pf);
                return;
            }

            /* Fichier binaire ou encodage non UTF-8 : l'éditeur ne peut
             * pas l'afficher — on prévient au lieu de casser le buffer. */
            if (!g_utf8_validate(content, len, NULL)) {
                GtkAlertDialog *alert = gtk_alert_dialog_new(
                    "Fichier binaire ou encodage non UTF-8 :\n%s", path);

                gtk_alert_dialog_show(alert, app->win);
                g_free(content);
                g_object_unref(pf->buffer);
                g_free(pf);
                return;
            }
        }

        /* Détection de la langue par extension (retombe sur C). */
        lang_mgr = gtk_source_language_manager_get_default();
        language = gtk_source_language_manager_guess_language(lang_mgr, path, NULL);
        if (language == NULL)
            language = gtk_source_language_manager_get_language(lang_mgr, "c");
        gtk_source_buffer_set_language(pf->buffer, language);

        /* Pendant le chargement on ignore les signaux pour ne pas marquer
         * un sale transitoire. */
        app->suppress_dirty = TRUE;
        gtk_text_buffer_set_text(GTK_TEXT_BUFFER(pf->buffer),
                                 restore_dirty ? cached : content, -1);
        app->suppress_dirty = FALSE;

        pf->saved_content = content; /* devient la référence « propre » */
        /* Ref EXPLICITE détenue par PerFile : le buffer (flottant) est
         * sinké par la vue éditeur ; sans notre ref il mourrait avec elle
         * au retrait de la tuile (l'état doit survivre aux vues). */
        g_object_ref(pf->buffer);
        g_hash_table_insert(app->files, g_strdup(path), pf);
    }

    /* Bascule sur le buffer de ce fichier (réutilisation : son undo est
     * préservé ; app->buffer/saved_content dérivent de PerFile). */
    app->buffer = pf->buffer;
    app->saved_content = pf->saved_content;
    /* L'état existe toujours ; l'affichage seulement s'il y a une vue. */
    if (app->source_view != NULL)
        gtk_text_view_set_buffer(GTK_TEXT_VIEW(app->source_view),
                                 GTK_TEXT_BUFFER(pf->buffer));
    /* Le buffer (neuf ou réutilisé) doit suivre le thème clair/sombre. */
    update_style_scheme(app);

    /* Le fichier ouvert devient la cible de Ctrl+S. */
    g_free(app->current_file);
    app->current_file = g_strdup(path);

    gtk_label_set_text(app->status_file, path);
    gtk_label_set_ellipsize(app->status_file, PANGO_ELLIPSIZE_MIDDLE);
    update_status(app);

    if (dirty_contains(app->dirty, path)) {
        /* Fichier sale (restauré ou rouvert) : on garde « non sauvegardé ». */
        gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(app->buffer), TRUE);
    } else {
        gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(app->buffer), FALSE);
        dirty_clear(app->dirty, path); /* propre après un chargement disque */
    }

    /* Mémorise le dernier fichier ouvert (réouverture au boot). */
    roots_write_last_file(path);

    /* L'explorateur suit l'éditeur : déplie jusqu'au fichier, sélectionne. */
    reveal_path(app, path);

    update_modified_indicator(app);
    recompute_dirty(app);
    update_diff(app);
}

/* ------------------------------------------------------------------ */
/* Témoin non sauvegardé + sauvegarde (Ctrl+S)                          */
/* ------------------------------------------------------------------ */

/* Contenu courant du buffer (g_strdup). */
static char *
buffer_text(App *app)
{
    GtkTextIter start;
    GtkTextIter end;

    if (app->buffer == NULL)
        return NULL;
    gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(app->buffer), &start);
    gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(app->buffer), &end);
    return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(app->buffer), &start, &end, TRUE);
}

/* Met à jour le titre de la fenêtre et le point de la barre de statut :
 * le témoin ne vaut « sale » que pour le fichier COURANT (∈ dirty_store). */
static void
update_modified_indicator(App *app)
{
    gboolean dirty = app->current_file != NULL
                     && dirty_contains(app->dirty, app->current_file);

    if (app->status_mod != NULL)
        gtk_label_set_text(app->status_mod, dirty ? "●" : "");

    if (app->current_file != NULL) {
        char *title = g_strdup_printf("%s%s — SIEB - CodeDashBoard",
                                      app->current_file, dirty ? "*" : "");

        gtk_window_set_title(app->win, title);
        g_free(title);
    } else {
        gtk_window_set_title(app->win, "SIEB - CodeDashBoard");
    }
}

/* TRUE si l'item est « sale » : fichier présent dans dirty_store, ou
 * dossier/root contenant (transitivement) un fichier sale. */
static gboolean
item_is_dirty(App *app, gpointer item)
{
    const char *path;

    if (g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)) {
        path = ((RootEntry *)item)->path;
    } else {
        FileEntry *f = item;

        if (!f->is_dir)
            return g_hash_table_contains(app->dirty->store, f->path);
        path = f->path;
    }
    /* Dossier / structure / projet : un descendant sale suffit. */
    return dirty_under(app->dirty, path);
}

/* Recalcule et affiche l'indicateur de chaque ligne de l'arbre. Pas de
 * rebuild global : on met à jour en place les widgets témoins (le scroll
 * et le curseur de l'éditeur sont préservés). */
static void
recompute_dirty(App *app)
{
    GtkTreeListModel *tree = app->tree_model;
    guint             n;

    if (tree == NULL)
        return;
    n = g_list_model_get_n_items(G_LIST_MODEL(tree));
    for (guint i = 0; i < n; i++) {
        GtkTreeListRow *row = g_list_model_get_item(G_LIST_MODEL(tree), i);
        gboolean        dirty;
        GtkWidget      *indicator;

        if (row == NULL)
            continue;
        {
            gpointer item = gtk_tree_list_row_get_item(row);

            dirty = item_is_dirty(app, item);
            if (g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)) {
                RootEntry *e = item;

                e->dirty = dirty;
                indicator = e->indicator;
            } else {
                FileEntry *f = item;

                f->dirty = dirty;
                indicator = f->indicator;
            }
        }
        if (indicator != NULL)
            gtk_label_set_text(GTK_LABEL(indicator), dirty ? "●" : "");
        g_object_unref(row);
    }
}

/* Le buffer a changé (frappe/annulation) : on recalcule l'état sale du
 * fichier courant par comparaison au contenu disque de référence. */
static void
on_buffer_changed(GtkTextBuffer G_GNUC_UNUSED *buffer, gpointer data)
{
    App *app = data;

    if (app->suppress_dirty)
        return; /* chargement en cours : ne pas écraser l'ancien contenu sale */
    sync_current_dirty(app);
    schedule_diff(app);
}

/* Sale = buffer ≠ contenu disque de référence. Détecte aussi la transition
 * propre -> sale et sale -> propre (ex: taper « 555 » puis l'effacer) pour
 * n'afficher / persister que si l'état change. */
static void
sync_current_dirty(App *app)
{
    gchar    *text;
    gboolean  is;
    gboolean  was;

    if (app->current_file == NULL || app->saved_content == NULL)
        return;

    text = buffer_text(app);
    is = g_strcmp0(text, app->saved_content) != 0;
    was = dirty_contains(app->dirty, app->current_file);

    if (is)
        dirty_mark(app->dirty, app->current_file, text, app->saved_content);
    else
        dirty_clear(app->dirty, app->current_file);
    g_free(text);

    if (is != was) {
        /* L'état a basculé : on rafraîchit témoins + persistance. */
        update_modified_indicator(app);
        recompute_dirty(app);
        dirty_schedule_persist(app->dirty);
    }
}

#define DIFF_DEBOUNCE_MS 150

static gboolean
update_diff_timeout(gpointer data)
{
    App *app = data;

    app->diff_timer = 0;
    update_diff(app);
    return G_SOURCE_REMOVE;
}

/* Recalcule la barre de diff après une courte accalmie de frappe. */
static void
schedule_diff(App *app)
{
    if (app->diff_timer != 0)
        g_source_remove(app->diff_timer);
    app->diff_timer = g_timeout_add(DIFF_DEBOUNCE_MS, update_diff_timeout, app);
}

/* Diff référence disque <-> buffer, affiché dans la barre (vert/rouge). */
static void
update_diff(App *app)
{
    GPtrArray *ranges;
    guint      total;

    if (app->diffbar == NULL)
        return;
    ranges = g_ptr_array_new_with_free_func(g_free);
    if (app->current_file != NULL && app->saved_content != NULL)
        siebd_diff_compute(app->saved_content, buffer_text(app), ranges, &total);
    else
        total = 0;
    siebd_diff_bar_set_ranges(SIEBD_DIFF_BAR(app->diffbar), ranges, total);
    g_ptr_array_free(ranges, TRUE);
}

/* Ctrl+S : écrit le contenu du buffer dans le fichier courant. */
static void
on_save_activated(GSimpleAction G_GNUC_UNUSED *action,
                  GVariant G_GNUC_UNUSED *param, gpointer data)
{
    App        *app = data;
    GtkTextIter start;
    GtkTextIter end;
    char       *text;
    GError     *error = NULL;

    if (app->current_file == NULL)
        return; /* demo : pas de fichier à sauvegarder */

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: save -> %s\n", app->current_file);

    gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(app->buffer), &start);
    gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(app->buffer), &end);
    text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(app->buffer), &start, &end, TRUE);

    if (!g_file_set_contents(app->current_file, text, -1, &error)) {
        GtkAlertDialog *alert = gtk_alert_dialog_new(
            "Impossible d'enregistrer :\n%s", error->message);

        gtk_alert_dialog_show(alert, app->win);
        g_error_free(error);
    } else {
        /* Le contenu disque devient la nouvelle référence « propre ». */
        PerFile *pf = g_hash_table_lookup(app->files, app->current_file);

        if (pf != NULL) {
            g_free(pf->saved_content);
            pf->saved_content = buffer_text(app);
            app->saved_content = pf->saved_content;
        }
        dirty_clear(app->dirty, app->current_file);
        gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(app->buffer), FALSE);
        update_modified_indicator(app);
        recompute_dirty(app);
        update_diff(app);
        dirty_persist_now(app->dirty);
    }
    g_free(text);
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
    GtkWidget  *indicator = gtk_label_new(NULL);
    GtkGesture *gesture = gtk_gesture_click_new();

    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    /* Témoin non sauvegardé, tout à droite de la ligne (le label hexpand
     * pousse l'indicateur vers la droite). */
    gtk_widget_set_margin_start(indicator, 8);
    gtk_box_append(GTK_BOX(box), indicator);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(item, expander);

    g_object_set_data(G_OBJECT(item), "icon", icon);
    g_object_set_data(G_OBJECT(item), "label", label);
    g_object_set_data(G_OBJECT(item), "indicator", indicator);

    /* Clic droit sur la ligne → menu de suppression. */
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 3);
    g_signal_connect(gesture, "pressed", G_CALLBACK(on_row_pressed), data);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(gesture));

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: row setup (gesture bouton 3 attaché)\n");
}

static void
on_row_bind(GtkListItemFactory G_GNUC_UNUSED *factory, GtkListItem *item,
            gpointer data)
{
    App           *app = data;
    GtkTreeListRow *row = gtk_list_item_get_item(item);
    GtkWidget      *icon = g_object_get_data(G_OBJECT(item), "icon");
    GtkWidget      *label = g_object_get_data(G_OBJECT(item), "label");
    GtkWidget      *indicator = g_object_get_data(G_OBJECT(item), "indicator");
    GtkWidget      *expander = gtk_list_item_get_child(item);
    gboolean        dirty;

    if (g_type_is_a(G_TYPE_FROM_INSTANCE(gtk_tree_list_row_get_item(row)),
                    ROOT_TYPE_ENTRY)) {
        RootEntry *entry = gtk_tree_list_row_get_item(row);

        gtk_image_set_from_icon_name(GTK_IMAGE(icon),
            entry->kind == ROOT_STRUCTURE ? "folder-symbolic"
                                          : "folder-documents-symbolic");
        gtk_label_set_text(GTK_LABEL(label), entry->basename);
        dirty = item_is_dirty(app, entry);
        entry->dirty = dirty;
        entry->indicator = indicator;
    } else {
        FileEntry *f = gtk_tree_list_row_get_item(row);

        gtk_image_set_from_icon_name(GTK_IMAGE(icon),
            f->is_dir ? "folder-symbolic" : "text-x-generic-symbolic");
        gtk_label_set_text(GTK_LABEL(label), f->name);
        dirty = item_is_dirty(app, f);
        f->dirty = dirty;
        f->indicator = indicator;
    }
    gtk_label_set_text(GTK_LABEL(indicator), dirty ? "●" : "");
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
    /* pos+1 : l'index 0 est valide, GUINT_TO_POINTER(0) == NULL. */
    g_object_set_data(G_OBJECT(expander), "sieb-pos",
                      GUINT_TO_POINTER(gtk_list_item_get_position(item) + 1));
}

static void
on_row_unbind(GtkListItemFactory G_GNUC_UNUSED *factory, GtkListItem *item,
              gpointer G_GNUC_UNUSED data)
{
    GtkTreeListRow *row = gtk_list_item_get_item(item);

    /* La row est recyclée : on détache l'indicateur de l'entrée pour que
     * recompute_dirty n'écrive pas dans un widget qui montre autre chose. */
    if (row != NULL) {
        gpointer it = gtk_tree_list_row_get_item(row);

        if (g_type_is_a(G_TYPE_FROM_INSTANCE(it), ROOT_TYPE_ENTRY))
            ((RootEntry *)it)->indicator = NULL;
        else
            ((FileEntry *)it)->indicator = NULL;
    }
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

    /* ÉTAT : modèle + sélection, créé une fois et partagé par toutes les
     * vues « explorer » (les tuiles multiples montrent la même sélection). */
    if (app->tree_model == NULL)
        create_roots_state(app);

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
     * l'expansion n'arrive qu'au clic de l'utilisateur, sans limite.
     * VUE : chaque tuile a son scrolled + sa liste (état partagé). */
    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    if (g_getenv("SIEB_DEBUG") != NULL)
        g_signal_connect(scrolled, "destroy", G_CALLBACK(trace_destroy), app);
    app->explorer_scrolled = scrolled;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled),
                                  build_roots_view(app));

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
                     gpointer data)
{
    App *app = data;

    if (g_getenv("SIEB_DEBUG") != NULL) {
        GtkBitset *bits = gtk_selection_model_get_selection(model);
        g_printerr("SIEB: sélection -> %lu éléments (changed pos=%u n=%u)\n",
                   (unsigned long)gtk_bitset_get_size(bits), position, n_items);
        gtk_bitset_unref(bits);
    }

    /* Source de vérité = multi_paths ; GTK n'est que le rendu. Dès qu'il
     * modifie la sélection seul (souris, ListView), on la réécrit depuis
     * la table. Le guard couvre nos propres mutations (pas de récursion). */
    if (app->selection_guard)
        return;
    if (g_hash_table_size(app->multi_paths) >= 1)
        selection_apply_from_paths(app);
}

/* Chemin (g_strdup) de la row à pos, ou NULL si invalide. */
static char *
explorer_path_at_pos(App *app, guint pos)
{
    GtkTreeListRow *row;

    if (app->tree_model == NULL)
        return NULL;
    row = gtk_tree_list_model_get_row(app->tree_model, pos);
    if (row == NULL)
        return NULL;
    {
        gpointer item = gtk_tree_list_row_get_item(row);
        char    *path = g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)
                        ? g_strdup(((RootEntry *)item)->path)
                        : g_strdup(((FileEntry *)item)->path);

        g_object_unref(row);
        return path;
    }
}

/* Réécrit la sélection GTK pour qu'elle reflète exactement multi_paths.
 * guard posé pendant la mutation : les selection-changed imbriqués sont
 * ignorés (la sélection correspond déjà à la table à la fin). */
static void
selection_apply_from_paths(App *app)
{
    GListModel *model = G_LIST_MODEL(app->tree_model);
    guint       n;

    app->selection_guard = TRUE;
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(app->selection));
    n = g_list_model_get_n_items(model);
    for (guint i = 0; i < n; i++) {
        char    *path = explorer_path_at_pos(app, i);
        gboolean in_set = path != NULL
                          && g_hash_table_contains(app->multi_paths, path);

        g_free(path);
        if (in_set)
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(app->selection),
                                            i, FALSE);
    }
    app->selection_guard = FALSE;
}

/* Re-synchronise multi_paths depuis la sélection GTK courante (utilisé
 * après un Shift+clic pour ne pas désaligner la table). */
static void
selection_sync_from_model(App *app)
{
    GtkBitset    *bits = gtk_selection_model_get_selection(
        GTK_SELECTION_MODEL(app->selection));
    GtkBitsetIter iter;
    guint         pos = 0;

    g_hash_table_remove_all(app->multi_paths);
    if (gtk_bitset_iter_init_first(&iter, bits, &pos)) {
        do {
            char *path = explorer_path_at_pos(app, pos);

            if (path != NULL)
                g_hash_table_add(app->multi_paths, path);
            else
                g_free(path);
        } while (gtk_bitset_iter_next(&iter, &pos));
    }
    gtk_bitset_unref(bits);
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
        /* Clic simple : on réinitialise la multi à {path sous le curseur}
         * AVANT que GTK ne fasse son select exclusif — sinon le handler
         * selection-changed ré-appliquerait la vieille multi. */
        app->sel_anchor = pos;
        g_hash_table_remove_all(app->multi_paths);
        if (pos != GTK_INVALID_LIST_POSITION) {
            char *p = explorer_path_at_pos(app, pos);

            if (p != NULL)
                g_hash_table_add(app->multi_paths, p);
        }
        selection_apply_from_paths(app);
        return; /* la vue gère normalement le reste */
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
        selection_sync_from_model(app);
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

    /* Ctrl : toggle du chemin dans multi_paths (source de vérité), puis
     * on réécrit la sélection GTK. On ne se fie plus à is_selected(pos) :
     * le bitset GTK peut avoir été écrasé par le passage du pointeur. */
    {
        char *path = explorer_path_at_pos(app, pos);

        if (path == NULL)
            return;
        if (g_hash_table_contains(app->multi_paths, path))
            g_hash_table_remove(app->multi_paths, path);
        else
            g_hash_table_add(app->multi_paths, path);
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: Ctrl+clic -> multi_paths size=%lu\n",
                       (unsigned long)g_hash_table_size(app->multi_paths));
        selection_apply_from_paths(app);
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

/* Sélectionne la row à l'ouverture d'un fichier : on réinitialise la
 * multi à {chemin} (même contrat qu'un clic simple), puis on réapplique.
 * C'est la source de vérité multi_paths qui est mise à jour, pas le
 * bitset GTK directement. */
static void
select_row(App *app, GtkTreeListRow *row)
{
    gpointer     item = gtk_tree_list_row_get_item(row);
    const char  *path = g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)
                        ? ((RootEntry *)item)->path
                        : ((FileEntry *)item)->path;

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: select_row (open) reset multi -> %s\n", path);

    g_hash_table_remove_all(app->multi_paths);
    g_hash_table_add(app->multi_paths, g_strdup(path));
    selection_apply_from_paths(app);
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
    GListModel *model;
    guint n;

    /* Layout sans explorateur : rien à déplier/sélectionner (l'état peut
     * être créé plus tard par la première tuile « explorer »). */
    if (tree == NULL)
        return;
    model = G_LIST_MODEL(tree);
    n = g_list_model_get_n_items(model);

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
    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: reveal_path path=%s\n", file_path);
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

/* ÉTAT de l'explorateur : modèle + sélection. Créé une seule fois, partagé
 * par toutes les vues « explorer » ; retirer une tuile ne détruit rien.
 * La sélection multi (app->multi_paths) est la source de vérité. */
/* Trace d'état (SIEB_DEBUG) : adresses + refcounts des objets d'état
 * partagés — permet de voir quand tree_model/selection deviennent
 * invalides (double-unref / use-after-free). */
static void
trace_destroy(GtkWidget *w, gpointer data)
{
    App *app = data;

    if (g_getenv("SIEB_DEBUG") == NULL)
        return;
    g_printerr("SIEB: destroy %s @%p (refs selection=%d)\n",
               G_OBJECT_TYPE_NAME(w), (void *)w,
               app->selection != NULL
                   ? (int)((GObject *)app->selection)->ref_count : -1);
}

static void
trace_state(App *app, const char *where)
{
    if (g_getenv("SIEB_DEBUG") == NULL)
        return;
    g_printerr("SIEB: [%s] tree_model=%p selection=%p (refs=%d) roots=%p "
               "layout=%p\n",
               where, (void *)app->tree_model, (void *)app->selection,
               app->selection != NULL ? (int)((GObject *)app->selection)->ref_count
                                      : -1,
               (void *)app->roots, (void *)app->layout);
}

static void
create_roots_state(App *app)
{
    GtkTreeListModel *tree_model;

    app->sel_anchor = GTK_INVALID_LIST_POSITION;
    tree_model = gtk_tree_list_model_new(G_LIST_MODEL(app->roots), FALSE, FALSE,
                                         roots_create_child, NULL, NULL);
    /* Ref explicite : l'état nous appartient. Sans elle, la destruction
     * des vues (qui unref le modèle/la sélection à leur dispose) peut
     * libérer l'objet alors que app->selection le pointe encore. */
    app->tree_model = tree_model;
    g_object_ref(app->tree_model);
    app->selection = gtk_multi_selection_new(G_LIST_MODEL(tree_model));
    g_object_ref(app->selection);
    selection_changed_handler_id = g_signal_connect(app->selection, "selection-changed",
                                                    G_CALLBACK(on_selection_changed), app);

    /* Après (re)création du modèle, les positions changent mais les chemins
     * restent : on réapplique la multi (source de vérité) si elle existe. */
    if (g_hash_table_size(app->multi_paths) >= 1)
        selection_apply_from_paths(app);
    trace_state(app, "create_roots_state");
}

/* VUE de l'explorateur : une GtkListView par tuile, sur l'état partagé. */
static GtkWidget *
build_roots_view(App *app)
{
    GtkListItemFactory *factory;
    GtkWidget           *view;

    factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_row_setup), app);
    g_signal_connect(factory, "bind", G_CALLBACK(on_row_bind), app);
    g_signal_connect(factory, "unbind", G_CALLBACK(on_row_unbind), app);

    view = gtk_list_view_new(GTK_SELECTION_MODEL(app->selection),
                             GTK_LIST_ITEM_FACTORY(factory));
    /* GTK 4.22 libère 2 refs du model (et du selection) à la mort d'une
     * GtkListView alors qu'il n'en a pris qu'une : le compteur de S1
     * descend de 2 par vue morte. Avec plusieurs vues explorer partagées,
     * S1 finit par être libéré alors que app->selection le pointe encore
     * (use-after-free). On compense : +1 ref par vue créée ; elles ne sont
     * jamais libérées (fuite bornée par le nombre de vues créées, l'état
     * reste vivant pour toute la durée de vie de l'application). */
    g_object_ref(app->selection);
    if (g_getenv("SIEB_DEBUG") != NULL) {
        g_printerr("SIEB: build_roots_view selection refs apres new=%d\n",
                   app->selection != NULL
                       ? (int)((GObject *)app->selection)->ref_count : -1);
        g_signal_connect(view, "destroy", G_CALLBACK(trace_destroy), app);
    }
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
    return view;
}

/* Reconstruit entièrement l'explorateur (après création/suppression/
 * renommage sur le disque) et restaure les dossiers ouverts. */
static void
rebuild_explorer(App *app)
{
    GPtrArray *expanded = g_ptr_array_new_with_free_func(g_free);

    if (app->tree_model != NULL)
        collect_expanded(G_LIST_MODEL(app->tree_model), expanded);

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: rebuild_explorer (dossiers ouverts=%u)\n",
                   expanded->len);

    /* L'ancien état (modèle + sélection) est remplacé : on libère nos
     * refs explicites. Les vues existantes gardent leur propre ref. */
    if (app->tree_model != NULL)
        g_object_unref(app->tree_model);
    if (app->selection != NULL)
        g_object_unref(app->selection);

    create_roots_state(app);

    /* Met à jour la dernière vue créée (app->explorer_scrolled). Avec
     * plusieurs vues « explorer », seules les vues suivantes reflètent le
     * nouvel état — limitation acceptée (Phase 1). */
    if (app->explorer_scrolled != NULL) {
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(app->explorer_scrolled),
                                      build_roots_view(app));
        if (g_getenv("SIEB_DEBUG") != NULL)
            g_printerr("SIEB: rebuild_explorer vue créée (n_items=%u)\n",
                       g_list_model_get_n_items(G_LIST_MODEL(app->tree_model)));
    }

    /* Restaure l'état d'expansion. */
    for (guint i = 0; i < expanded->len; i++)
        expand_path(app, expanded->pdata[i]);
    g_ptr_array_free(expanded, TRUE);

    /* Réaffiche les témoins non sauvegardés (les chemins sont inchangés). */
    recompute_dirty(app);
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

/* Ctrl+Z : undo standard du buffer ; quand plus rien à défaire mais que le
 * fichier est encore sale (ex: dirty restauré via set_text, historique vide),
 * un dernier Ctrl+Z revient au baseline et efface tout le dirty. */
static gboolean
on_editor_key_pressed(GtkEventControllerKey G_GNUC_UNUSED *controller,
                      guint keyval, guint G_GNUC_UNUSED keycode,
                      GdkModifierType state, gpointer data)
{
    App *app = data;

    if (keyval != GDK_KEY_z)
        return FALSE;
    if ((state & GDK_CONTROL_MASK) == 0
        || (state & GDK_ALT_MASK) != 0
        || (state & GDK_SHIFT_MASK) != 0) /* Ctrl+Shift+Z = redo, laisser */
        return FALSE;

    if (gtk_text_buffer_get_can_undo(GTK_TEXT_BUFFER(app->buffer))) {
        gtk_text_buffer_undo(GTK_TEXT_BUFFER(app->buffer));
        return TRUE; /* consommé, l'undo standard a fait le boulot */
    }

    /* Plus d'undo dans le buffer : si le fichier est sale, on revient au
     * baseline (ce dernier Ctrl+Z « efface le dirty »). */
    if (app->current_file != NULL && app->saved_content != NULL
        && dirty_contains(app->dirty, app->current_file)) {
        app->suppress_dirty = TRUE;
        gtk_text_buffer_set_text(GTK_TEXT_BUFFER(app->buffer),
                                 app->saved_content, -1);
        app->suppress_dirty = FALSE;
        gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(app->buffer), FALSE);
        sync_current_dirty(app); /* vide le dirty + indicateurs + persist */
        update_diff(app);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Construction de l'UI                                                */
/* ------------------------------------------------------------------ */

/* Au premier affichage, applique les fractions persistées aux poignées. */
static gboolean
center_paned(GtkWidget G_GNUC_UNUSED *widget, GdkFrameClock G_GNUC_UNUSED *clock, gpointer data)
{
    App *app = data;

    /* Tick callback : tourne après le layout du frame — les tailles sont
     * réelles. On réapplique les fractions persistées du modèle. */
    if (app->layout_root == NULL
        || gtk_widget_get_width(app->layout_root) <= 0)
        return G_SOURCE_CONTINUE;

    set_paned_positions(app);
    return G_SOURCE_REMOVE; /* une seule fois */
}

/* Debug (SIEB_DEBUG=1) : répartition verticale des allocations. */
static gboolean
dump_allocations(gpointer data)
{
    App       *app = data;
    GtkWidget *root = gtk_window_get_child(app->win);

    fprintf(stderr,
            "SIEB: win %dx%d | root %dx%d | layout %dx%d | statusbar %dx%d\n",
            gtk_widget_get_width(GTK_WIDGET(app->win)),
            gtk_widget_get_height(GTK_WIDGET(app->win)),
            gtk_widget_get_width(root), gtk_widget_get_height(root),
            gtk_widget_get_width(app->layout_root),
            gtk_widget_get_height(app->layout_root),
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
    GtkWidget                *overlay;
    GtkWidget                *view;

    /* ÉTAT : le buffer (contenu + undo) vit dans App, créé une seule fois.
     * Chaque tuile « editor » n'est qu'une vue sur ce buffer ; retirer une
     * tuile ne détruit pas l'éditeur (le buffer survit). */
    if (app->buffer == NULL) {
        const char *demo =
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
        gtk_text_buffer_set_text(GTK_TEXT_BUFFER(app->buffer), demo, -1);
        /* Ref explicite détenue par App : le buffer demo (flottant) est
         * sinké par la vue ; il doit survivre au retrait de toutes les
         * vues éditeur (l'état vit plus longtemps que les tuiles). */
        g_object_ref(app->buffer);

        g_signal_connect(app->buffer, "notify::cursor-position",
                         G_CALLBACK(on_cursor_notify), app);
        g_signal_connect(app->buffer, "changed",
                         G_CALLBACK(on_buffer_changed), app);
    }

    /* VUE : une par tuile, attachée au buffer partagé. */
    view = gtk_source_view_new_with_buffer(app->buffer);
    if (g_getenv("SIEB_DEBUG") != NULL)
        g_signal_connect(view, "destroy", G_CALLBACK(trace_destroy), app);
    app->source_view = view;
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(view), 4);
    gtk_source_view_set_auto_indent(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_insert_spaces_instead_of_tabs(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_show_right_margin(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_right_margin_position(GTK_SOURCE_VIEW(view), 80);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_NONE);

    /* Intercepte Ctrl+Z (phase capture) pour gérer le « dernier undo »
     * qui efface le dirty quand l'historique du buffer est épuisé. */
    {
        GtkEventController *key = gtk_event_controller_key_new();

        gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
        g_signal_connect(key, "key-pressed",
                         G_CALLBACK(on_editor_key_pressed), app);
        gtk_widget_add_controller(view, key);
    }

    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);

    /* La barre de diff est superposée à droite, par-dessus la scrollbar :
     * transparente et sans cible d'événements (la scrollbar reste
     * utilisable). */
    overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scrolled);
    app->diffbar = siebd_diff_bar_new();
    gtk_widget_set_halign(app->diffbar, GTK_ALIGN_END);
    gtk_widget_set_valign(app->diffbar, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app->diffbar);
    return overlay;
}

/* ------------------------------------------------------------------ */
/* Système de tuiles (layout dynamique + persistant)                   */
/* ------------------------------------------------------------------ */

/* Contexte d'une action de menu (split/change/remove) pour une tuile. */
typedef struct {
    App     *app;
    Layout  *node;
    gboolean horizontal; /* split : orientation */
    const char *piece;   /* split/change : pièce visée */
    gboolean change;     /* change : remplacer la pièce de la tuile */
    gboolean remove;     /* remove : retirer la tuile */
    GtkWidget *popover;
} TileAction;

/* Crée la VUE d'une pièce. Les morceaux (editor/explorer) ont un état
 * unique partagé (buffer / modèle+sélection) qui survit aux tuiles ;
 * chaque tuile obtient sa propre vue sur cet état. */

static GtkWidget *
create_piece(const char *id, App *app)
{
    GtkWidget *w;

    if (strcmp(id, "editor") == 0)
        w = build_editor(app);
    else if (strcmp(id, "explorer") == 0)
        w = build_roots_panel(app);
    else if (strcmp(id, "bash") == 0)
        w = bash_panel_new(app->roots, app->multi_paths);
    else {
        /* Vide : emplacement réservé, prêt à recevoir un morceau (Phase 2). */
        w = gtk_label_new("Vide\n(emplacement réservé)");
        gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    }

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: create_piece id=%s -> %p\n",
                   id != NULL ? id : "(null)", (void *)w);
    return w;
}

/* Re-rendu différé : le handler de clic du menu est encore en train
 * d'émettre quand on_tile_action tourne — détruire l'ancien arbre
 * (donc le popover et son bouton) dans le handler provoque un
 * use-after-free. Le re-rendu au tick suivant est sûr. */
static gboolean
render_layout_idle(gpointer data)
{
    App *app = data;

    app->render_idle = 0;
    render_layout(app);
    return G_SOURCE_REMOVE;
}

/* Action choisie dans le menu d'une tuile : split, transformation ou
 * remove, puis re-rendu différé + persistance. */
static void
on_tile_action(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    TileAction *act = data;
    App        *app = act->app;
    Layout     *new_root;

    /* Ferme d'abord le popover (le re-rendu va détruire l'ancien arbre
     * qui le contient — ne plus y toucher ensuite). */
    gtk_popover_popdown(GTK_POPOVER(act->popover));

    if (act->remove)
        new_root = layout_remove(app->layout, act->node);
    else if (act->change) {
        /* Transformation : la tuile garde sa place, change de pièce.
         * L'état de l'ancienne pièce survit (découplé des vues). */
        layout_retile(act->node, act->piece);
        new_root = app->layout;
    } else
        new_root = layout_split(app->layout, act->node,
                                act->horizontal, act->piece);

    app->layout = new_root;
    layout_save(new_root);
    if (app->render_idle == 0)
        app->render_idle = g_idle_add(render_layout_idle, app);
}

static void
add_menu_button(GtkWidget *box, const char *label, TileAction *act)
{
    GtkWidget *b = gtk_button_new_with_label(label);

    gtk_widget_set_halign(b, GTK_ALIGN_FILL);
    g_signal_connect(b, "clicked", G_CALLBACK(on_tile_action), act);
    gtk_box_append(GTK_BOX(box), b);
}

/* Menu d'une tuile : diviser (même pièce), changer de pièce, retirer.
 * Actions explicites pour éviter tout split/retrait accidentel. */
static GtkWidget *
build_tile_menu(Layout *node, App *app)
{
    GtkWidget  *pop;
    GtkWidget  *menu;
    GPtrArray  *acts;
    const char *pieces[] = { "editor", "explorer", "bash", "empty" };
    gsize       i;

    pop = gtk_popover_new();
    /* Libère les TileAction quand le popover est détruit. */
    acts = g_ptr_array_new_with_free_func(g_free);
    g_object_set_data_full(G_OBJECT(pop), "tile-actions", acts,
                           (GDestroyNotify)g_ptr_array_unref);

    menu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(menu, 8);
    gtk_widget_set_margin_end(menu, 8);
    gtk_widget_set_margin_top(menu, 6);
    gtk_widget_set_margin_bottom(menu, 6);

    /* Diviser : la nouvelle tuile prend la même pièce (un éditeur divisé
     * donne un éditeur — deux vues sur le même état). */
    for (int h = 0; h < 2; h++) {
        TileAction *a = g_new0(TileAction, 1);

        a->app = app;
        a->node = node;
        a->horizontal = (h == 0);
        a->piece = node->id;
        a->popover = pop;
        g_ptr_array_add(acts, a);
        add_menu_button(menu, h == 0 ? "Diviser horizontalement"
                                     : "Diviser verticalement",
                        a);
    }

    /* Changer de pièce : transformation en place (l'état de l'ancienne
     * pièce survit). La pièce actuelle est désactivée. */
    gtk_box_append(GTK_BOX(menu),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    for (i = 0; i < G_N_ELEMENTS(pieces); i++) {
        TileAction *a = g_new0(TileAction, 1);
        GtkWidget  *b;

        a->app = app;
        a->node = node;
        a->change = TRUE;
        a->piece = pieces[i];
        a->popover = pop;
        g_ptr_array_add(acts, a);

        b = gtk_button_new_with_label(layout_name(pieces[i]));
        gtk_widget_set_halign(b, GTK_ALIGN_FILL);
        if (node->id != NULL && strcmp(pieces[i], node->id) == 0)
            gtk_widget_set_sensitive(b, FALSE); /* pièce actuelle */
        g_signal_connect(b, "clicked", G_CALLBACK(on_tile_action), a);
        gtk_box_append(GTK_BOX(menu), b);
    }

    /* Séparateur + retirer. */
    gtk_box_append(GTK_BOX(menu), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    if (node->parent != NULL) {
        TileAction *r = g_new0(TileAction, 1);

        r->app = app;
        r->node = node;
        r->remove = TRUE;
        r->popover = pop;
        g_ptr_array_add(acts, r);
        add_menu_button(menu, "Retirer cette tuile", r);
    }

    /* Actions de GROUPE sur le bloc parent (les blocs n'ont pas de barre
     * propre — une barre par niveau de split mangeait la page). */
    if (node->parent != NULL) {
        Layout *grp = node->parent;

        gtk_box_append(GTK_BOX(menu),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        {
            GtkWidget *lbl = gtk_label_new("Groupe");

            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_widget_add_css_class(lbl, "dim-label");
            gtk_box_append(GTK_BOX(menu), lbl);
        }
        for (int h = 0; h < 2; h++) {
            TileAction *a = g_new0(TileAction, 1);

            a->app = app;
            a->node = grp;
            a->horizontal = (h == 0);
            a->piece = "empty"; /* le bloc n'a pas de pièce */
            a->popover = pop;
            g_ptr_array_add(acts, a);
            add_menu_button(menu, h == 0 ? "Diviser le groupe horizontalement"
                                         : "Diviser le groupe verticalement",
                            a);
        }
        {
            /* Réduire tout le groupe en LA pièce de cette tuile (l'état
             * de la pièce survit dans App). */
            TileAction *a = g_new0(TileAction, 1);

            a->app = app;
            a->node = grp;
            a->change = TRUE;
            a->piece = node->id;
            a->popover = pop;
            g_ptr_array_add(acts, a);
            add_menu_button(menu, "Réduire le groupe à cette pièce", a);
        }
        if (grp->parent != NULL) {
            TileAction *r = g_new0(TileAction, 1);

            r->app = app;
            r->node = grp;
            r->remove = TRUE;
            r->popover = pop;
            g_ptr_array_add(acts, r);
            add_menu_button(menu, "Retirer le groupe", r);
        }
    }

    gtk_popover_set_child(GTK_POPOVER(pop), menu);
    return pop;
}

static GtkWidget *
build_tile_wrapper(Layout *node, App *app, GtkWidget *content)
{
    GtkWidget *box;
    GtkWidget *header;
    GtkWidget *menu_btn;
    const char *title;

    /* Vue fraîche par tuile : retirer une tuile ne détruit que la vue,
     * l'état du morceau (buffer / modèle) survit dans App. Les blocs
     * (sous-arbres) n'ont PAS de wrapper : leurs actions sont dans le
     * menu des tuiles (section « Groupe »). */
    title = layout_name(node->id);

    if (g_getenv("SIEB_DEBUG") != NULL)
        g_printerr("SIEB: tile id=%s widget=%p\n",
                   node->id != NULL ? node->id : "(null)", (void *)content);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (g_getenv("SIEB_DEBUG") != NULL)
        g_signal_connect(box, "destroy", G_CALLBACK(trace_destroy), app);
    /* Barre de titre compacte : un GtkHeaderBar fait ~51 px de haut — avec
     * les tuiles/blocs empilés (et un niveau de barre par split), les
     * barres mangeaient ~1/3 de la page. Une ligne label + menu suffit. */
    header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(header, 8);
    gtk_widget_set_margin_end(header, 4);
    gtk_widget_set_margin_top(header, 2);
    gtk_widget_set_margin_bottom(header, 2);
    {
        GtkWidget *label = gtk_label_new(title);

        /* Titre discret : 10 pt (une barre fine par tuile). */
        gtk_widget_add_css_class(label, "tile-title");
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_hexpand(label, TRUE); /* pousse « :: » à droite */
        gtk_box_append(GTK_BOX(header), label);
    }

    menu_btn = gtk_menu_button_new();
    gtk_widget_add_css_class(menu_btn, "flat");
    gtk_widget_add_css_class(menu_btn, "tile-menu");
    /* « :: » seul (set_child remplace label+flèche internes). */
    {
        GtkWidget *grip = gtk_label_new("::");

        gtk_widget_add_css_class(grip, "tile-title");
        gtk_menu_button_set_child(GTK_MENU_BUTTON(menu_btn), grip);
    }
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_btn),
                                build_tile_menu(node, app));
    gtk_widget_set_halign(menu_btn, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(header), menu_btn);

    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), content);
    gtk_widget_set_vexpand(content, TRUE);
    gtk_widget_set_hexpand(content, TRUE);
    return box;
}

/* Rendu récursif du modèle -> arbre GtkPaned. Chaque tuile crée une vue
 * fraîche sur l'état partagé de son morceau ; les paned sont reconstruits. */
static GtkWidget *
render_layout_node(Layout *node, App *app)
{
    GtkWidget *content;

    if (node->kind == LAYOUT_TILE)
        return build_tile_wrapper(node, app, create_piece(node->id, app));

    content = gtk_paned_new(node->kind == LAYOUT_HSPLIT
                                ? GTK_ORIENTATION_HORIZONTAL
                                : GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_start_child(GTK_PANED(content),
                              render_layout_node(node->a, app));
    gtk_paned_set_end_child(GTK_PANED(content),
                            render_layout_node(node->b, app));
    gtk_paned_set_resize_start_child(GTK_PANED(content), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(content), TRUE);
    /* Le drag de la poignée met à jour la fraction du modèle (persistée
     * par layout_save) — sans quoi le re-rendu reviendrait aux valeurs
     * par défaut. */
    g_object_set_data(G_OBJECT(content), "sieb-app", app);
    g_signal_connect(content, "notify::position",
                     G_CALLBACK(on_paned_position), node);
    /* PAS de wrapper sur les blocs : une barre par niveau de split
     * s'empilait (4 barres au-dessus d'une tuile profonde = 1/3 de page).
     * Les actions de groupe sont dans le menu des tuiles. */
    return content;
}

/* Save différé du layout : le drag de poignée émet notify::position en
 * continu ; on persiste une seule fois à la fin du mouvement. */
static guint layout_save_idle = 0;

static gboolean
layout_save_idle_cb(gpointer data)
{
    App *app = data;

    layout_save_idle = 0;
    layout_save(app->layout);
    return G_SOURCE_REMOVE;
}

static void
on_paned_position(GObject *paned, GParamSpec G_GNUC_UNUSED *pspec,
                  gpointer data)
{
    Layout    *node = data;
    GtkPaned  *p = GTK_PANED(paned);
    App       *app = NULL;
    int        total;
    double     fraction;

    total = (node->kind == LAYOUT_HSPLIT)
                ? gtk_widget_get_width(GTK_WIDGET(p))
                : gtk_widget_get_height(GTK_WIDGET(p));
    if (total <= 0)
        return;
    fraction = (double)gtk_paned_get_position(p) / (double)total;
    node->fraction = CLAMP(fraction, 0.05, 0.95);

    /* App pour le save différé : retrouvé via le handler d'action ? Non —
     * on passe par l'état global du contexte GTK : le data du notify est
     * le nœud ; on stocke app dans le widget pour le retrouver. */
    app = g_object_get_data(G_OBJECT(p), "sieb-app");
    if (app != NULL && layout_save_idle == 0)
        layout_save_idle = g_idle_add(layout_save_idle_cb, app);
}

/* Applique les fractions persistées aux poignées (après allocation). */

/* Le paned d'un nœud split se trouve dans son wrapper (le dernier enfant
 * de la box). Recherche récursive robuste. */
static GtkWidget *
find_paned_widget(GtkWidget *w)
{
    GtkWidget *child;
    GtkWidget *found;

    if (w == NULL)
        return NULL;
    if (GTK_IS_PANED(w))
        return w;
    for (child = gtk_widget_get_first_child(w); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        found = find_paned_widget(child);
        if (found != NULL)
            return found;
    }
    return NULL;
}

static void
set_paned_positions_walk(GtkWidget *widget, Layout *node)
{
    GtkWidget *paned_w;
    GtkPaned  *paned;
    int        total;

    if (node == NULL || node->kind == LAYOUT_TILE)
        return;
    paned_w = find_paned_widget(widget);
    if (paned_w == NULL)
        return;
    paned = GTK_PANED(paned_w);
    total = (node->kind == LAYOUT_HSPLIT)
                ? gtk_widget_get_width(paned_w)
                : gtk_widget_get_height(paned_w);
    if (total > 0)
        gtk_paned_set_position(paned, (int)(node->fraction * total));
    set_paned_positions_walk(gtk_paned_get_start_child(paned), node->a);
    set_paned_positions_walk(gtk_paned_get_end_child(paned), node->b);
}

static void
set_paned_positions(App *app)
{
    if (app->layout_root != NULL && app->layout != NULL)
        set_paned_positions_walk(app->layout_root, app->layout);
}

/* À chaque affichage (1er ou après re-rendu), on réapplique les fractions. */
static void
on_layout_map(GtkWidget G_GNUC_UNUSED *widget, gpointer data)
{
    set_paned_positions((App *)data);
}

/* Reconstruit l'arbre paned depuis le modèle et l'insère dans le holder.
 * Les vues des tuiles meurent avec l'ancien arbre ; l'état des morceaux
 * (buffer / modèle) survit dans App. Les pointeurs de VUE sont invalidés
 * ici et réassignés par les build_* des nouvelles tuiles (ou restent NULL
 * si le nouveau layout n'affiche pas la pièce). */
static void
render_layout(App *app)
{
    trace_state(app, "render_layout: avant unparent");
    /* gtk_widget_unparent retire proprement l'ancien arbre (les vues sont
     * détruites : widgets GInitiallyUnowned sans ref externe). */
    if (app->layout_root != NULL) {
        gtk_widget_unparent(app->layout_root);
        app->layout_root = NULL;
    }
    app->source_view = NULL;
    app->diffbar = NULL;
    app->explorer_scrolled = NULL;
    trace_state(app, "render_layout: apres unparent");

    app->layout_root = render_layout_node(app->layout, app);
    if (g_getenv("SIEB_DEBUG") != NULL)
        g_signal_connect(app->layout_root, "destroy",
                         G_CALLBACK(trace_destroy), app);
    gtk_widget_set_vexpand(app->layout_root, TRUE);
    gtk_box_append(GTK_BOX(app->layout_holder), app->layout_root);
    g_signal_connect_after(app->layout_root, "map",
                           G_CALLBACK(on_layout_map), app);
    gtk_widget_set_visible(app->layout_root, TRUE);
    trace_state(app, "render_layout: fin");
}

static void
on_activate(GtkApplication *gtk_app, gpointer data)
{
    App      *app = data;
    GtkWidget *header;
    GtkWidget *statusbar;
    GtkWidget *sep;
    AdwStyleManager *style_mgr;

    app->win = GTK_WINDOW(gtk_application_window_new(gtk_app));
    gtk_window_set_title(app->win, "SIEB - CodeDashBoard");
    gtk_window_set_default_size(app->win, 1280, 800);

    /* CSS applicatif : titres de tuiles discrets (10 pt). */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        const char *data_css =
            ".tile-title { font-size: 10pt; }\n"
            "menubutton.tile-menu > button { font-size: 9pt; "
            "padding: 0 4px; min-height: 0; }\n";

        gtk_css_provider_load_from_string(css, data_css);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }

    /* HeaderBar (sans bouton Ouvrir : l'explorateur suffit). */
    header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(app->win, header);

    /* Actions du menu "+" du panneau Explorateur. */
    {
        const GActionEntry win_actions[] = {
            { "add-structure", on_add_structure, NULL, NULL, NULL, { 0 } },
            { "add-project",   on_add_project,   NULL, NULL, NULL, { 0 } },
            { "save",          on_save_activated, NULL, NULL, NULL, { 0 } },
        };
        g_action_map_add_action_entries(G_ACTION_MAP(app->win), win_actions,
                                        G_N_ELEMENTS(win_actions), app);
        /* Ctrl+S (ou Cmd+S sur mac) déclenche win.save. */
        gtk_application_set_accels_for_action(gtk_app, "win.save",
                                              (const char *[]){"<Primary>s", NULL});
    }

    /* Système de tuiles : modèle (source de vérité) + rendu paned. */
    app->roots = roots_load();
    app->layout = layout_load();
    app->layout_holder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    render_layout(app);

    /* Moitié/moitié (fractions persistées) au premier affichage. */
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

    /* Témoin non sauvegardé : point affiché à droite du nom de fichier. */
    app->status_mod = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(GTK_LABEL(app->status_mod), 0.0);
    gtk_widget_set_margin_start(GTK_WIDGET(app->status_mod), 8);

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
    gtk_box_append(GTK_BOX(statusbar), GTK_WIDGET(app->status_mod));
    gtk_box_append(GTK_BOX(statusbar), sep);
    gtk_box_append(GTK_BOX(statusbar), GTK_WIDGET(app->status_pos));

    /* Assemblage final */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), app->layout_holder);
    gtk_box_append(GTK_BOX(root), statusbar);

    gtk_window_set_child(app->win, root);

    update_status(app);
    update_modified_indicator(app);
    recompute_dirty(app);

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

    /* MODE TEST (debug) : SIEB_TEST_SPLIT=1 reproduit le crash du split —
     * split horizontal du root dans un idle, puis quitte après 2 s. */
    if (g_getenv("SIEB_TEST_SPLIT") != NULL) {
        g_idle_add(test_split_idle, app);
        g_timeout_add(2000, test_quit_idle, gtk_app);
    }
    /* MODE TEST (debug) : SIEB_TEST_GRID=1 construit l'arrangement
     * A:B:C / A:B:C / D:B:C / E:F:F par les opérations du modèle
     * (comme le menu des tuiles/blocs), avec re-rendu à chaque étape. */
    if (g_getenv("SIEB_TEST_GRID") != NULL) {
        g_idle_add(test_grid_idle, app);
        g_timeout_add(2500, test_quit_idle, gtk_app);
    }
}

/* MODE TEST (debug) : voir SIEB_TEST_SPLIT dans on_activate. */
static Layout *test_first_tile(Layout *n)
{
    return n->kind == LAYOUT_TILE ? n : test_first_tile(n->a);
}

static gboolean
test_split_idle(gpointer data)
{
    App *app = data;
    Layout *t;

    /* SÉQUENCE STRESS : split+retile+remove enchaînés, avec re-rendu à
     * chaque étape (comme les clics du user). */
    t = test_first_tile(app->layout);
    app->layout = layout_split(app->layout, t, TRUE, t->id);
    render_layout(app);

    t = test_first_tile(app->layout);
    if (strcmp(t->id, "editor") == 0)
        layout_retile(t, "explorer");
    else
        layout_retile(t, "editor");
    /* PAS de layout_save : ne pas écraser le layout.json du user. */
    render_layout(app);

    t = test_first_tile(app->layout);
    app->layout = layout_split(app->layout, t, FALSE, t->id);
    render_layout(app);

    t = test_first_tile(app->layout);
    if (t->parent != NULL) {
        app->layout = layout_remove(app->layout, t);
        render_layout(app);
    }

    t = test_first_tile(app->layout);
    if (t->parent != NULL) {
        app->layout = layout_remove(app->layout, t);
        /* PAS de layout_save : ne pas écraser le layout.json du user. */
        render_layout(app);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
test_quit_idle(gpointer data)
{
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_REMOVE;
}

/* MODE TEST (debug) : voir SIEB_TEST_GRID dans on_activate. Construit le
 * pavage V( H(A,H(D,E)), H(V(B,C),F) ) — « A:B:C/A:B:C/D:B:C/E:F:F » — en
 * n'utilisant que layout_split/layout_retile sur tuiles ET blocs. */
static gboolean
test_grid_idle(gpointer data)
{
    App    *app = data;
    Layout *t;

    /* 1. V : 2 colonnes (editor|editor). */
    app->layout = layout_split(app->layout,
                               test_first_tile(app->layout), FALSE, "editor");
    render_layout(app);

    /* 2. V(b) : 3 colonnes : A | B | C. */
    t = app->layout->b;
    app->layout = layout_split(app->layout, t, FALSE, "editor");
    render_layout(app);

    /* 3. H(A) : A sur 2 lignes. */
    t = app->layout->a;
    app->layout = layout_split(app->layout, t, TRUE, "editor");
    render_layout(app);

    /* 4. H(X) : X = D | E. */
    t = app->layout->a->b;
    app->layout = layout_split(app->layout, t, TRUE, "editor");
    render_layout(app);

    /* 5. H(bloc B|C) : Y=B|C | F (empty). */
    t = app->layout->b;
    app->layout = layout_split(app->layout, t, TRUE, "empty");
    render_layout(app);

    /* 6. V(Y) : Y = B | C. */
    t = app->layout->b->a;
    app->layout = layout_split(app->layout, t, FALSE, "editor");
    render_layout(app);

    /* 7. Pièces finales du dessin. */
    layout_retile(app->layout->a->a, "editor");       /* A */
    layout_retile(app->layout->a->b->a, "editor");    /* D */
    layout_retile(app->layout->a->b->b, "explorer");  /* E */
    layout_retile(app->layout->b->a->a, "editor");    /* B */
    layout_retile(app->layout->b->a->b, "editor");    /* C */
    layout_retile(app->layout->b->b, "editor");       /* F */
    render_layout(app);
    return G_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    GtkApplication *gtk_app;
    App            *app;
    int             status;

    app = g_new0(App, 1);
    app->sel_anchor = GTK_INVALID_LIST_POSITION;
    app->multi_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    app->dirty = dirty_store_new();
    app->files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       per_file_free);

    gtk_app = GTK_APPLICATION(adw_application_new("org.sieb.code-dashboard",
                                                  G_APPLICATION_DEFAULT_FLAGS));
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), app);

    status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    /* Persiste les fichiers sales avant de quitter (le buffer courant inclus). */
    if (app->dirty != NULL)
        dirty_persist_now(app->dirty);
    g_object_unref(gtk_app);
    if (app->diff_timer != 0)
        g_source_remove(app->diff_timer);
    if (layout_save_idle != 0) {
        g_source_remove(layout_save_idle);
        layout_save_idle = 0;
    }
    if (app->tree_model != NULL)
        g_object_unref(app->tree_model);
    if (app->selection != NULL)
        g_object_unref(app->selection);
    g_hash_table_destroy(app->multi_paths);
    g_hash_table_destroy(app->files); /* libère chaque PerFile */
    layout_free(app->layout);
    dirty_store_free(app->dirty);
    g_free(app);

    return status;
}
