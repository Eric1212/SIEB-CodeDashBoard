/*
 * CodeDashBoard (CDB) — SIEB
 *
 * Fenêtre GTK4 + GtkSourceView 5 : HeaderBar avec ouverture de fichier,
 * panneau latéral "Dossiers" (roots de structure / roots de projet,
 * persistance JSON), éditeur avec coloration syntaxique, barre de statut.
 *
 * Compilation : make
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gtksourceview/gtksource.h>
#include <adwaita.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <spawn.h>
#include <limits.h>
#include <unistd.h>

extern char **environ;
#include "roots.h"
#include "fslist.h"
#include "dirty.h"
#include "diffbar.h"
#include "bashpanel.h"
#include "modal.h"
#include "session.h"
#include "llm.h"
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
    GtkLabel          *header_file; /* chemin courant dans la titlebar */
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
    int                modal_count; /* fenêtres-modales ouvertes (max 4) */
    /* Ignore les signaux pendant un chargement (évite un sale transitoire). */
    gboolean           suppress_dirty;
    /* Source de vérité de la multi-sélection : set de chemins (clés
     * g_strdup/g_free). GTK ne fait qu'afficher. */
    GHashTable        *multi_paths;
    /* Évite la récursion selection-changed pendant nos propres mutations. */
    gboolean           selection_guard;
    /* Config LLM de la session (llm.json), possédée par App. */
    LlmConfig         *llm_cfg;
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
static gboolean test_layout_idle(gpointer data);
static void     test_grid_sequence(App *app);
static void     test_split_sequence(App *app);
static gboolean test_close_idle(gpointer data);
static gboolean test_modal_idle(gpointer data);
static gboolean test_settings_step(gpointer data);
static gboolean test_quit_idle(gpointer data);

/* Délai entre pas des scénarios de test (ms). Volontairement court :
 * l'app est légère, les tests se comptent en ms — pas en secondes. */
static guint
cdb_test_delay(void)
{
    static guint ms = 0;

    if (ms == 0) {
        const char *e = g_getenv("CDB_TEST_DELAY");

        ms = e != NULL ? (guint)g_ascii_strtoll(e, NULL, 10) : 150;
        if (ms < 50 || ms > 500)
            ms = 150;
    }
    return ms;
}
static void recompute_dirty(App *app);
static void on_buffer_changed(GtkTextBuffer *buffer, gpointer data);
static void update_style_scheme(App *app);
static void render_layout(App *app);
static void set_paned_positions(App *app);
static GtkWidget *build_editor(App *app);
static GtkWidget *build_roots_panel(App *app);

/* ------------------------------------------------------------------ */
/* Chargement de fichier                                               */
/* ------------------------------------------------------------------ */

static void
load_file(App *app, const char *path)
{
    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: load_file path=%s\n", path);

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
                g_printerr("CDB: %s\n", error->message);
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
        char *title = g_strdup_printf("%s%s", app->current_file,
                                      dirty ? "*" : "");

        gtk_window_set_title(app->win, title);
        if (app->header_file != NULL)
            gtk_label_set_text(app->header_file, title);
        g_free(title);
    } else {
        gtk_window_set_title(app->win, "CodeDashBoard");
        if (app->header_file != NULL)
            gtk_label_set_text(app->header_file, "");
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
    cdb_diff_bar_set_ranges(CDB_DIFF_BAR(app->diffbar), ranges, total);
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

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: save -> %s\n", app->current_file);

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

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: row setup (gesture bouton 3 attaché)\n");
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
    g_object_set_data(G_OBJECT(expander), "cdb-pos",
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
        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: suppression de « %s »\n", app->pending_remove->path);
        roots_remove(app->roots, app->pending_remove);
        roots_save(app->roots);
        app->pending_remove = NULL;
        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: MUTATION unselect_all from %s\n", G_STRFUNC);
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

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: clic droit capté (row=%p)\n", (void *)row);

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

        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: popover popup demandé pour « %s »\n", dir_path);
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
            if (g_getenv("CDB_DEBUG") != NULL)
                g_printerr("CDB: création « %s » dans « %s »\n",
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
            g_printerr("CDB: %s\n", error->message);
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
    if (g_getenv("CDB_DEBUG") != NULL)
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

    if (g_getenv("CDB_DEBUG") != NULL) {
        GtkBitset *bits = gtk_selection_model_get_selection(model);
        g_printerr("CDB: sélection -> %lu éléments (changed pos=%u n=%u)\n",
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
        char    *path;

        /* item peut être NULL (row en cours de remplissage) : sans ce
         * garde, g_type_is_a sur NULL fait planter le ref interne. */
        if (item == NULL) {
            g_object_unref(row);
            return NULL;
        }
        path = g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)
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

/* Position d'une ligne : GtkTreeListRow de l'expander, sinon cdb-pos
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

    tagged = g_object_get_data(G_OBJECT(w), "cdb-pos");
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

        if (g_getenv("CDB_DEBUG") != NULL) {
            const char *parent_type = w != NULL ? G_OBJECT_TYPE_NAME(w) : "NULL";
            gpointer tagged = w != NULL
                ? g_object_get_data(G_OBJECT(w), "cdb-pos") : NULL;
            guint tagged_pos = tagged != NULL ? GPOINTER_TO_UINT(tagged) - 1 : GTK_INVALID_LIST_POSITION;
            g_printerr("CDB: pick_walk widget=%s cdb-pos=%u pos=%u\n",
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

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: pick=%s — pas de pos\n",
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
        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: Ctrl+clic hors item\n");
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
        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: Shift+clic plage [%u, %u] (unselect_rest=%d)\n",
                       lo, lo + n - 1, unselect_rest);
        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: MUTATION select_range lo=%u n=%u unselect_rest=%d from %s\n",
                       lo, n, unselect_rest, G_STRFUNC);
        gtk_selection_model_select_range(GTK_SELECTION_MODEL(app->selection),
                                         lo, n, unselect_rest);
        selection_sync_from_model(app);
        return;
    }

    /* Ctrl : toggle de la ligne, le reste de la sélection est conservé. */
    if (g_getenv("CDB_DEBUG") != NULL) {
        GtkBitset *bits = gtk_selection_model_get_selection(
            GTK_SELECTION_MODEL(app->selection));
        gboolean was_selected = gtk_selection_model_is_selected(
            GTK_SELECTION_MODEL(app->selection), pos);
        g_printerr("CDB: Ctrl+clic toggle pos=%u avant: size=%lu is_selected=%d",
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
            g_printerr("CDB: Ctrl+clic bitset complet size=%lu:", (unsigned long)n);
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
        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: Ctrl+clic -> multi_paths size=%lu\n",
                       (unsigned long)g_hash_table_size(app->multi_paths));
        selection_apply_from_paths(app);
    }

    if (g_getenv("CDB_DEBUG") != NULL) {
        GtkBitset *bits = gtk_selection_model_get_selection(
            GTK_SELECTION_MODEL(app->selection));
        g_printerr("CDB: Ctrl+clic toggle pos=%u après: size=%lu\n",
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
        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: release CLAIMÉ (mods=0x%x) — pas d'écrasement\n",
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

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: activate pos=%u (mods=%u)\n", position,
                   app->last_click_mods);

    if (row != NULL && g_getenv("CDB_DEBUG") != NULL) {
        gpointer item = gtk_tree_list_row_get_item(row);
        if (g_type_is_a(G_TYPE_FROM_INSTANCE(item), ROOT_TYPE_ENTRY)) {
            RootEntry *e = item;
            g_printerr("CDB: activate path=root:%s\n", e->path);
        } else {
            FileEntry *f = item;
            g_printerr("CDB: activate path=%s%s\n", f->path,
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
    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: activate -> ouverture fichier\n");

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

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: select_row (open) reset multi -> %s\n", path);

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
    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: reveal_path path=%s\n", file_path);
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
/* Trace d'état (CDB_DEBUG) : adresses + refcounts des objets d'état
 * partagés — permet de voir quand tree_model/selection deviennent
 * invalides (double-unref / use-after-free). */
static void
trace_destroy(GtkWidget *w, gpointer data)
{
    App *app = data;

    if (g_getenv("CDB_DEBUG") == NULL)
        return;
    g_printerr("CDB: destroy %s @%p (refs selection=%d)\n",
               G_OBJECT_TYPE_NAME(w), (void *)w,
               app->selection != NULL
                   ? (int)((GObject *)app->selection)->ref_count : -1);
}

static void
trace_state(App *app, const char *where)
{
    if (g_getenv("CDB_DEBUG") == NULL)
        return;
    g_printerr("CDB: [%s] tree_model=%p selection=%p (refs=%d) roots=%p "
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
    if (g_getenv("CDB_DEBUG") != NULL) {
        g_printerr("CDB: build_roots_view selection refs apres new=%d\n",
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

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: rebuild_explorer (dossiers ouverts=%u)\n",
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
        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: rebuild_explorer vue créée (n_items=%u)\n",
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

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: schéma = %s (dark=%d)\n", name,
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

/* Debug (CDB_DEBUG=1) : répartition verticale des allocations. */
static gboolean
dump_allocations(gpointer data)
{
    App       *app = data;
    GtkWidget *root = gtk_window_get_child(app->win);

    fprintf(stderr,
            "CDB: win %dx%d | root %dx%d | layout %dx%d | statusbar %dx%d\n",
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
        if (g_getenv("CDB_DEBUG") != NULL)
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
    if (g_getenv("CDB_DEBUG") != NULL)
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
    app->diffbar = cdb_diff_bar_new();
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

/* -------------------------------------------------------------- */
/* Settings : accordéon (GtkRevealer) — General / GitHub-Git / LLM */
/*                                                                */
/* Sections fermées par défaut ; le contenu de chaque section est  */
/* un placeholder en attendant l'implémentation réelle.            */
/* -------------------------------------------------------------- */

typedef struct SettingsSection {
    const char *title;
    const char *placeholder;             /* section simple */
    const struct SettingsSection *subs;  /* ou sous-sections (accordéon) */
    gsize        n_subs;
} SettingsSection;

static void on_settings_section_toggled(GtkToggleButton *btn, gpointer data);
static GtkWidget *build_settings_section(const SettingsSection *sec);
static void on_provider_save_clicked(GtkButton *btn, gpointer data);
static void on_allowed_models_changed(GtkEditable *editable,
                                      gpointer data);
static GtkWidget *build_provider_form(const char *provider_name);
static GtkWidget *build_harness_form(void);
static GtkWidget *build_initprompt_editor(void);

static GtkWidget *
build_settings_section(const SettingsSection *sec)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header_btn = gtk_toggle_button_new_with_label(sec->title);
    GtkWidget *revealer = gtk_revealer_new();
    GtkWidget *body;

    /* Bouton-titre : plat, aligné à gauche (look ligne d'accordéon). */
    gtk_widget_add_css_class(header_btn, "flat");
    gtk_button_set_child(GTK_BUTTON(header_btn), NULL);
    {
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *arrow = gtk_label_new("›");
        GtkWidget *title = gtk_label_new(sec->title);

        gtk_widget_add_css_class(title, "titlebar-brand");
        gtk_widget_set_halign(title, GTK_ALIGN_START);
        gtk_widget_set_hexpand(title, TRUE);
        gtk_widget_set_halign(arrow, GTK_ALIGN_START);
        g_object_set_data(G_OBJECT(header_btn), "arrow", arrow);
        gtk_box_append(GTK_BOX(hbox), arrow);
        gtk_box_append(GTK_BOX(hbox), title);
        gtk_button_set_child(GTK_BUTTON(header_btn), hbox);
    }

    /* Le revealer suit l'état du bouton ; la flèche pivote quand ouvert. */
    g_object_bind_property(header_btn, "active",
                           revealer, "reveal-child",
                           G_BINDING_DEFAULT);
    g_signal_connect(header_btn, "toggled",
                     G_CALLBACK(on_settings_section_toggled), NULL);

    /* Corps : formulaire provider, sous-accordéons, ou placeholder. */
    if (g_strcmp0(sec->title, "OpenRouter") == 0)
        body = build_provider_form("OpenRouter");
    else if (g_strcmp0(sec->title, "OpenCode") == 0)
        body = build_provider_form("OpenCode");
    else if (g_strcmp0(sec->title, "HyperCharm") == 0)
        body = build_provider_form("HyperCharm");
    else if (g_strcmp0(sec->title, "429") == 0)
        body = build_harness_form();
    else if (g_strcmp0(sec->title, "Init-Prompt") == 0)
        body = build_initprompt_editor();
    else if (sec->subs != NULL && sec->n_subs > 0) {
        body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_margin_start(body, 16);
        for (gsize i = 0; i < sec->n_subs; i++)
            gtk_box_append(GTK_BOX(body),
                           build_settings_section(&sec->subs[i]));
    } else {
        body = gtk_label_new(sec->placeholder);
        gtk_label_set_xalign(GTK_LABEL(body), 0.0);
        gtk_widget_set_margin_start(body, 24);
        gtk_widget_set_margin_top(body, 6);
        gtk_widget_set_margin_bottom(body, 6);
        gtk_widget_add_css_class(body, "dim-label");
    }
    gtk_revealer_set_child(GTK_REVEALER(revealer), body);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE); /* fermé */

    gtk_box_append(GTK_BOX(box), header_btn);
    gtk_box_append(GTK_BOX(box), revealer);
    return box;
}

static void
on_settings_section_toggled(GtkToggleButton *btn, gpointer G_GNUC_UNUSED data)
{
    GtkWidget *arrow = g_object_get_data(G_OBJECT(btn), "arrow");

    if (arrow != NULL)
        gtk_label_set_text(GTK_LABEL(arrow),
                           gtk_toggle_button_get_active(btn) ? "⌄" : "›");
}

/* Sauvegarde EXPLICITE d'un provider (bouton « Enregistrer ») :
 * la clé, rien d'autre. « active » (provider/modèle du chat) n'est
 * JAMAIS touché ici — il se choisit dans le menu de la tuile LLM.
 * Une clé vide s'enregistre aussi (ex: OpenCode Zen sans clé) : les
 * champs vides sont des informations volontaires. */
static void
on_provider_save_clicked(GtkButton *btn, gpointer G_GNUC_UNUSED data)
{
    GtkWidget  *w = GTK_WIDGET(btn);
    const char *provider = g_object_get_data(G_OBJECT(w), "provider");
    GtkWidget  *key_entry = g_object_get_data(G_OBJECT(w), "key-entry");
    GtkWidget  *status = g_object_get_data(G_OBJECT(w), "status");
    const char *key = gtk_editable_get_text(GTK_EDITABLE(key_entry));

    llm_config_save_provider(provider, key);
    gtk_label_set_text(GTK_LABEL(status), "Enregistré \u2713");
}

/* ------------------------------------------------ */
/* Suggestions de modèles depuis /models             */
/*                                                   */
/* GTK4 déprécie GtkEntryCompletion : popover maison */
/* ancré au champ, rempli par llm_models_fetch.      */
/* ------------------------------------------------ */

typedef struct {
    LlmModelInfo *entries; /* tableau NULL-terminé (copie possédée) */
    GtkWidget    *entry;
    GtkWidget    *popover;
    GtkWidget    *listbox;
    GtkWidget    *anchor;  /* grid du formulaire : parent LÉGITIME du popover */
    char         *provider;
} ModelSuggest;

/* Contexte de fetch : la ref sur l'entry garantit que s reste vivant
 * pendant le vol (la fenêtre peut fermer, l'entry attend le callback). */
typedef struct {
    ModelSuggest *s;
    GtkWidget    *entry; /* ref possédée pendant le vol */
} ModelFetchCtx;

/* Refresh différé : reconstruire la liste PENDANT row-activated détruit
 * la rangée cliquée sous les pieds du signal (liste « qui disparaît »).
 * L'idle passe après la fin de l'émission ; on rouvre le popover pour
 * enchaîner les multi-sélections. */
static void     model_suggest_refresh(ModelSuggest *s);
static void     model_suggest_popup(ModelSuggest *s);

static gboolean
model_suggest_refresh_idle(gpointer data)
{
    ModelSuggest *s = data;

    model_suggest_refresh(s);
    model_suggest_popup(s);
    return G_SOURCE_REMOVE;
}

static void
on_model_row_activated(GtkListBox G_GNUC_UNUSED *lb, GtkListBoxRow *row,
                       gpointer data)
{
    ModelSuggest *s = data;
    const char   *id = g_object_get_data(G_OBJECT(row), "model-id");
    const char   *cur;
    GString      *out;
    gchar       **toks;
    gboolean      had = FALSE;

    if (id == NULL)
        return;
    cur = gtk_editable_get_text(GTK_EDITABLE(s->entry));

    /* Bascule : réémet la liste virgule-sans l'id, puis l'ajoute s'il
     * n'y était pas. */
    out = g_string_new(NULL);
    toks = g_strsplit(cur, ",", -1);
    for (int i = 0; toks[i] != NULL; i++) {
        char *tok = g_strstrip(toks[i]);

        if (tok[0] == '\0')
            continue;
        if (strcmp(tok, id) == 0) {
            had = TRUE;
            continue;
        }
        g_string_append_printf(out, "%s%s", out->len > 0 ? ", " : "", tok);
    }
    if (!had)
        g_string_append_printf(out, "%s%s", out->len > 0 ? ", " : "", id);
    g_strfreev(toks);

    gtk_editable_set_text(GTK_EDITABLE(s->entry), out->str);
    g_string_free(out, TRUE);
    /* set_text → changed → sauvegarde ; le refresh des ✓ passe à l'idle
     * (voir ci-dessus) — pas de popdown : multi-sélection. */
    g_idle_add(model_suggest_refresh_idle, s);
}

static void
model_suggest_refresh(ModelSuggest *s)
{
    char *filter;

    if (s->listbox == NULL)
        return; /* fenêtre fermée : callbacks en vol ignorés */
    filter = llm_config_get_allowed_models(s->provider);

    /* Vide la liste puis une row par modèle autorisé. */
    for (GtkWidget *child = gtk_widget_get_first_child(s->listbox);
         child != NULL; ) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);

        gtk_list_box_remove(GTK_LIST_BOX(s->listbox), child);
        child = next;
    }
    if (s->entries != NULL) {
        for (int i = 0; s->entries[i].id != NULL; i++) {
            const char *id = s->entries[i].id;
            const char *display = s->entries[i].name != NULL
                                      ? s->entries[i].name
                                      : id;
            GtkWidget *lbl, *row;
            gboolean   allowed = llm_model_allowed(filter, id);
            char      *shown;

            /* TOUS les modèles restent visibles ici : c'est l'UI de
             * sélection multiple (✓ = autorisé, clic = bascule). Le
             * filtrage s'applique aux CONSOMMATEURS (tuile LLM). */
            shown = allowed ? g_strdup_printf("\u2713 %s", display)
                            : g_strdup(display);
            lbl = gtk_label_new(shown);
            row = gtk_list_box_row_new();
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_widget_set_margin_start(lbl, 8);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
            g_object_set_data_full(G_OBJECT(row), "model-id",
                                   g_strdup(id), g_free);
            gtk_list_box_append(GTK_LIST_BOX(s->listbox), row);
            g_free(shown);
        }
    }
    g_free(filter);
}

static void
on_models_fetched(LlmModelInfo *models, gpointer data)
{
    ModelFetchCtx *ctx = data;
    ModelSuggest  *s = ctx->s;

    /* La ref sur l'entry garantit que s est vivant ici. ids possédés
     * par llm.c, libérés au retour du callback : on copie. */
    if (s != NULL) {
        llm_models_free(s->entries);
        s->entries = models != NULL ? llm_models_copy(models) : NULL;
        model_suggest_refresh(s);
    }
    g_object_unref(ctx->entry); /* lâche l'ancre : teardown normal */
    g_free(ctx);
}

static void
model_suggest_popup(ModelSuggest *s)
{
    graphene_rect_t bounds;

    if (s->popover == NULL || s->entry == NULL || s->anchor == NULL)
        return; /* fenêtre fermée */
    if (s->entries == NULL || s->entries[0].id == NULL)
        return;
    /* Ancre sous le champ : rect dans les coordonnées de l'ancre
     * (parent du popover). */
    if (gtk_widget_compute_bounds(s->entry, s->anchor, &bounds)) {
        GdkRectangle below = { (int)bounds.origin.x,
                               (int)(bounds.origin.y +
                                     bounds.size.height),
                               (int)bounds.size.width, 1 };

        gtk_popover_set_pointing_to(GTK_POPOVER(s->popover), &below);
    }
    gtk_popover_popup(GTK_POPOVER(s->popover));
}

static void
on_model_entry_icon(GtkEntry G_GNUC_UNUSED *entry,
                    gint G_GNUC_UNUSED icon_pos,
                    GdkEvent G_GNUC_UNUSED *event, gpointer data)
{
    model_suggest_popup(data);
}

static void
on_model_entry_focus(GtkEventControllerFocus G_GNUC_UNUSED *ctrl,
                     gpointer data)
{
    ModelSuggest *s = data;

    /* Champ vide → liste complète ; sinon le popup reste sur demande. */
    if (s->entries != NULL &&
        gtk_editable_get_text(GTK_EDITABLE(s->entry))[0] == '\0')
        model_suggest_popup(data);
}

/* À la destruction de l'entry : invalide tout — des callbacks/idles en
 * vol peuvent encore arriver (fetch /models, refresh différé). */
static void
on_model_entry_destroy(GtkWidget G_GNUC_UNUSED *entry, gpointer data)
{
    ModelSuggest *s = data;

    s->popover = NULL;
    s->listbox = NULL;
    s->entry = NULL;
}

static void
model_suggest_free(gpointer data)
{
    ModelSuggest *s = data;

    llm_models_free(s->entries);
    g_free(s->provider);
    g_free(s);
}

/* Champ « Modèles autorisés » : le filtre se sauvegarde même vide
 * (vide = tout autoriser) et ne touche jamais au provider/modèle actif. */
static void
on_allowed_models_changed(GtkEditable *editable, gpointer G_GNUC_UNUSED data)
{
    const char   *provider =
        g_object_get_data(G_OBJECT(editable), "provider");
    ModelSuggest *s = g_object_get_data(G_OBJECT(editable),
                                        "model-suggest");

    if (provider == NULL)
        return;
    llm_config_set_allowed_models(
        provider, gtk_editable_get_text(GTK_EDITABLE(editable)));
    /* La liste de suggestions suit le filtre en direct (différé idle :
     * ne pas reconstruire pendant l'émission de changed). */
    if (s != NULL)
        g_idle_add(model_suggest_refresh_idle, s);
}

/* Attache les suggestions /models au champ modèle du provider. */
static void
model_suggest_attach(GtkWidget *model_entry, const char *provider_name,
                     GtkWidget *anchor)
{
    ModelSuggest          *s = g_new0(ModelSuggest, 1);
    GtkWidget             *scroll;
    GtkEventController    *focus;

    s->entry = model_entry;
    s->anchor = anchor;
    s->provider = g_strdup(provider_name);
    s->listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s->listbox),
                                    GTK_SELECTION_NONE);
    /* Clic SIMPLE = choix immédiat (row-activated sinon exige un
     * double-clic — l'utilisateur croyait que ça « ne marchait pas »). */
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(s->listbox),
                                              TRUE);
    g_signal_connect(s->listbox, "row-activated",
                     G_CALLBACK(on_model_row_activated), s);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll),
                                               24);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll),
                                               320);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), s->listbox);

    s->popover = gtk_popover_new();
    /* Parent = grid du formulaire (conteneur légitime). L'ancrage sous
     * le champ se fait au popup via pointing_to. */
    gtk_widget_set_parent(s->popover, s->anchor);
    gtk_popover_set_child(GTK_POPOVER(s->popover), scroll);

    /* Icône cliquable + focus champ vide → popup. */
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(model_entry),
                                      GTK_ENTRY_ICON_SECONDARY,
                                      "pan-down-symbolic");
    g_signal_connect(model_entry, "icon-press",
                     G_CALLBACK(on_model_entry_icon), s);
    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_model_entry_focus), s);
    gtk_widget_add_controller(model_entry, focus);
    /* Le popover parenté manuellement doit être déparenté à la mort de
     * l'entry (sinon finalisation cassée → segfault). */
    g_signal_connect(model_entry, "destroy",
                     G_CALLBACK(on_model_entry_destroy), s);

    g_object_set_data_full(G_OBJECT(model_entry), "model-suggest", s,
                           model_suggest_free);
    {
        ModelFetchCtx *ctx = g_new0(ModelFetchCtx, 1);

        ctx->s = s;
        ctx->entry = model_entry;
        g_object_ref(model_entry); /* ref pendant le vol : pas de course */
        llm_models_fetch(provider_name, on_models_fetched, ctx);
    }
}

/* Application de l'état du formulaire Harness : sensibilité + save. */
static void
harness_apply(GtkWidget *src)
{
    GtkWidget *grid = gtk_widget_get_ancestor(src, GTK_TYPE_GRID);
    GtkWidget *sw;
    GtkWidget *spin_max;
    GtkWidget *spin_delay;

    if (grid == NULL)
        return;
    sw = GTK_WIDGET(g_object_get_data(G_OBJECT(grid), "h-sw"));
    spin_max = GTK_WIDGET(g_object_get_data(G_OBJECT(grid), "h-max"));
    spin_delay = GTK_WIDGET(g_object_get_data(G_OBJECT(grid), "h-delay"));
    if (sw == NULL || spin_max == NULL || spin_delay == NULL)
        return;

    {
        gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));

        gtk_widget_set_sensitive(spin_max, on);
        gtk_widget_set_sensitive(spin_delay, on);
        llm_config_save_retry429(
            on,
            (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_max)),
            (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_delay)));
    }
}

static void
on_harness_switch_changed(GtkSwitch G_GNUC_UNUSED *sw,
                          GParamSpec G_GNUC_UNUSED *pspec,
                          gpointer G_GNUC_UNUSED data)
{
    harness_apply(GTK_WIDGET(sw));
}

static void
on_harness_spin_changed(GtkEditable *editable, gpointer G_GNUC_UNUSED data)
{
    harness_apply(GTK_WIDGET(editable));
}

/* Formulaire Harness : politique de retry sur HTTP 429.
 * Défauts : oui / 200 répétitions / 250 ms. */
static GtkWidget *
build_harness_form(void)
{
    GtkWidget     *grid = gtk_grid_new();
    GtkWidget     *retry_lbl = gtk_label_new("Retry sur HTTP 429 :");
    GtkWidget     *sw = gtk_switch_new();
    GtkWidget     *max_lbl = gtk_label_new("Répétitions (0 = infini) :");
    GtkWidget     *delay_lbl = gtk_label_new("Délai entre essais (ms) :");
    GtkAdjustment *adj_max = gtk_adjustment_new(200, 0, 5000, 10, 100, 0);
    GtkWidget     *spin_max = gtk_spin_button_new(adj_max, 10, 0);
    GtkAdjustment *adj_d = gtk_adjustment_new(250, 10, 100000, 10, 1000, 0);
    GtkWidget     *spin_delay = gtk_spin_button_new(adj_d, 10, 0);
    LlmRetry429    rc;

    llm_retry429_load(&rc);
    gtk_switch_set_active(GTK_SWITCH(sw), rc.retry);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_max),
                              (double)rc.max_retries);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_delay),
                              (double)rc.delay_ms);
    gtk_widget_set_sensitive(spin_max, rc.retry);
    gtk_widget_set_sensitive(spin_delay, rc.retry);

    g_object_set_data_full(G_OBJECT(grid), "h-sw", sw, NULL);
    g_object_set_data_full(G_OBJECT(grid), "h-max", spin_max, NULL);
    g_object_set_data_full(G_OBJECT(grid), "h-delay", spin_delay, NULL);

    g_signal_connect(sw, "notify::active",
                     G_CALLBACK(on_harness_switch_changed), NULL);
    g_signal_connect(spin_max, "changed",
                     G_CALLBACK(on_harness_spin_changed), NULL);
    g_signal_connect(spin_delay, "changed",
                     G_CALLBACK(on_harness_spin_changed), NULL);

    gtk_widget_set_halign(retry_lbl, GTK_ALIGN_START);
    gtk_widget_set_halign(max_lbl, GTK_ALIGN_START);
    gtk_widget_set_halign(delay_lbl, GTK_ALIGN_START);
    gtk_widget_set_halign(sw, GTK_ALIGN_START); /* pilule native, pas étirée */
    gtk_widget_set_hexpand(spin_max, TRUE);
    gtk_widget_set_hexpand(spin_delay, TRUE);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_attach(GTK_GRID(grid), retry_lbl, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sw, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), max_lbl, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin_max, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), delay_lbl, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin_delay, 1, 2, 1, 1);
    return grid;
}

/* ------------------------------------------------ */
/* Éditeur « Init-Prompt » (prompt système par session) */
/* ------------------------------------------------ */

typedef struct {
    GtkWidget *view;
    GtkWidget *status;
} InitPromptCtx;

static void
on_ip_insert(GtkButton *btn, gpointer data)
{
    const char    *snippet = data;
    InitPromptCtx *ctx = g_object_get_data(G_OBJECT(btn), "ip-ctx");
    GtkTextBuffer *buf;
    GtkTextIter    ins;

    if (ctx == NULL || ctx->view == NULL || snippet == NULL)
        return;
    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->view));
    gtk_text_buffer_get_iter_at_mark(buf, &ins,
                                     gtk_text_buffer_get_insert(buf));
    gtk_text_buffer_insert(buf, &ins, snippet, -1);
}

static void
on_ip_save_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    InitPromptCtx *ctx = data;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->view));
    GtkTextIter    s, e;
    char          *text;

    gtk_text_buffer_get_bounds(buf, &s, &e);
    text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    llm_persona_save(text);
    g_free(text);
    gtk_label_set_text(GTK_LABEL(ctx->status), "Enregistré ✓");
}

static GtkWidget *
build_initprompt_editor(void)
{
    GtkSourceBuffer *sbuf;
    typedef struct {
        const char *label;
        const char *snippet;
    } IpSnippet;

    static const IpSnippet snippets[] = {
        { "[PROJET]", "[PROJET]" },
        { "[CHEMIN]", "[CHEMIN]" },
        { "/CDB::",   "/CDB::bash-0::\"COMMANDE\"" },
    };
    InitPromptCtx *ctx = g_new0(InitPromptCtx, 1);
    GtkWidget     *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget     *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget     *scroll;
    GtkTextBuffer *buf;
    char          *raw;

    /* Barre d'insertion : les variables et le gabarit de commande. */
    for (gsize i = 0; i < G_N_ELEMENTS(snippets); i++) {
        GtkWidget *b = gtk_button_new_with_label(snippets[i].label);

        gtk_widget_add_css_class(b, "flat");
        /* Sans focus : le clic n'arrache pas le curseur de l'éditeur. */
        gtk_widget_set_focusable(b, FALSE);
        g_object_set_data(G_OBJECT(b), "ip-ctx", ctx); /* pour le handler */
        g_signal_connect(b, "clicked", G_CALLBACK(on_ip_insert),
                         (gpointer)snippets[i].snippet);
        gtk_box_append(GTK_BOX(bar), b);
    }

    /* Buffer source + schéma Adwaita : le fond suit le thème clair/
     * sombre comme l'éditeur principal. */
    {
        GtkSourceStyleSchemeManager *smgr_m =
            gtk_source_style_scheme_manager_get_default();
        AdwStyleManager *style_mgr = adw_style_manager_get_default();
        const char *sname = adw_style_manager_get_dark(style_mgr)
                                ? "Adwaita-dark"
                                : "Adwaita";
        GtkSourceStyleScheme *scheme =
            gtk_source_style_scheme_manager_get_scheme(smgr_m, sname);

        if (g_getenv("CDB_DEBUG") != NULL)
            g_printerr("CDB: Init-Prompt schéma=%s trouvé=%d\n", sname,
                       scheme != NULL);
        sbuf = gtk_source_buffer_new(NULL);
        if (scheme != NULL)
            gtk_source_buffer_set_style_scheme(GTK_SOURCE_BUFFER(sbuf),
                                               scheme);
    }

    ctx->view =
        gtk_source_view_new_with_buffer(GTK_SOURCE_BUFFER(sbuf));
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctx->view),
                                GTK_WRAP_WORD_CHAR);
    gtk_source_view_set_show_line_numbers(
        GTK_SOURCE_VIEW(ctx->view), TRUE);
    gtk_widget_add_css_class(ctx->view, "initprompt-editor");

    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->view));
    raw = llm_persona_raw();
    gtk_text_buffer_set_text(buf, raw != NULL ? raw : "", -1);
    g_free(raw);

    /* Curseur initial en fin de texte : les tags partent de là tant
     * qu'Éric n'a pas cliqué ailleurs dans l'éditeur. */
    {
        GtkTextIter cur;

        gtk_text_buffer_get_end_iter(buf, &cur);
        gtk_text_buffer_place_cursor(buf, &cur);
    }

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ctx->view);
    /* PAS de vexpand : un accordéon fermé doit libérer son espace.
     * Hauteur utile bornée par min/max-content-height. */
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(scroll), 260);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(scroll), 420);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroll), TRUE);

    {
        GtkWidget *save_btn = gtk_button_new_with_label("Enregistrer");

        gtk_widget_add_css_class(save_btn, "flat");
        g_signal_connect(save_btn, "clicked",
                         G_CALLBACK(on_ip_save_clicked), ctx);
        gtk_box_append(GTK_BOX(box), bar);
        gtk_box_append(GTK_BOX(box), scroll);
        {
            GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

            gtk_box_append(GTK_BOX(foot), save_btn);
            gtk_box_append(GTK_BOX(foot), ctx->status);
            gtk_widget_set_halign(ctx->status, GTK_ALIGN_END);
            gtk_widget_set_hexpand(ctx->status, TRUE);
            gtk_box_append(GTK_BOX(box), foot);
        }
    }

    g_object_set_data_full(G_OBJECT(box), "ip-ctx-owner", ctx, g_free);
    return box;
}

static GtkWidget *
build_provider_form(const char *provider_name)
{
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *key_lbl = gtk_label_new("Clé API :");
    GtkWidget *key_entry = gtk_entry_new();
    GtkWidget *model_lbl = gtk_label_new("Modèles autorisés :");
    GtkWidget *model_entry = gtk_entry_new();
    GtkWidget *save_btn = gtk_button_new_with_label("Enregistrer");
    GtkWidget *status = gtk_label_new("");

    gtk_widget_set_halign(key_lbl, GTK_ALIGN_START);
    gtk_widget_set_halign(model_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(key_entry, TRUE);
    gtk_widget_set_hexpand(model_entry, TRUE);
    /* La clé est un secret : affichage masqué. OpenCode Zen marche
     * SANS clé (placeholder différent). */
    gtk_entry_set_visibility(GTK_ENTRY(key_entry), FALSE);
    if (g_strcmp0(provider_name, "OpenCode") == 0) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(key_entry),
                                       "(optionnelle)");
        gtk_entry_set_placeholder_text(GTK_ENTRY(model_entry),
                                       "(vide : tous les modèles)");
    } else if (g_strcmp0(provider_name, "HyperCharm") == 0) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(key_entry),
                                       "clé HyperCharm…");
        gtk_entry_set_placeholder_text(GTK_ENTRY(model_entry),
                                       "(vide : tous les modèles)");
    } else {
        gtk_entry_set_placeholder_text(GTK_ENTRY(key_entry), "sk-or-v1-…");
        gtk_entry_set_placeholder_text(GTK_ENTRY(model_entry),
                                       "(vide : tous les modèles)");
    }

    /* Préremplit la clé depuis llm.json — pour TOUS les providers,
     * pas seulement l'actif (avant : la clé disparaissait dès que le
     * provider cessait d'être actif). */
    {
        char *saved_key = llm_config_get_api_key(provider_name);

        if (saved_key != NULL && saved_key[0] != '\0')
            gtk_editable_set_text(GTK_EDITABLE(key_entry), saved_key);
        g_free(saved_key);
    }
    /* Préremplit le filtre (indépendant du provider actif). */
    {
        char *filter = llm_config_get_allowed_models(provider_name);

        if (filter != NULL)
            gtk_editable_set_text(GTK_EDITABLE(model_entry), filter);
        g_free(filter);
    }

    /* Le champ modèle connaît SON provider (pour le filtre direct). */
    g_object_set_data_full(G_OBJECT(model_entry), "provider",
                           g_strdup(provider_name), g_free);
    /* Le champ modèles = FILTRE : sauvegarde même vide (tout autoriser),
     * sans toucher au provider/modèle actifs. */
    g_signal_connect(model_entry, "changed",
                     G_CALLBACK(on_allowed_models_changed), NULL);

    /* Bouton Enregistrer : écrit la clé d'un coup (l'auto-save à la
     * frappe perdait la clé). Ne touche JAMAIS au provider/modèle
     * actifs — ça, c'est le menu de la tuile LLM qui le fait. */
    gtk_widget_add_css_class(save_btn, "flat");
    gtk_widget_set_halign(save_btn, GTK_ALIGN_START);
    g_object_set_data_full(G_OBJECT(save_btn), "provider",
                           g_strdup(provider_name), g_free);
    g_object_set_data(G_OBJECT(save_btn), "key-entry", key_entry);
    g_object_set_data(G_OBJECT(save_btn), "status", status);
    g_signal_connect(save_btn, "clicked",
                     G_CALLBACK(on_provider_save_clicked), NULL);
    gtk_label_set_xalign(GTK_LABEL(status), 0.0);
    gtk_widget_add_css_class(status, "dim-label");

    /* Suggestions de modèles depuis /models du provider : champ vide →
     * liste complète (focus ou icône), le popup reste disponible ensuite. */
    if (llm_provider_default_url(provider_name) != NULL)
        model_suggest_attach(model_entry, provider_name, grid);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 24);
    gtk_widget_set_margin_top(grid, 6);
    gtk_widget_set_margin_bottom(grid, 6);
    gtk_grid_attach(GTK_GRID(grid), key_lbl, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), key_entry, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), model_lbl, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), model_entry, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), save_btn, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), status, 1, 3, 1, 1);
    return grid;
}

static GtkWidget *
build_settings(App *app G_GNUC_UNUSED)
{
    static const SettingsSection provider_subs[] = {
        { "OpenAi-Compatible", "(à venir : endpoint, modèle, clé API…)", NULL, 0 },
        { "HyperCharm",        NULL, NULL, 0 }, /* formulaire provider */
        { "OpenCode",          NULL, NULL, 0 }, /* formulaire provider */
        { "OpenRouter",        NULL, NULL, 0 }, /* formulaire provider */
    };
    static const SettingsSection harness_subs[] = {
        { "429",         NULL, NULL, 0 }, /* formulaire retry */
        { "Init-Prompt", NULL, NULL, 0 }, /* éditeur du prompt système */
    };
    static const SettingsSection llm_subs[] = {
        { "Harness",    NULL, harness_subs, G_N_ELEMENTS(harness_subs) },
        { "Tools",      "(à venir : outils exposés au modèle…)", NULL, 0 },
        { "Providers",  NULL, provider_subs, G_N_ELEMENTS(provider_subs) },
    };
    static const SettingsSection sections[] = {
        { "General",     "(à venir : thème, police, indentation…)", NULL, 0 },
        { "GitHub/Git",  "(à venir : token, user, repo par défaut…)", NULL, 0 },
        { "LLM",         NULL, llm_subs, G_N_ELEMENTS(llm_subs) },
    };
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    for (gsize i = 0; i < G_N_ELEMENTS(sections); i++)
        gtk_box_append(GTK_BOX(box), build_settings_section(&sections[i]));

    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

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
    else if (strcmp(id, "llm") == 0)
        w = llm_tile_new(app->llm_cfg, G_ACTION_GROUP(app->win),
                         app->roots, app->multi_paths);
    else if (strcmp(id, "settings") == 0) {
        w = build_settings(app);
    } else {
        /* Vide : emplacement réservé, prêt à recevoir un morceau (Phase 2). */
        w = gtk_label_new("Vide\n(emplacement réservé)");
        gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    }

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: create_piece id=%s -> %p\n",
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
    const char *pieces[] = { "editor", "explorer", "bash", "llm", "empty" };
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

    if (g_getenv("CDB_DEBUG") != NULL)
        g_printerr("CDB: tile id=%s widget=%p\n",
                   node->id != NULL ? node->id : "(null)", (void *)content);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* Taille minimale d'une tuile : 100×100 px (le paned parent ne peut
     * pas réduire davantage — shrink désactivé dans render_layout_node). */
    gtk_widget_set_size_request(box, 100, 100);
    if (g_getenv("CDB_DEBUG") != NULL)
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
    /* Pas de réduction sous la taille minimale des tuiles (100×100). */
    gtk_paned_set_shrink_start_child(GTK_PANED(content), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(content), FALSE);
    /* PAS de handler notify::position ici : les positions sont lues une
     * seule fois à la fermeture (collect_fractions_walk) — suivre les
     * notifications en continu sauvegardait les valeurs d'allocation
     * internes de GTK (fractions corrompues). */
    return content;
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

/* À l'émission de « map », l'allocation n'a pas encore eu lieu : les
 * largeurs/hauteurs valent 0 et set_paned_positions sautait tout — les
 * fractions sauvegardées n'étaient JAMAIS restaurées au boot. On applique
 * à la prochaine itération de la boucle, une fois les tailles allouées. */
static gboolean
apply_positions_idle(gpointer data)
{
    set_paned_positions((App *)data);
    return G_SOURCE_REMOVE;
}

static void
on_layout_map(GtkWidget G_GNUC_UNUSED *widget, gpointer data)
{
    g_idle_add(apply_positions_idle, data);
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
    if (g_getenv("CDB_DEBUG") != NULL)
        g_signal_connect(app->layout_root, "destroy",
                         G_CALLBACK(trace_destroy), app);
    gtk_widget_set_vexpand(app->layout_root, TRUE);
    gtk_box_append(GTK_BOX(app->layout_holder), app->layout_root);
    g_signal_connect_after(app->layout_root, "map",
                           G_CALLBACK(on_layout_map), app);
    gtk_widget_set_visible(app->layout_root, TRUE);
    trace_state(app, "render_layout: fin");
}

/* ------------------------------------------------ */
/* Persistance de la fenêtre (taille + état)         */
/*                                                   */
/* Wayland interdit le positionnement ; la taille et */
/* les états maximisé/fullscreen sont restaurables.  */
/* ------------------------------------------------ */

static char *
window_state_path(void)
{
    return session_config_path("window.json");
}

static void
window_state_load(App *app)
{
    char       *path = window_state_path();
    JsonParser *parser;
    JsonObject *obj;

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return;
    }
    parser = json_parser_new();
    if (json_parser_load_from_file(parser, path, NULL) &&
        (obj = json_node_get_object(json_parser_get_root(parser))) != NULL) {
        if (json_object_has_member(obj, "width") &&
            json_object_has_member(obj, "height")) {
            int w = (int)json_object_get_int_member(obj, "width");
            int h = (int)json_object_get_int_member(obj, "height");

            if (w > 0 && h > 0)
                gtk_window_set_default_size(app->win, w, h);
        }
        /* Les états s'appliquent au map de la fenêtre. */
        if (json_object_has_member(obj, "fullscreen") &&
            json_object_get_boolean_member(obj, "fullscreen"))
            gtk_window_fullscreen(app->win);
        else if (json_object_has_member(obj, "maximized") &&
                 json_object_get_boolean_member(obj, "maximized"))
            gtk_window_maximize(app->win);
    }
    g_object_unref(parser);
    g_free(path);
}

static void
window_state_save(App *app)
{
    JsonBuilder *builder;
    JsonNode    *root_node;
    gchar       *text;
    char        *dir;
    int          w, h;

    /* Taille réelle allouée (Wayland : pas de position à sauver). */
    w = gtk_widget_get_width(GTK_WIDGET(app->win));
    h = gtk_widget_get_height(GTK_WIDGET(app->win));

    /* Garde-fou : fenêtre jamais mappée (arrêt précoce) ou taille
     * absurde → ne pas écraser l'état précédent avec de la bouillie. */
    if (!gtk_widget_get_mapped(GTK_WIDGET(app->win)) ||
        w < 200 || h < 200)
        return;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "width");
    json_builder_add_int_value(builder, w);
    json_builder_set_member_name(builder, "height");
    json_builder_add_int_value(builder, h);
    json_builder_set_member_name(builder, "maximized");
    json_builder_add_boolean_value(builder,
                                   gtk_window_is_maximized(app->win));
    json_builder_set_member_name(builder, "fullscreen");
    json_builder_add_boolean_value(builder,
                                   gtk_window_is_fullscreen(app->win));
    json_builder_end_object(builder);

    dir = g_path_get_dirname(window_state_path());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    root_node = json_builder_get_root(builder);
    text = json_to_string(root_node, TRUE);
    {
        char     *path = window_state_path();
        GError   *error = NULL;

        if (!g_file_set_contents(path, text, -1, &error)) {
            g_printerr("CDB: écriture window.json : %s\n",
                       error->message);
            g_error_free(error);
        }
        g_free(path);
    }
    g_free(text);
    json_node_unref(root_node);
    g_object_unref(builder);
}

/* Fermeture : lecture des positions RÉELLES des poignées dans le modèle,
 * sauvegarde du layout + de l'état de la fenêtre. */
static void
collect_fractions_walk(GtkWidget *widget, Layout *node)
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
        node->fraction = CLAMP((double)gtk_paned_get_position(paned) / total,
                               0.05, 0.95);
    collect_fractions_walk(gtk_paned_get_start_child(paned), node->a);
    collect_fractions_walk(gtk_paned_get_end_child(paned), node->b);
}

/* Fermeture (clic sur X / gtk_window_close) : sauvegarde de l'état puis
 * laisse la fenêtre se fermer. C'est LE point de sauvegarde : au signal
 * ::shutdown de l'application, la fenêtre est déjà finalisée. */
static gboolean
on_close_request(GtkWindow G_GNUC_UNUSED *win, gpointer data)
{
    App *app = data;

    if (app->layout_root != NULL && app->layout != NULL) {
        collect_fractions_walk(app->layout_root, app->layout);
        layout_save(app->layout);
    }
    window_state_save(app);
    return FALSE; /* la fermeture continue */
}

/* ------------------------------------------------ */
/* Fenêtres-modales : tuile vide à attribuer         */
/*                                                   */
/* Une modale contient UNE tuile autonome (hors      */
/* layout) qui attend qu'on lui attribue sa pièce    */
/* via son menu (titlebar). Max 4 (MODAL_MAX).       */
/* ------------------------------------------------ */

typedef struct {
    App       *app;
    Layout    *node;      /* tuile autonome (parent NULL) */
    GtkWindow *win;
    GtkLabel  *title_lbl; /* label dans la titlebar de la modale */
} ModalCtx;

typedef struct {
    App        *app;
    ModalCtx   *ctx;
    const char *piece;
} ModalPieceAction;

static void on_new_window_activated(GSimpleAction *action, GVariant *param,
                                    gpointer data);
static void on_new_session_activated(GSimpleAction *action, GVariant *param,
                                     gpointer data);
static gboolean modal_open_empty(App *app);

static void
on_new_window_activated(GSimpleAction G_GNUC_UNUSED *action,
                        GVariant G_GNUC_UNUSED *param, gpointer data)
{
    App *app = data;

    if (!modal_open_empty(app))
        g_warning("CDB: limite de %d modales atteinte", MODAL_MAX);
}

/* About CDB : GtkAboutDialog standard. */
static void
on_about_activated(GSimpleAction G_GNUC_UNUSED *action,
                   GVariant G_GNUC_UNUSED *param, gpointer data)
{
    App            *app = data;
    GtkWidget      *dlg = gtk_about_dialog_new();
    const char     *authors[] = { "SIEB", NULL };

    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dlg), "CodeDashBoard");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dlg), "0.1");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dlg),
        "CodeDashBoard (CDB) by SIEB is a C IDE for developing with LLM "
        "on a lightweight footprint on RAM and CPU.");
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dlg),
                                   "Copyright © 2026 SIEB");
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dlg), authors);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), app->win);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_present(GTK_WINDOW(dlg));
}

/* Settings : modale avec la tuile settings (accordéon). */
static void
on_settings_activated(GSimpleAction G_GNUC_UNUSED *action,
                      GVariant G_GNUC_UNUSED *param, gpointer data)
{
    App       *app = data;
    GtkWidget *win = gtk_window_new();
    GtkWidget *content = build_settings(app);

    gtk_window_set_title(GTK_WINDOW(win), "Settings — CodeDashBoard");
    gtk_window_set_transient_for(GTK_WINDOW(win), app->win);
    /* NON modale : l'utilisateur peut avoir besoin de copier/coller
     * (clé API, endpoint…) depuis l'éditeur pendant que Settings est
     * ouvert. */
    gtk_window_set_default_size(GTK_WINDOW(win), 800, 600);
    gtk_window_set_child(GTK_WINDOW(win), content);
    gtk_window_present(GTK_WINDOW(win));
}

/* Exit : ferme la session courante (passe par close-request → layout +
 * window state sauvegardés). Les autres sessions ne sont pas touchées. */
static void
on_exit_activated(GSimpleAction G_GNUC_UNUSED *action,
                  GVariant G_GNUC_UNUSED *param, gpointer data)
{
    App *app = data;

    if (app->win != NULL)
        gtk_window_destroy(app->win);
}

/* New Session : ouvre juste un nouveau PID du binaire (spawn sans
 * CDB_SESSION) — le nouveau processus suit la logique standard de
 * lancement (000 si seul, dialogue numéro sinon). */
static void
on_new_session_activated(GSimpleAction G_GNUC_UNUSED *action,
                         GVariant G_GNUC_UNUSED *param, gpointer data)
{
    App       *app = data;
    char       self[PATH_MAX];
    ssize_t    r;
    char      *argv[2];
    posix_spawnattr_t attr;
    pid_t      pid = -1;
    char     **envp;
    gsize      envc = 0, i;

    (void)app;
    r = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (r <= 0) {
        g_warning("CDB: readlink /proc/self/exe échoué");
        return;
    }
    self[r] = '\0';

    while (environ[envc] != NULL)
        envc++;
    envp = g_new(char *, envc + 1);
    for (i = 0; i < envc; i++)
        envp[i] = environ[i];
    envp[envc] = NULL;

    argv[0] = self;
    argv[1] = NULL;
    posix_spawnattr_init(&attr);
    /* Détache l'enfant : nouvelle session POSIX (pas de terminal
     * contrôleur), indépendant du cycle de vie du parent. */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    {
        int rc = posix_spawn(&pid, self, NULL, &attr, argv, envp);

        posix_spawnattr_destroy(&attr);
        g_free(envp);
        if (rc != 0)
            g_warning("CDB: échec du spawn de la nouvelle session");
    }
}

static void
on_modal_piece_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    ModalPieceAction *act = data;
    ModalCtx         *ctx = act->ctx;
    GtkWidget        *pop =
        gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);

    /* La tuile autonome change de pièce : titre + contenu suivent.
     * L'ancien contenu est détruit par set_child. */
    layout_retile(ctx->node, act->piece);
    gtk_label_set_text(ctx->title_lbl, layout_name(act->piece));
    gtk_window_set_title(ctx->win, layout_name(act->piece));
    gtk_window_set_child(ctx->win, create_piece(act->piece, act->app));

    if (pop != NULL)
        gtk_popover_popdown(GTK_POPOVER(pop));
}

/* Menu d'une modale : uniquement « changer de pièce » (pas de split/
 * remove — la tuile est hors layout). */
static GtkWidget *
build_modal_menu(App *app, ModalCtx *ctx)
{
    GtkWidget  *pop = gtk_popover_new();
    GtkWidget  *menu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    const char *pieces[] = { "editor", "explorer", "bash", "llm", "empty" };

    gtk_widget_set_margin_start(menu, 8);
    gtk_widget_set_margin_end(menu, 8);
    gtk_widget_set_margin_top(menu, 6);
    gtk_widget_set_margin_bottom(menu, 6);

    for (gsize i = 0; i < G_N_ELEMENTS(pieces); i++) {
        ModalPieceAction *a = g_new0(ModalPieceAction, 1);
        GtkWidget        *b = gtk_button_new_with_label(
                                     layout_name(pieces[i]));

        a->app = app;
        a->ctx = ctx;
        a->piece = pieces[i];
        gtk_widget_set_halign(b, GTK_ALIGN_FILL);
        if (strcmp(pieces[i], ctx->node->id) == 0)
            gtk_widget_set_sensitive(b, FALSE); /* pièce actuelle */
        g_signal_connect(b, "clicked",
                         G_CALLBACK(on_modal_piece_clicked), a);
        gtk_box_append(GTK_BOX(menu), b);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), menu);
    return pop;
}

/* Ouvre une fenêtre avec une tuile VIDE : elle attend l'attribution de
 * sa pièce via le menu de sa titlebar. Renvoie FALSE à la limite. */
static gboolean
modal_open_empty(App *app)
{
    ModalCtx  *ctx = g_new0(ModalCtx, 1);
    GtkWidget *content = create_piece("empty", app);
    GtkWidget *titlebar = gtk_header_bar_new();
    GtkWidget *lbl = gtk_label_new(layout_name("empty"));
    GtkWidget *menu_btn = gtk_menu_button_new();
    GtkWindow *win = NULL;

    ctx->app = app;
    ctx->node = layout_tile("empty"); /* autonome, hors layout */

    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(titlebar), TRUE);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(titlebar), lbl);
    ctx->title_lbl = GTK_LABEL(lbl);

    gtk_widget_add_css_class(menu_btn, "flat");
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn),
                                  "open-menu-symbolic");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_btn),
                                build_modal_menu(app, ctx));
    gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), menu_btn);

    if (!modal_open(app->win, &app->modal_count, titlebar, content, &win)) {
        g_object_ref_sink(content);
        g_object_unref(content);
        g_object_ref_sink(titlebar);
        g_object_unref(titlebar);
        g_free(ctx);
        return FALSE;
    }
    ctx->win = win;
    /* Libère le contexte quand la fenêtre meurt. */
    g_object_set_data_full(G_OBJECT(win), "modal-ctx", ctx, g_free);
    return TRUE;
}

static void
on_activate(GtkApplication *gtk_app, gpointer data)
{
    App      *app = data;
    GtkWidget *header;
    GtkWidget *statusbar;
    GtkWidget *sep;
    AdwStyleManager *style_mgr;

    /* Session (000-999) : CDB_SESSION, sinon 000 si aucune autre
     * instance, sinon dialogue — ici, GTK est initialisé (display
     * disponible pour le dialogue). Annulé = quitter sans fenêtre. */
    if (!session_init())
        return;

    app->win = GTK_WINDOW(gtk_application_window_new(gtk_app));
    gtk_window_set_title(app->win, "CodeDashBoard");
    gtk_window_set_default_size(app->win, 1280, 800);
    window_state_load(app);
    g_signal_connect(app->win, "close-request",
                     G_CALLBACK(on_close_request), app);

    /* CSS applicatif : titres de tuiles discrets (10 pt). */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        const char *data_css =
            ".tile-title { font-size: 10pt; }\n"
            /* Bouton « Configurer… » du sélecteur LLM : même fine
             * print que le reste — le style bouton par défaut rend le
             * label en gras et casse la hiérarchie 10 px. */
            ".initprompt-editor text { font-family: monospace; font-size: 10pt; }\n"
            "button.llm-configure { font-size: 10pt; "
            "font-weight: normal; padding: 2px 6px; }\n"
            "menubutton.tile-menu > button { font-size: 9pt; "
            "padding: 0 4px; min-height: 0; }\n"
            /* Barre de composition LLM : bloc plein légèrement plus sombre
             * que la tuile ; la saisie y est transparente (fond du bloc). */
            ".llm-compose { background: alpha(shade(@view_bg_color, 0.92), 1); "
            "border: 1px solid @borders; border-radius: 6px; }\n"
            ".llm-compose-entry, .llm-compose-entry text "
            "{ background: none; }\n"
            ".llm-compose-entry { padding: 4px 8px; }\n"
            ".llm-compose-send { padding: 2px 8px; min-height: 0; }\n"
            /* Point orange d'onglet bash : commande /CDB:: en cours. */
            ".cdb-busy-dot { color: orange; font-size: 8pt; }\n"
            /* Sélecteur de modèle : label phrasique discret — pas de fond,
             * pas de bordure, 10 pt, chevron atténué (style « gracile »). */
            "menubutton.llm-model-btn > button { background: none; "
            "border: none; box-shadow: none; min-height: 0; padding: 0 2px; "
            "font-size: 10pt; font-weight: normal; }\n"
            "menubutton.llm-model-btn > button:hover:not(:checked) "
            "{ background: none; }\n"
            "menubutton.llm-model-btn > button > box > label "
            "{ opacity: 0.85; }\n"
            /* Popover du sélecteur : flat/square comme le thème — fond
             * uni, coins droits, bordure fine ; rangées transparentes. */
            "popover.llm-model-pop > contents "
            "{ border-radius: 0; background: @view_bg_color; }\n"
            "popover.llm-model-pop listbox > row "
            "{ background: none; border-radius: 0; }\n"
            "popover.llm-model-pop listbox > row:hover "
            "{ background: alpha(@theme_fg_color, 0.06); }\n"
            /* Titlebar : teintes uniformes, tout en 10 pt non gras. */
            "headerbar { font-size: 10pt; font-weight: normal; }\n"
            "headerbar .title { font-weight: normal; font-size: 10pt; }\n"
            ".titlebar-brand { padding: 0; font-size: 10pt; "
            "font-weight: normal; }\n"
            "menubutton.titlebar-brand > button { background: none; "
            "box-shadow: none; min-height: 0; min-width: 0; padding: 0 2px; "
            "margin: 0; border: none; }\n"
            "menubutton.titlebar-brand > button:hover:not(:checked) "
            "{ background: none; }\n"
            ".titlebar-sep { padding: 0 4px; font-size: 10pt; }\n"
            ".titlebar-signature { font-size: 10pt; font-weight: normal; }\n"
            ".titlebar-file { font-weight: normal; font-size: 10pt; }\n"
            "headerbar { min-height: 0; padding: 0 8px; }\n"
            "headerbar > box { min-height: 0; }\n"
            "headerbar button { min-height: 0; min-width: 0; padding: 0 6px; "
            "margin: 0; }\n"
            "headerbar button > image { min-height: 0; min-width: 0; "
            "-gtk-icon-size: 12px; }\n"
            "headerbar windowcontrols { min-height: 0; }\n"
            "headerbar windowcontrols > button { min-height: 0; "
            "min-width: 0; padding: 0 4px; margin: 0; border: none; "
            "border-radius: 0; }\n"
            "headerbar windowcontrols > button > image { min-height: 12px; "
            "min-width: 12px; -gtk-icon-size: 12px; }\n";

        gtk_css_provider_load_from_string(css, data_css);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }

    /* HeaderBar (sans bouton Ouvrir : l'explorateur suffit). */
    /* Titlebar : [logo système] CDB(menu, vide) :: chemin — signature
     * (titre de fenêtre en repli de la HeaderBar) :: boutons système.
     * Pas de widget titre : la HeaderBar affiche gtk_window_get_title(),
     * déjà tenu à jour par update_modified_indicator. */
    header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    {
        /* Menu « CDB » : vide pour l'instant (entrées à venir). */
        GMenu     *menu = g_menu_new();
        GtkWidget *menu_btn = gtk_menu_button_new();
        GtkWidget *brand = gtk_label_new("CDB");
        GtkWidget *sep = gtk_label_new("::");

        g_menu_append(menu, "About CDB", "win.about");
        g_menu_append(menu, "Settings", "win.settings");
        g_menu_append(menu, "Exit", "win.exit");

        gtk_widget_add_css_class(brand, "titlebar-brand");
        gtk_widget_add_css_class(sep, "titlebar-sep");
        gtk_widget_set_valign(sep, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(menu_btn, "flat");
        gtk_widget_add_css_class(menu_btn, "titlebar-brand");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn),
                                       G_MENU_MODEL(menu));
        gtk_menu_button_set_child(GTK_MENU_BUTTON(menu_btn), brand);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(header), menu_btn);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(header), sep);
        g_object_unref(menu);

        /* Bouton « nouvelle fenêtre » : son menu porte l'isolation de
         * processus — Nouvelle fenêtre (modale, même processus) et
         * Nouvelle session (dialogue numéro puis spawn avec CDB_SESSION,
         * qui court-circuite le dialogue dans la nouvelle instance). */
        {
            GtkWidget *new_win_btn = gtk_menu_button_new();
            GMenu     *menu = g_menu_new();

            g_menu_append(menu, "Nouvelle fenêtre", "win.new-window");
            g_menu_append(menu, "Nouvelle session…", "win.new-session");
            gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(new_win_btn),
                                          "window-new-symbolic");
            gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(new_win_btn),
                                           G_MENU_MODEL(menu));
            gtk_widget_add_css_class(new_win_btn, "flat");
            gtk_header_bar_pack_start(GTK_HEADER_BAR(header), new_win_btn);
            g_object_unref(menu);
        }
    }
    gtk_window_set_titlebar(app->win, header);

    /* Chemin + signature dans le widget titre (centré, non gras). */
    {
        GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

        /* Pas d'alignement par baseline : les labels sont centrés
         * verticalement (le « :: » sinon paraît plus bas que les
         * capitales de la signature). */
        gtk_box_set_baseline_position(GTK_BOX(title_box),
                                      GTK_BASELINE_POSITION_CENTER);
        GtkWidget *sep1 = gtk_label_new("::");
        GtkWidget *sep2 = gtk_label_new("::");
        GtkWidget *sep3 = gtk_label_new("::");
        GtkWidget *sig = gtk_label_new("CodeDashBoard by SIEB");

        gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(sep1, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(sep2, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(sep3, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(sig, GTK_ALIGN_CENTER);

        gtk_widget_add_css_class(sep1, "titlebar-sep");
        gtk_widget_add_css_class(sep2, "titlebar-sep");
        gtk_widget_add_css_class(sep3, "titlebar-sep");
        gtk_widget_add_css_class(sig, "titlebar-signature");
        app->header_file = GTK_LABEL(gtk_label_new(""));
        gtk_label_set_ellipsize(app->header_file, PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_valign(GTK_WIDGET(app->header_file), GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(GTK_WIDGET(app->header_file),
                                 "titlebar-file");
        gtk_box_append(GTK_BOX(title_box), sep1);
        gtk_box_append(GTK_BOX(title_box), GTK_WIDGET(app->header_file));
        gtk_box_append(GTK_BOX(title_box), sep2);
        gtk_box_append(GTK_BOX(title_box), sig);
        gtk_box_append(GTK_BOX(title_box), sep3);
        gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title_box);
    }

    /* Actions du menu "+" du panneau Explorateur. */
    {
        const GActionEntry win_actions[] = {
            { "add-structure", on_add_structure, NULL, NULL, NULL, { 0 } },
            { "add-project",   on_add_project,   NULL, NULL, NULL, { 0 } },
            { "save",          on_save_activated, NULL, NULL, NULL, { 0 } },
            { "new-window",    on_new_window_activated, NULL, NULL, NULL, { 0 } },
            { "new-session",   on_new_session_activated, NULL, NULL, NULL, { 0 } },
            { "about",         on_about_activated, NULL, NULL, NULL, { 0 } },
            { "settings",      on_settings_activated, NULL, NULL, NULL, { 0 } },
            { "exit",          on_exit_activated, NULL, NULL, NULL, { 0 } },
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

    /* Scénarios de test : déclenchement immédiat, pas courts
     * (cdb_test_delay), sortie automatique — la suite entière se
     * compte en centaines de ms. */
    {
        guint tms = cdb_test_delay();

        /* CDB_TEST_CLOSE=1 : ferme la fenêtre (close-request → save). */
        if (g_getenv("CDB_TEST_CLOSE") != NULL)
            g_timeout_add(tms * 2, test_close_idle, app->win);
        /* CDB_TEST_MODAL=1 : 5 modales (la 5e doit être refusée). */
        if (g_getenv("CDB_TEST_MODAL") != NULL) {
            g_timeout_add(tms, test_modal_idle, app);
            g_timeout_add(tms * 3, test_quit_idle, gtk_app);
        }
        /* CDB_TEST_SETTINGS=1 : ouvre/ferme Settings ×2. */
        if (g_getenv("CDB_TEST_SETTINGS") != NULL)
            g_timeout_add(tms, test_settings_step, app);
        /* CDB_TEST_LAYOUT=1 (alias SPLIT/GRID) : pavage complet puis
         * rounds de churn ; CDB_TEST_REPEAT=N ajoute des tours. */
        if (g_getenv("CDB_TEST_LAYOUT") != NULL ||
            g_getenv("CDB_TEST_SPLIT") != NULL ||
            g_getenv("CDB_TEST_GRID") != NULL)
            g_idle_add(test_layout_idle, app);
    }
}

/* MODE TEST (debug) : voir CDB_TEST_SPLIT dans on_activate. */
static Layout *test_first_tile(Layout *n)
{
    return n->kind == LAYOUT_TILE ? n : test_first_tile(n->a);
}

static void
test_split_sequence(App *app)
{
    Layout *t;

    /* SÉQUENCE STRESS : split+retile+remove enchaînés, avec re-rendu à
     * chaque étape (comme les clics du user). PAS de layout_save : ne
     * pas écraser le layout.json du user. */
    t = test_first_tile(app->layout);
    app->layout = layout_split(app->layout, t, TRUE, t->id);
    render_layout(app);

    t = test_first_tile(app->layout);
    if (strcmp(t->id, "editor") == 0)
        layout_retile(t, "explorer");
    else
        layout_retile(t, "editor");
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
        render_layout(app);
    }
}

/* Scénario LAYOUT (fusion GRID+SPLIT) : pavage complet puis rounds de
 * churn. CDB_TEST_REPEAT=N ajoute des tours (chasse aux courses). */
static gboolean
test_layout_idle(gpointer data)
{
    App      *app = data;
    const char *rep = g_getenv("CDB_TEST_REPEAT");
    int       rounds = rep != NULL ? atoi(rep) : 1;

    test_grid_sequence(app);
    for (int r = 0; r < rounds; r++)
        test_split_sequence(app);
    g_timeout_add(cdb_test_delay(), test_quit_idle,
                  g_application_get_default());
    return G_SOURCE_REMOVE;
}

static gboolean
test_quit_idle(gpointer data)
{
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_REMOVE;
}

/* MODE TEST (debug) : voir CDB_TEST_GRID dans on_activate. Construit le
 * pavage V( H(A,H(D,E)), H(V(B,C),F) ) — « A:B:C/A:B:C/D:B:C/E:F:F » — en
 * n'utilisant que layout_split/layout_retile sur tuiles ET blocs. */
static void
test_grid_sequence(App *app)
{
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
}

/* MODE TEST (debug) : voir CDB_TEST_CLOSE dans on_activate. */
static gboolean
test_close_idle(gpointer data)
{
    gtk_window_close(GTK_WINDOW(data));
    return G_SOURCE_REMOVE;
}

/* MODE TEST (debug) : CDB_TEST_MODAL=1 ouvre 5 fenêtres vides après
 * 1 s — la 5e doit être refusée (limite MODAL_MAX=4). */
static gboolean
test_modal_idle(gpointer data)
{
    App *app = data;

    for (int i = 0; i < 5; i++)
        g_printerr("CDB: modal %d -> %s\n", i + 1,
                   modal_open_empty(app) ? "ouverte" : "REFUSÉE");
    return G_SOURCE_REMOVE;
}

/* MODE TEST (debug) : SIEB_TEST_SETTINGS=1 ouvre/ferme la fenêtre
 * settings deux fois (reproduction du crash au X). */
static gboolean
test_settings_step(gpointer data)
{
    App              *app = data;
    static int        step = 0;
    /* CDB_TEST_SETTINGS_DELAY : délai d'ouverture→fermeture en ms
     * (course avec le fetch /models). Défaut 500. */
    const int         delay =
        g_getenv("CDB_TEST_SETTINGS_DELAY")
            ? (int)g_ascii_strtoll(g_getenv("CDB_TEST_SETTINGS_DELAY"),
                                   NULL, 10)
            : 80;

    switch (step++) {
    case 0:
        g_action_group_activate_action(G_ACTION_GROUP(app->win),
                                       "settings", NULL);
        g_timeout_add(delay, test_settings_step, app);
        break;
    case 1:
    case 3: {
        /* Retrouve la fenêtre settings PAR TITRE avec une ref possédée
         * pendant l'usage — jamais de pointeur brut entre les étapes
         * (la fenêtre peut être détruite dès close). */
        GtkWindow *swin = NULL;
        GList     *l;

        g_printerr("CDB: fermeture settings (%s)\n",
                   step == 2 ? "1" : "2");
        l = gtk_window_list_toplevels();
        for (; l != NULL && swin == NULL; l = l->next) {
            const char *title =
                gtk_window_get_title(GTK_WINDOW(l->data));

            if (l->data != app->win && title != NULL &&
                strstr(title, "Settings") != NULL)
                swin = g_object_ref(GTK_WINDOW(l->data));
        }
        g_list_free(l);
        if (swin != NULL) {
            gtk_window_close(swin);
            g_object_unref(swin);
        } else {
            g_printerr("CDB: settings introuvable !\n");
        }
        if (step == 4) {
            /* Dernière étape : quitte proprement (les outils d'analyse
             * type valgrind doivent voir un exit normal). */
            g_timeout_add(300, test_quit_idle,
                          g_application_get_default());
        } else {
            g_timeout_add(delay, test_settings_step, app);
        }
        break;
    }
    case 2:
        g_printerr("CDB: réouverture settings\n");
        g_action_group_activate_action(G_ACTION_GROUP(app->win),
                                       "settings", NULL);
        g_timeout_add(delay, test_settings_step, app);
        break;
    }
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
    app->llm_cfg = llm_config_load();

    /* NON_UNIQUE : chaque processus est sa propre application — sinon
     * l'enfant délègue son activation au primaire (même ID D-Bus) et
     * meurt, en perturbant la fenêtre du parent. */
    gtk_app = GTK_APPLICATION(adw_application_new("org.sieb.cdb",
                                                  G_APPLICATION_NON_UNIQUE));
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), app);

    status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    /* Persiste les fichiers sales avant de quitter (le buffer courant inclus). */
    if (app->dirty != NULL)
        dirty_persist_now(app->dirty);
    g_object_unref(gtk_app);
    if (app->diff_timer != 0)
        g_source_remove(app->diff_timer);
    if (app->tree_model != NULL)
        g_object_unref(app->tree_model);
    if (app->selection != NULL)
        g_object_unref(app->selection);
    g_hash_table_destroy(app->multi_paths);
    g_hash_table_destroy(app->files); /* libère chaque PerFile */
    layout_free(app->layout);
    dirty_store_free(app->dirty);
    llm_config_free(app->llm_cfg);
    g_free(app);

    return status;
}
