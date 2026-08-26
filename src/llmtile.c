/*
 * llmtile.c : tuile LLM (vue) — historique GtkTextBuffer, selecteur
 * de modele, slots, approbations /CDB:: rendues depuis le core.
 * Aucune propriete conversationnelle : tout vient de LlmCore.
 */

#define _POSIX_C_SOURCE 200809L
#include "llm.h"
#include "session.h"
#include "mdview.h"
#include "bashpanel.h"
#include "modal.h"
#include "llmslots.h"
#include "roots.h"
#include "llmlive.h"
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <string.h>
#include <gdk/gdkkeysyms.h>


/* ===== Constantes de vue (split C0) ===== */

#define CDB_RETRY_MAX 3

static const char *const LLM_STATUS_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};

#define LLM_STATUS_N_FRAMES 10

#define LLM_STATUS_TICK_MS 80

#define CDB_ENTRY_MAX_LINES 8


/* ===== CORPS VUE (split C0) ===== */

void
llm_tile_free(gpointer data)
{
    LlmTile *t = data;

    /* REQUÊTE EN VOL : détacher la tuile AVANT toute libération.
     * Les callbacks async (llm_send_done, llm_stream_read,
     * llm_retry_tick) testent req->tile == NULL et se retirent sans
     * toucher à la tuile morte. Sans ce garde-fou, fermer une pièce
     * ou la fenêtre pendant un streaming = use-after-free sur
     * req->tile (crash « malloc(): unaligned tcache » constaté). */
    /* Détachement : la vue se retire de la liste de diffusion. */
    if (t->core != NULL)
        g_ptr_array_remove_fast(t->core->views, t);

    if (t->status_timeout_id != 0) {
        g_source_remove(t->status_timeout_id);
        t->status_timeout_id = 0;
    }
    if (t->pending_images != NULL)
        g_ptr_array_unref(t->pending_images);
    /* cfg EMPRUNTÉE à App (app->llm_cfg) : PAS libérée ici — main()
     * la libère une seule fois en fin de programme. */
    if (t->sections != NULL)
        g_ptr_array_unref(t->sections); /* libère les ModelSection */
    if (t->actions != NULL)
        g_object_unref(t->actions);
    g_free(t);
}

void
model_section_free(gpointer data)
{
    ModelSection *sec = data;

    g_free(sec->provider);
    llm_models_free(sec->models);
    g_free(sec);
}

gboolean
llm_model_matches(const char *query, const char *id)
{
    char *q, *lid;
    gboolean ok;

    if (query == NULL || query[0] == '\0')
        return TRUE;
    q = g_utf8_casefold(query, -1);
    lid = g_utf8_casefold(id, -1);
    ok = strstr(lid, q) != NULL;
    g_free(q);
    g_free(lid);
    return ok;
}

void
llm_model_button_refresh(LlmTile *t)
{
    const char *slug;
    const char *name = NULL;
    const char *prov = NULL;
    char       *label;

    if (t->cfg == NULL || t->cfg->model == NULL || t->cfg->model[0] == '\0') {
        if (t->model_phrase != NULL)
            gtk_label_set_text(GTK_LABEL(t->model_phrase), "?");
        return;
    }
    slug = t->cfg->model;
    /* Nom long : cherché dans les sections déjà fetchées. */
    if (t->sections != NULL)
        for (guint i = 0; i < t->sections->len && name == NULL; i++) {
            ModelSection *sec = g_ptr_array_index(t->sections, i);

            if (t->cfg->provider != NULL &&
                g_strcmp0(sec->provider, t->cfg->provider) != 0)
                continue;
            prov = sec->provider;
            if (sec->models != NULL)
                for (int k = 0; sec->models[k].id != NULL; k++)
                    if (g_strcmp0(sec->models[k].id, slug) == 0) {
                        name = sec->models[k].name;
                        break;
                    }
        }
    if (prov == NULL)
        prov = t->cfg->provider;
    if (name != NULL)
        label = g_strdup_printf("%s par %s · slug %s", name, prov, slug);
    else if (prov != NULL)
        label = g_strdup_printf("%s · slug %s", prov, slug);
    else
        label = g_strdup(slug);
    if (t->model_phrase != NULL)
        gtk_label_set_text(GTK_LABEL(t->model_phrase), label);
    g_free(label);
}

void
llm_model_section_refresh(LlmTile *t, ModelSection *sec)
{
    char *filter = llm_config_get_allowed_models(sec->provider);

    for (GtkWidget *child = gtk_widget_get_first_child(sec->list);
         child != NULL; ) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);

        gtk_list_box_remove(GTK_LIST_BOX(sec->list), child);
        child = next;
    }
    if (sec->models != NULL) {
        for (int i = 0; sec->models[i].id != NULL; i++) {
            const char *id = sec->models[i].id;
            gboolean active = t->cfg != NULL &&
                              strcmp(t->cfg->provider,
                                     sec->provider) == 0 &&
                              t->cfg->model != NULL &&
                              strcmp(t->cfg->model, id) == 0;

            if (!llm_model_allowed(filter, id))
                continue;
            {
                GtkWidget *lbl;
                GtkWidget *row;
                const char *display = sec->models[i].name != NULL
                                          ? sec->models[i].name
                                          : id;
                char      *shown = active
                                       ? g_strdup_printf("\u2713 %s",
                                                         display)
                                       : g_strdup(display);

                lbl = gtk_label_new(shown);
                row = gtk_list_box_row_new();
                gtk_widget_set_halign(lbl, GTK_ALIGN_START);
                gtk_widget_set_margin_start(lbl, 8);
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
                gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
                g_object_set_data_full(G_OBJECT(row), "model-id",
                                       g_strdup(id), g_free);
                g_object_set_data_full(G_OBJECT(row), "model-provider",
                                       g_strdup(sec->provider), g_free);
                gtk_list_box_append(GTK_LIST_BOX(sec->list), row);
                g_free(shown);
            }
        }
    }
    g_free(filter);
}

void
on_llm_model_row_activated(GtkListBox G_GNUC_UNUSED *lb,
                           GtkListBoxRow *row, gpointer data)
{
    LlmTile    *t = data;
    const char *id = g_object_get_data(G_OBJECT(row), "model-id");
    const char *prov = g_object_get_data(G_OBJECT(row), "model-provider");

    if (id == NULL || prov == NULL || t->cfg == NULL)
        return;
    if (strcmp(prov, t->cfg->provider) == 0 &&
        g_strcmp0(id, t->cfg->model) == 0) {
        gtk_popover_popdown(GTK_POPOVER(t->model_pop));
        return; /* déjà actif */
    }
    llm_config_switch_active(t->cfg, prov, id);
    for (guint vi = 0; vi < t->core->views->len; vi++)
        llm_model_button_refresh(g_ptr_array_index(t->core->views, vi));
    for (guint i = 0; i < t->sections->len; i++)
        llm_model_section_refresh(t, g_ptr_array_index(t->sections, i));
    llm_model_menu_apply_filter(t);
    gtk_popover_popdown(GTK_POPOVER(t->model_pop));
}

void
llm_model_menu_apply_filter(LlmTile *t)
{
    const char *q = gtk_editable_get_text(GTK_EDITABLE(t->model_search));

    for (guint i = 0; i < t->sections->len; i++) {
        ModelSection *sec = g_ptr_array_index(t->sections, i);
        int           visible = 0;

        for (GtkWidget *row = gtk_widget_get_first_child(sec->list);
             row != NULL; row = gtk_widget_get_next_sibling(row)) {
            const char *id = g_object_get_data(G_OBJECT(row),
                                               "model-id");
            gboolean show = llm_model_matches(
                q, id != NULL ? id : "");

            gtk_widget_set_visible(row, show);
            if (show)
                visible++;
        }
        gtk_widget_set_visible(sec->header, visible > 0);
    }
}

void
on_llm_model_search_changed(GtkSearchEntry G_GNUC_UNUSED *entry,
                            gpointer data)
{
    llm_model_menu_apply_filter(data);
}

void
on_llm_configure_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    LlmTile *t = data;

    if (t->actions != NULL)
        g_action_group_activate_action(t->actions, "settings", NULL);
}

void
on_section_models_fetched(LlmModelInfo *models, gpointer data)
{
    SectionFetchCtx *ctx = data;

    if (ctx->t != NULL && ctx->sec != NULL) {
        llm_models_free(ctx->sec->models);
        ctx->sec->models = models != NULL ? llm_models_copy(models) : NULL;
        llm_model_section_refresh(ctx->t, ctx->sec);
        llm_model_menu_apply_filter(ctx->t);
        /* Le nom long du modèle actif vient peut-être d'arriver. */
        llm_model_button_refresh(ctx->t);
    }
    g_object_unref(ctx->anchor); /* lâche l'ancre : teardown normal */
    g_free(ctx);
}

void
llm_model_pop_mapped(GtkWidget G_GNUC_UNUSED *w, gpointer data)
{
    LlmTile *t = data;

    llm_model_menu_ensure(t);
    /* Largeur du popover = celle du bouton sélecteur : le menu termine
     * juste à droite du chevron. */
    llm_model_pop_width_sync(t);
    /* Recherche réinitialisée à chaque ouverture. */
    gtk_editable_set_text(GTK_EDITABLE(t->model_search), "");
}

void
llm_model_pop_width_sync(LlmTile *t)
{
    int width;

    if (t->model_btn == NULL || t->model_pop == NULL)
        return;
    width = gtk_widget_get_width(t->model_btn);
    if (width > 100)
        gtk_widget_set_size_request(t->model_pop, width, -1);
}

void
llm_model_chevron_update(GtkWidget *popover, gpointer data)
{
    LlmTile *t = data;

    if (t->chevron != NULL)
        gtk_image_set_from_icon_name(GTK_IMAGE(t->chevron),
            gtk_widget_get_mapped(popover) ? "pan-up-symbolic"
                                           : "pan-down-symbolic");
}

void
llm_model_menu_ensure(LlmTile *t)
{
    char **names;

    if (t->menu_built)
        return;
    t->menu_built = TRUE;

    names = llm_config_provider_names();
    if (names == NULL) {
        GtkWidget *lbl = gtk_label_new(
            "Aucun provider configuré.\nSettings → LLM → Providers");

        gtk_widget_add_css_class(lbl, "dim-label");
        gtk_widget_set_margin_start(lbl, 8);
        gtk_box_append(GTK_BOX(t->rows_box), lbl);
        return;
    }
    for (int i = 0; names[i] != NULL; i++) {
        ModelSection    *sec = g_new0(ModelSection, 1);
        SectionFetchCtx *fctx;

        sec->provider = g_strdup(names[i]);
        sec->header = gtk_label_new(names[i]);
        gtk_widget_add_css_class(sec->header, "dim-label");
        gtk_widget_set_halign(sec->header, GTK_ALIGN_START);
        gtk_widget_set_margin_start(sec->header, 8);
        gtk_widget_set_margin_top(sec->header, 6);
        sec->list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(sec->list),
                                        GTK_SELECTION_NONE);
        gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(sec->list),
                                                  TRUE);
        g_signal_connect(sec->list, "row-activated",
                         G_CALLBACK(on_llm_model_row_activated), t);
        g_ptr_array_add(t->sections, sec);

        gtk_box_append(GTK_BOX(t->rows_box), sec->header);
        gtk_box_append(GTK_BOX(t->rows_box), GTK_WIDGET(sec->list));

        fctx = g_new0(SectionFetchCtx, 1);
        fctx->t = t;
        fctx->anchor = g_object_ref(t->model_pop);
        fctx->sec = sec;
        llm_models_fetch(sec->provider, on_section_models_fetched, fctx);
    }
    g_strfreev(names);
    llm_model_menu_apply_filter(t);
}

char *
llm_persona_load(LlmTile *t)
{
    char *raw = llm_persona_raw();
    char *proj_path;
    char *proj_name;
    char *s1, *s2;

    /* Priorité : CDB_TEST_PROJET > projet sélectionné > cwd. */
    proj_path = g_strdup(g_getenv("CDB_TEST_PROJET"));
    if (proj_path == NULL)
        proj_path = roots_current_project(t->roots, t->multi_paths);
    if (proj_path == NULL)
        proj_path = g_get_current_dir();

    proj_name = g_path_get_basename(proj_path);
    s1 = str_replace_all(raw, "[PROJET]", proj_name);
    s2 = str_replace_all(s1, "[CHEMIN]", proj_path);
    g_free(raw);
    g_free(s1);
    g_free(proj_name);
    g_free(proj_path);
    return s2;
}

void
hist_ensure_voice_tags(LlmTile *t)
{
    if (gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(t->hist),
                                  "voice-cdb") == NULL) {
        gtk_text_buffer_create_tag(
            t->hist, "voice-cdb", "style", PANGO_STYLE_ITALIC,
            "foreground-rgba", &(GdkRGBA){ 0.62, 0.62, 0.68, 1.0 }, NULL);
        gtk_text_buffer_create_tag(t->hist, "voice-sep", "foreground-rgba",
                                   &(GdkRGBA){ 0.55, 0.55, 0.60, 1.0 },
                                   NULL);
    }
}

void
hist_render_actor_header(LlmTile *t, LlmActor actor)
{
    GtkTextIter end;
    const char *label;

    hist_ensure_voice_tags(t);
    gtk_text_buffer_get_end_iter(t->hist, &end);

    switch (actor) {
    case LLMACTOR_USER:
        label = "\n— Éric —\n";
        gtk_text_buffer_insert(t->hist, &end, label, -1);
        break;
    case LLMACTOR_LLM:
        label = "\n— Claude —\n";
        gtk_text_buffer_insert_with_tags_by_name(t->hist, &end, label, -1,
                                                 "voice-sep", NULL);
        break;
    case LLMACTOR_CDB:
        label = "\n— CDB · local —\n";
        gtk_text_buffer_insert_with_tags_by_name(t->hist, &end, label, -1,
                                                 "voice-cdb", NULL);
        break;
    }
}

void
hist_cdb_announce(LlmTile *t, const char *text)
{
    GtkTextIter end;

    hist_render_actor_header(t, LLMACTOR_CDB);
    hist_ensure_voice_tags(t);
    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_insert_with_tags_by_name(t->hist, &end, text, -1,
                                             "voice-cdb", NULL);
    history_push(t, LLMACTOR_CDB, TRUE, text);
}

void
hist_append(LlmTile *t, const char *text)
{
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_insert(t->hist, &end, text, -1);
}

void
hist_update_reply(LlmTile *t)
{
    GtkTextIter end;
    const char *s = t->core->reply->str;
    gsize       len = t->core->reply->len;
    gsize       safe;

    if (len < t->rendered_len) {
        /* Incohérence (ne devrait jamais arriver) : repli sûr —
         * re-rendu complet depuis la marque du tour. */
        GtkTextIter start;

        gtk_text_buffer_get_iter_at_mark(t->hist, &start, t->reply_mark);
        gtk_text_buffer_get_end_iter(t->hist, &end);
        gtk_text_buffer_delete(t->hist, &start, &end);
        gtk_text_buffer_get_end_iter(t->hist, &end);
        md_insert(t->hist, &end, t->core->reply->str);
        t->rendered_len = len;
        llm_scroll_to_end(t);
        return;
    }
    safe = len;
    while (safe > 0 && s[safe - 1] != '\n')
        safe--;
    if (safe > t->rendered_len) {
        gtk_text_buffer_get_end_iter(t->hist, &end);
        md_insert_append(t->hist, &end, s + t->rendered_len,
                         safe - t->rendered_len, FALSE);
        t->rendered_len = safe;
    }

    if (t->tokens_estimated) {
        t->tokens_received = (long)((t->core->reply->len + 3) / 4);
        t->tokens_context = t->tokens_sent + t->tokens_received;
    }
    llm_status_update(t);
    llm_scroll_to_end(t);
}

void
hist_flush_reply(LlmTile *t)
{
    GtkTextIter end;

    if (t->rendered_len >= t->core->reply->len)
        return;
    gtk_text_buffer_get_end_iter(t->hist, &end);
    md_insert_append(t->hist, &end, t->core->reply->str + t->rendered_len,
                     t->core->reply->len - t->rendered_len, TRUE);
    t->rendered_len = t->core->reply->len;
}

void
llm_busy_set(LlmTile *t, gboolean busy)
{
    t->busy = busy;
    gtk_button_set_icon_name(GTK_BUTTON(t->send_btn), busy
                             ? "media-playback-pause-symbolic"
                             : "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(t->send_btn, busy
                                ? "Annuler la génération"
                                : "Envoyer");
    gtk_widget_set_sensitive(t->send_btn, TRUE);

    if (busy)
        llm_status_start(t);
    else
        llm_status_stop(t);
}

gboolean
llm_status_tick(gpointer data)
{
    LlmTile *t = data;

    t->status_logo_frame++;
    gtk_label_set_text(
        GTK_LABEL(t->status_logo),
        LLM_STATUS_FRAMES[t->status_logo_frame % LLM_STATUS_N_FRAMES]);
    llm_status_update(t);
    return G_SOURCE_CONTINUE;
}

void
llm_status_start(LlmTile *t)
{
    if (t->status_rev == NULL || t->status_label == NULL)
        return;

    if (t->status_timeout_id == 0) {
        t->status_started_us = g_get_monotonic_time();
        t->status_timeout_id = g_timeout_add(
            LLM_STATUS_TICK_MS, llm_status_tick, t);
    }

    gtk_widget_set_visible(t->status_rev, TRUE);
    gtk_widget_set_visible(t->status_logo, TRUE);
    t->status_logo_frame = 0;
    gtk_label_set_text(GTK_LABEL(t->status_logo), LLM_STATUS_FRAMES[0]);
    llm_status_update(t);
}

void
llm_status_stop(LlmTile *t)
{
    if (t->status_timeout_id != 0) {
        g_source_remove(t->status_timeout_id);
        t->status_timeout_id = 0;
    }
    t->status_elapsed_us = g_get_monotonic_time() - t->status_started_us;

    if (t->status_logo != NULL)
        gtk_label_set_text(GTK_LABEL(t->status_logo), " ");
    if (t->status_rev != NULL &&
        (t->tokens_sent > 0 || t->tokens_received > 0 ||
         t->tokens_context > 0))
        gtk_widget_set_visible(t->status_rev, TRUE);

    llm_status_update(t);
}

void
llm_status_update(LlmTile *t)
{
    gint64 now, elapsed;
    gint64 total_sec, display_sec;
    char *elapsed_txt;
    char *sent_txt;
    char *received_txt;
    char *context_txt;
    const char *approx;

    if (t->status_rev == NULL || t->status_label == NULL ||
        t->status_sent_label == NULL || t->status_received_label == NULL ||
        t->status_context_label == NULL)
        return;

    now = g_get_monotonic_time();
    elapsed = t->status_timeout_id != 0
              ? now - t->status_started_us
              : t->status_elapsed_us;
    if (elapsed < 0)
        elapsed = 0;

    total_sec = elapsed / G_USEC_PER_SEC;
    display_sec = total_sec % 60;
    if (total_sec >= 3600)
        elapsed_txt = g_strdup_printf(
            "%dh %02dm %02ds",
            (int)(total_sec / 3600),
            (int)((total_sec % 3600) / 60),
            (int)display_sec);
    else if (total_sec >= 60)
        elapsed_txt = g_strdup_printf(
            "%dm %02ds", (int)(total_sec / 60), (int)display_sec);
    else
        elapsed_txt = g_strdup_printf("%ds", (int)display_sec);

    if (t->tokens_context <= 0)
        t->tokens_context = t->tokens_sent + t->tokens_received;
    approx = t->tokens_estimated ? "~" : "";

    sent_txt = g_strdup_printf("%s%ld", approx, t->tokens_sent);
    received_txt = g_strdup_printf("%s%ld", approx, t->tokens_received);
    context_txt = g_strdup_printf("%s%ld", approx, t->tokens_context);

    gtk_label_set_text(GTK_LABEL(t->status_label), elapsed_txt);
    gtk_label_set_text(GTK_LABEL(t->status_sent_label), sent_txt);
    gtk_label_set_text(GTK_LABEL(t->status_received_label), received_txt);
    gtk_label_set_text(GTK_LABEL(t->status_context_label), context_txt);

    g_free(context_txt);
    g_free(received_txt);
    g_free(sent_txt);
    g_free(elapsed_txt);
}

void
cdb_approval_destroy(GtkWidget G_GNUC_UNUSED *w, gpointer data)
{
    CdbApproval *a = data;

    if (a->b_ok != NULL)
        g_signal_handlers_disconnect_by_data(a->b_ok, a);
    if (a->b_no != NULL)
        g_signal_handlers_disconnect_by_data(a->b_no, a);
    g_free(a->cmd);
    g_free(a);
}

LlmTile *
cdb_tile_from_button(GtkButton *btn)
{
    for (GtkWidget *w = GTK_WIDGET(btn); w != NULL;
         w = gtk_widget_get_parent(w)) {
        LlmTile *t = g_object_get_data(G_OBJECT(w), "cdb-llm-tile");

        if (t != NULL)
            return t;
    }
    return NULL;
}



void
hist_cdb_say(LlmTile *t, const char *text)
{
    GtkTextIter end;

    hist_render_actor_header(t, LLMACTOR_CDB);
    hist_ensure_voice_tags(t);
    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_insert_with_tags_by_name(t->hist, &end, text, -1,
                                             "voice-cdb", NULL);
    history_push(t, LLMACTOR_CDB, FALSE, text);
}


void
llm_tile_turn_reset(LlmTile *t)
{
    GtkTextIter end;

    hist_render_actor_header(t, LLMACTOR_LLM);
    md_thinking_reset(t->hist);
    t->rendered_len = 0; /* rendu incrémental repart à zéro */
    gtk_text_buffer_get_end_iter(t->hist, &end);
    if (t->reply_mark == NULL)
        t->reply_mark = gtk_text_buffer_create_mark(t->hist, NULL,
                                                    &end, TRUE);
    else
        gtk_text_buffer_move_mark(t->hist, t->reply_mark, &end);
}

char *
llm_entry_text(LlmTile *t)
{
    GtkTextIter start, end;

    gtk_text_buffer_get_start_iter(t->entry_buf, &start);
    gtk_text_buffer_get_end_iter(t->entry_buf, &end);
    return gtk_text_buffer_get_text(t->entry_buf, &start, &end, FALSE);
}

void
llm_entry_clear(LlmTile *t)
{
    gtk_text_buffer_set_text(t->entry_buf, "", -1);
}

void
llm_entry_resize(LlmTile *t)
{
    int n = gtk_text_buffer_get_line_count(t->entry_buf);
    int lines = n < 1 ? 1 : (n > CDB_ENTRY_MAX_LINES ? CDB_ENTRY_MAX_LINES : n);

    /* ~19 px par ligne : hauteur de ligne + padding (mesuré GTK défaut). */
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(t->entry_scroll), lines * 19 + 12);
}

void
on_llm_clip_texture(GObject *source_object,
                    GAsyncResult *res,
                    gpointer data)
{
    LlmTile     *t = data;
    GdkClipboard *clip = GDK_CLIPBOARD(source_object);
    GdkTexture   *texture;
    GBytes       *png;
    GError       *error = NULL;
    const guchar *bytes_data;
    gsize         bytes_len;
    gchar        *b64;
    gchar        *url;
    gchar        *marker;
    GtkTextBuffer *buf = t->entry_buf;
    GtkTextIter   start, end, iter;

    texture = gdk_clipboard_read_texture_finish(clip, res, &error);
    if (texture == NULL) {
        char *msg = g_strdup_printf(
            "collage image impossible : %s",
            error != NULL ? error->message : "erreur inconnue");

        g_clear_error(&error);
        core_cdb_announce(t->core, msg);
        g_free(msg);
        return;
    }

    png = gdk_texture_save_to_png_bytes(texture);
    g_object_unref(texture);
    if (png == NULL) {
        core_cdb_announce(t->core, "collage image impossible : encodage PNG.");
        return;
    }

    bytes_data = g_bytes_get_data(png, &bytes_len);
    b64 = g_base64_encode(bytes_data, bytes_len);
    url = g_strdup_printf("data:image/png;base64,%s", b64);
    g_free(b64);
    g_bytes_unref(png);
    g_ptr_array_add(t->pending_images, url);

    marker = g_strdup_printf("[image %u]",
                             (guint)t->pending_images->len);

    /* Comportement cohérent avec un collage : remplace la sélection,
     * puis insère un marqueur textuel à la position du curseur. */
    gtk_text_buffer_get_selection_bounds(buf, &start, &end);
    if (gtk_text_iter_compare(&start, &end) != 0)
        gtk_text_buffer_delete(buf, &start, &end);
    gtk_text_buffer_get_iter_at_mark(
        buf, &iter, gtk_text_buffer_get_insert(buf));
    if (gtk_text_iter_get_line_offset(&iter) > 0)
        gtk_text_buffer_insert(buf, &iter, "\n", -1);
    gtk_text_buffer_insert(buf, &iter, marker, -1);
    g_free(marker);
}

void
on_llm_entry_paste(GtkTextView *view, gpointer data)
{
    LlmTile          *t = data;
    GdkClipboard     *clip = gtk_widget_get_clipboard(GTK_WIDGET(view));
    GdkContentFormats *formats;

    formats = gdk_clipboard_get_formats(clip);
    if (formats == NULL ||
        !gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE))
        return;

    /* Image détectée : empêche le collage texte/binaire par défaut. */
    g_signal_stop_emission_by_name(view, "paste-clipboard");
    gdk_clipboard_read_texture_async(clip, NULL,
                                     (GAsyncReadyCallback)on_llm_clip_texture,
                                     t);
}

void
llm_pending_images_clear(LlmTile *t)
{
    if (t->pending_images != NULL)
        g_ptr_array_set_size(t->pending_images, 0);
}

GtkWindow *
tile_window(LlmTile *t)
{
    GtkRoot *root = gtk_widget_get_root(t->view);

    return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

void
on_digits_only_insert(GtkEditable *editable, const char *text, gint len,
                      gint G_GNUC_UNUSED *position, gpointer G_GNUC_UNUSED data)
{
    if (len < 0)
        len = (gint)strlen(text);
    for (gint i = 0; i < len; i++) {
        if (!g_ascii_isdigit(text[i])) {
            g_signal_stop_emission_by_name(editable, "insert-text");
            return;
        }
    }
}

void
on_num_pick_ok(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    NumPickCtx *ctx = data;
    GtkWidget  *entry = g_object_get_data(G_OBJECT(ctx->dialog), "entry");
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
    char       *end;
    long        v;

    v = strtol(txt, &end, 10);
    if (*end != '\0' || txt[0] == '\0' || v < 0 || v > 999)
        ctx->result = -1;
    else
        ctx->result = (int)v;
    gtk_window_destroy(GTK_WINDOW(ctx->dialog));
}

void
on_num_pick_cancel(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    NumPickCtx *ctx = data;

    ctx->result = -1;
    gtk_window_destroy(GTK_WINDOW(ctx->dialog));
}

void
on_num_pick_activate(GtkEntry G_GNUC_UNUSED *e, gpointer data)
{
    on_num_pick_ok(NULL, data);
}

int
num_pick_dialog(GtkWindow *parent, const char *title, const char *label)
{
    NumPickCtx ctx = { NULL, -1 };
    GtkWidget *win, *box, *lbl, *entry, *row, *cancel, *ok;
    GMainLoop *loop;

    win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 320, -1);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_append(GTK_BOX(box), lbl);

    entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry), 3);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "0");
    g_signal_connect(entry, "insert-text",
                     G_CALLBACK(on_digits_only_insert), NULL);
    gtk_box_append(GTK_BOX(box), entry);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("Annuler");
    ok = gtk_button_new_with_label("Valider");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_num_pick_cancel), &ctx);
    g_signal_connect(ok, "clicked", G_CALLBACK(on_num_pick_ok), &ctx);
    g_signal_connect(entry, "activate", G_CALLBACK(on_num_pick_activate), &ctx);
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(row), ok);
    gtk_box_append(GTK_BOX(box), row);

    g_object_set_data(G_OBJECT(win), "entry", entry);
    ctx.dialog = GTK_WINDOW(win);

    loop = g_main_loop_new(NULL, FALSE);
    g_signal_connect_swapped(win, "destroy", G_CALLBACK(g_main_loop_quit),
                             loop);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(entry);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return ctx.result;
}

void
on_confirm_yes(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    ConfirmCtx *ctx = data;

    ctx->ok = TRUE;
    gtk_window_destroy(ctx->dialog);
}

void
on_confirm_no(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    ConfirmCtx *ctx = data;

    gtk_window_destroy(ctx->dialog);
}

gboolean
confirm_dialog(GtkWindow *parent, const char *title, const char *msg,
               const char *ok_label, gboolean destructive)
{
    ConfirmCtx ctx = { NULL, FALSE };
    GtkWidget *win, *box, *lbl, *row, *cancel, *ok;
    GMainLoop *loop;

    win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 340, -1);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    lbl = gtk_label_new(msg);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_append(GTK_BOX(box), lbl);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("Annuler");
    ok = gtk_button_new_with_label(ok_label);
    gtk_widget_add_css_class(ok, destructive ? "destructive-action"
                                             : "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_confirm_no), &ctx);
    g_signal_connect(ok, "clicked", G_CALLBACK(on_confirm_yes), &ctx);
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(row), ok);
    gtk_box_append(GTK_BOX(box), row);

    ctx.dialog = GTK_WINDOW(win);

    loop = g_main_loop_new(NULL, FALSE);
    g_signal_connect_swapped(win, "destroy", G_CALLBACK(g_main_loop_quit),
                             loop);
    gtk_window_present(GTK_WINDOW(win));
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return ctx.ok;
}

void
on_view_copy_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    GtkWindow    *win = data;
    const char   *raw = g_object_get_data(G_OBJECT(win), "raw-json");

    if (raw == NULL)
        return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(win)), raw);
}

void
llm_slots_view(LlmTile *t)
{
    char          *raw = llm_body_build(t);
    char          *pretty;
    JsonParser    *parser;
    GError        *error = NULL;
    GtkWidget     *scroll, *text_view, *titlebar, *copy_btn;
    GtkWindow     *win;
    GtkTextBuffer *buf;

    parser = json_parser_new();
    if (json_parser_load_from_data(parser, raw, -1, &error)) {
        pretty = json_to_string(json_parser_get_root(parser), TRUE);
    } else {
        /* Ne devrait jamais arriver (sortie de json_to_string). */
        pretty = g_strdup_printf("/* JSON invalide : %s — brut : */\n%s",
                                 error->message, raw);
        g_clear_error(&error);
    }
    g_object_unref(parser);

    buf = gtk_text_buffer_new(NULL);
    gtk_text_buffer_set_text(buf, pretty, -1);
    g_free(pretty);
    text_view = gtk_text_view_new_with_buffer(buf);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_NONE);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text_view);
    gtk_widget_set_size_request(scroll, 640, 480);

    titlebar = gtk_header_bar_new();
    copy_btn = gtk_button_new_with_label("Copier");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), copy_btn);

    if (!modal_open(tile_window(t), t->modal_count, titlebar, scroll,
                    &win)) {
        g_object_ref_sink(scroll);
        g_object_unref(scroll);
        g_object_ref_sink(titlebar);
        g_object_unref(titlebar);
        g_free(raw);
        hist_cdb_announce(t,
            "limite de modales atteinte (4) : fermez-en une d'abord.");
        return;
    }
    gtk_window_set_title(win, "JSON — tel qu'envoyé");
    /* La copie porte sur le JSON BRUT ; libéré avec la fenêtre. */
    g_object_set_data_full(G_OBJECT(win), "raw-json", raw, g_free);
    g_signal_connect(copy_btn, "clicked",
                     G_CALLBACK(on_view_copy_clicked), win);
}

void
llm_slots_save_dialog(LlmTile *t)
{
    GtkWindow *parent = tile_window(t);
    int        slot;
    char      *body;
    char      *msg;

    slot = num_pick_dialog(parent, "Sauvegarder dans un slot",
                           "Numéro de slot (0-999) :");
    if (slot < 0)
        return;
    if (llm_slots_exists(slot)) {
        char *q = g_strdup_printf("Le slot %d existe déjà. L'écraser ?",
                                  slot);

        if (!confirm_dialog(parent, "Slot occupé", q, "Écraser", TRUE)) {
            g_free(q);
            return;
        }
        g_free(q);
    }
    body = llm_body_build(t);
    if (llm_slots_save(slot, body)) {
        t->slot_origin = slot;
        t->turns_since_ref = 0;
        t->ref_body_size = strlen(body);
        llm_slots_title_update(t);
        msg = g_strdup_printf("Slot %d sauvegardé.", slot);
        core_cdb_announce(t->core, msg);
    } else {
        msg = g_strdup_printf("Échec de la sauvegarde du slot %d.", slot);
        core_cdb_announce(t->core, msg);
    }
    g_free(msg);
    g_free(body);
}

void
llm_slots_load_dialog(LlmTile *t)
{
    GtkWindow  *parent = tile_window(t);
    int         slot;
    char       *json;
    char       *msg;
    JsonParser *parser;
    JsonObject *root;
    JsonArray  *msgs;
    GError     *error = NULL;
    GtkTextIter start, end;
    guint       n;

    if (t->busy) {
        hist_cdb_announce(t,
            "chargement impossible pendant une requête en cours.");
        return;
    }
    slot = num_pick_dialog(parent, "Charger un slot",
                           "Numéro de slot (0-999) :");
    if (slot < 0)
        return;
    json = llm_slots_load(slot);
    if (json == NULL) {
        msg = g_strdup_printf("Slot %d vide.", slot);
        core_cdb_announce(t->core, msg);
        g_free(msg);
        return;
    }
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, &error)) {
        msg = g_strdup_printf("Slot %d : JSON invalide (%s).", slot,
                              error->message);
        g_clear_error(&error);
        g_object_unref(parser);
        g_free(json);
        core_cdb_announce(t->core, msg);
        g_free(msg);
        return;
    }
    g_free(json);
    root = json_node_get_object(json_parser_get_root(parser));
    msgs = root != NULL ? json_object_get_array_member(root, "messages")
                        : NULL;
    if (msgs == NULL) {
        g_object_unref(parser);
        core_cdb_announce(t->core, "pas de tableau « messages » dans ce slot.");
        return;
    }

    /* --- Wipe complet : comme si le fil n'avait jamais existé. --- */
    llm_history_wipe(t);
    llm_pending_images_clear(t);
    llm_queues_purge(t);
    gtk_text_buffer_get_bounds(t->hist, &start, &end);
    gtk_text_buffer_delete(t->hist, &start, &end);
    md_thinking_reset(t->hist);
    g_string_truncate(t->core->reply, 0);
    t->rendered_len = 0;
    t->in_reasoning = FALSE;
    if (t->reply_mark != NULL) {
        gtk_text_buffer_get_end_iter(t->hist, &end);
        gtk_text_buffer_move_mark(t->hist, t->reply_mark, &end);
    }
    llm_entry_clear(t);
    t->core->cdb_retries = 0;

    /* --- Repeuplement + rendu du fil importé. --- */
    n = json_array_get_length(msgs);
    for (guint i = 0; i < n; i++) {
        JsonObject *m = json_array_get_object_element(msgs, i);
        const char *role = m != NULL
                           ? json_object_get_string_member(m, "role") : NULL;
        JsonNode *content_node =
            m != NULL && json_object_has_member(m, "content")
                ? json_object_get_member(m, "content") : NULL;
        GPtrArray *images = NULL;
        char      *content = NULL;
        GtkTextIter eit;

        if (content_node != NULL && JSON_NODE_HOLDS_VALUE(content_node)) {
            if (json_node_get_value_type(content_node) == G_TYPE_STRING)
                content = g_strdup(json_node_get_string(content_node));
        } else if (content_node != NULL && JSON_NODE_HOLDS_ARRAY(content_node)) {
            JsonArray *parts = json_node_get_array(content_node);
            GString    *text = g_string_new(NULL);

            images = g_ptr_array_new_with_free_func(g_free);
            for (guint k = 0; k < json_array_get_length(parts); k++) {
                JsonObject *part =
                    json_array_get_object_element(parts, k);
                const char *type = part != NULL &&
                                   json_object_has_member(part, "type")
                                   ? json_object_get_string_member(part, "type")
                                   : NULL;

                if (part == NULL)
                    continue;
                if (g_strcmp0(type, "text") == 0 &&
                    json_object_has_member(part, "text")) {
                    if (text->len > 0)
                        g_string_append(text, "\n");
                    g_string_append(
                        text, json_object_get_string_member(part, "text"));
                } else if (g_strcmp0(type, "image_url") == 0 &&
                           json_object_has_member(part, "image_url")) {
                    JsonNode *node =
                        json_object_get_member(part, "image_url");
                    JsonObject *obj;
                    const char *url = NULL;

                    if (JSON_NODE_HOLDS_VALUE(node) &&
                        json_node_get_value_type(node) == G_TYPE_STRING)
                        url = json_node_get_string(node);
                    else if (JSON_NODE_HOLDS_OBJECT(node)) {
                        obj = json_node_get_object(node);
                        if (json_object_has_member(obj, "url"))
                            url = json_object_get_string_member(obj, "url");
                    }
                    if (url != NULL && url[0] != '\0')
                        g_ptr_array_add(images, g_strdup(url));
                }
            }

            content = g_string_free(text, FALSE);
            if (images->len == 0) {
                g_ptr_array_unref(images);
                images = NULL;
            }
        }

        if (g_strcmp0(role, "assistant") == 0 &&
            json_object_has_member(m, "tool_calls")) {
            JsonArray   *ta = json_object_get_array_member(m, "tool_calls");
            GPtrArray   *calls = llm_tool_calls_new();
            LlmMsg       tm = { 0 };

            for (guint k = 0; k < json_array_get_length(ta); k++) {
                JsonObject  *to = json_array_get_object_element(ta, k);
                LlmToolCall *tc = g_new0(LlmToolCall, 1);

                if (json_object_has_member(to, "id"))
                    tc->id = g_strdup(
                        json_object_get_string_member(to, "id"));
                if (json_object_has_member(to, "name"))
                    tc->name = g_strdup(
                        json_object_get_string_member(to, "name"));
                if (json_object_has_member(to, "arguments"))
                    tc->arguments_json = g_strdup(
                        json_object_get_string_member(to, "arguments"));
                g_ptr_array_add(calls, tc);
            }

            memset(&tm, 0, sizeof(tm));
            tm.actor = LLMACTOR_LLM;
            tm.local = FALSE;
            tm.kind = LLM_MSG_ASSISTANT_TOOL_CALLS;
            tm.content = content;
            tm.tool_calls = calls;
            g_array_append_vals(t->core->history, &tm, 1);

            hist_render_actor_header(t, LLMACTOR_LLM);
            gtk_text_buffer_get_end_iter(t->hist, &eit);
            md_insert(t->hist, &eit, content != NULL ? content : "");
            for (guint k = 0; k < calls->len; k++) {
                LlmToolCall *tc = g_ptr_array_index(calls, k);
                char         *line = g_strdup_printf(
                    "\n〔tool〕 %s %s",
                    tc->name != NULL ? tc->name : "?",
                    tc->arguments_json != NULL
                        ? tc->arguments_json : "{}");

                hist_append(t, line);
                g_free(line);
            }
            hist_append(t, "\n");
            continue;
        }

        if (g_strcmp0(role, "tool") == 0) {
            const char *tool_call_id =
                json_object_has_member(m, "tool_call_id")
                    ? json_object_get_string_member(m, "tool_call_id") : NULL;
            LlmMsg rm = { 0 };
            const char *shown = content != NULL
                                    ? content : "(aucun contenu nouveau)";

            memset(&rm, 0, sizeof(rm));
            rm.actor = LLMACTOR_CDB;
            rm.local = FALSE;
            rm.kind = LLM_MSG_TOOL_RESULT;
            rm.content = content;
            rm.tool_call_id = g_strdup(tool_call_id);
            g_array_append_vals(t->core->history, &rm, 1);
            content = NULL; /* transféré au message */

            hist_render_actor_header(t, LLMACTOR_CDB);
            hist_ensure_voice_tags(t);
            gtk_text_buffer_get_end_iter(t->hist, &eit);
            gtk_text_buffer_insert_with_tags_by_name(
                t->hist, &eit, shown, -1, "voice-cdb", NULL);
            hist_append(t, "\n");
            continue;
        }

        if (content == NULL && images == NULL)
            continue;

        if (g_strcmp0(role, "system") == 0) {
            if (i == 0) {
                g_free(content);
                if (images != NULL)
                    g_ptr_array_unref(images);
                continue; /* persona : ré-injecté live à chaque envoi */
            }
            if (content != NULL && g_str_has_prefix(content, "[CDB] ")) {
                char *stripped = g_strdup(content + 6);

                g_free(content);
                content = stripped;
            }
            history_push_images(t, LLMACTOR_CDB, FALSE, content, NULL);
            hist_render_actor_header(t, LLMACTOR_CDB);
            hist_ensure_voice_tags(t);
            gtk_text_buffer_get_end_iter(t->hist, &eit);
            gtk_text_buffer_insert_with_tags_by_name(
                t->hist, &eit, content != NULL ? content : "", -1,
                "voice-cdb", NULL);
            hist_append(t, "\n");
        } else if (g_strcmp0(role, "assistant") == 0) {
            history_push_images(t, LLMACTOR_LLM, FALSE, content, NULL);
            hist_render_actor_header(t, LLMACTOR_LLM);
            gtk_text_buffer_get_end_iter(t->hist, &eit);
            md_insert(t->hist, &eit, content != NULL ? content : "");
            hist_append(t, "\n");
        } else {
            history_push_images(t, LLMACTOR_USER, FALSE, content, images);
            images = NULL; /* transférée à l'historique */
            hist_render_actor_header(t, LLMACTOR_USER);
            hist_append(t, content != NULL ? content : "");
            hist_append(t, "\n");
        }

        g_free(content);
        if (images != NULL)
            g_ptr_array_unref(images);
    }
    g_object_unref(parser);
    llm_scroll_to_end(t);
    llm_live_save(t->core);

    t->slot_origin = slot;
    t->turns_since_ref = 0;
    {
        char *b = llm_body_build(t);
        t->ref_body_size = (b != NULL) ? strlen(b) : 0;
        g_free(b);
    }
    llm_slots_title_update(t);

    msg = g_strdup_printf("Slot %d chargé — le fil a été remplacé.", slot);
    core_cdb_announce(t->core, msg);
    g_free(msg);
}

void
llm_slots_clear_dialog(LlmTile *t)
{
    GtkWindow *parent = tile_window(t);
    int        slot;
    char      *msg;

    slot = num_pick_dialog(parent, "Vider un slot",
                           "Numéro de slot (0-999) :");
    if (slot < 0)
        return;
    if (!llm_slots_exists(slot)) {
        msg = g_strdup_printf("Slot %d déjà vide.", slot);
        core_cdb_announce(t->core, msg);
        g_free(msg);
        return;
    }
    msg = g_strdup_printf("Vider le slot %d ?", slot);
    if (!confirm_dialog(parent, "Vider un slot", msg, "Vider", TRUE)) {
        g_free(msg);
        return;
    }
    g_free(msg);
    llm_slots_clear(slot);
    msg = g_strdup_printf("Slot %d vidé.", slot);
    core_cdb_announce(t->core, msg);
    g_free(msg);
}

int
entry_to_int(GtkWidget *entry, gboolean *ok)
{
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
    char       *end;
    long        v;

    *ok = FALSE;
    if (txt[0] == '\0')
        return -1;
    v = strtol(txt, &end, 10);
    if (*end != '\0' || v < 0 || v > 999)
        return -1;
    *ok = TRUE;
    return (int)v;
}

void
on_import_ok(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    ImportCtx *ctx = data;
    gboolean   a, b2, c;
    int        sess = entry_to_int(ctx->e_session, &a);
    int        src = entry_to_int(ctx->e_src, &b2);
    int        dst = entry_to_int(ctx->e_dst, &c);

    ctx->attempted = TRUE;
    if (!a || !b2 || !c) {
        gtk_label_set_text(GTK_LABEL(ctx->err),
                           "Trois nombres 0-999 attendus.");
        return;
    }
    if (!llm_slots_dir_exists(sess)) {
        char *m = g_strdup_printf("La session %03d n'a pas de slots.",
                                  sess);

        gtk_label_set_text(GTK_LABEL(ctx->err), m);
        g_free(m);
        return;
    }
    if (!llm_slots_exists_in(sess, src)) {
        char *m = g_strdup_printf("Slot %d vide dans la session %03d.",
                                  src, sess);

        gtk_label_set_text(GTK_LABEL(ctx->err), m);
        g_free(m);
        return;
    }
    if (llm_slots_exists(dst)) {
        char *q = g_strdup_printf("Le slot cible %d existe déjà. "
                                  "L'écraser ?", dst);

        if (!confirm_dialog(ctx->dialog, "Slot occupé", q, "Écraser",
                            TRUE)) {
            g_free(q);
            return;
        }
        g_free(q);
    }
    ctx->done = llm_slots_import(sess, src, dst);
    ctx->dst_slot = dst;
    gtk_window_destroy(ctx->dialog);
}

void
on_import_cancel(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    ImportCtx *ctx = data;

    gtk_window_destroy(ctx->dialog);
}

void
llm_slots_import_dialog(LlmTile *t)
{
    GtkWindow *parent = tile_window(t);
    ImportCtx  ctx = { NULL, NULL, NULL, NULL, NULL, FALSE, FALSE, -1 };
    GtkWidget *win, *box, *row, *cancel, *ok;
    GMainLoop *loop;

    win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Importer d'une session");
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 360, -1);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    {
        struct {
            const char *label;
            GtkWidget **out;
        } fields[] = {
            { "Session source (000-999) :", &ctx.e_session },
            { "Slot source (0-999) :",      &ctx.e_src },
            { "Slot cible (0-999) :",       &ctx.e_dst },
        };

        for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
            GtkWidget *lbl = gtk_label_new(fields[i].label);
            GtkWidget *e = gtk_entry_new();

            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_box_append(GTK_BOX(box), lbl);
            gtk_entry_set_max_length(GTK_ENTRY(e), 3);
            gtk_entry_set_placeholder_text(GTK_ENTRY(e), "0");
            g_signal_connect(e, "insert-text",
                             G_CALLBACK(on_digits_only_insert), NULL);
            gtk_box_append(GTK_BOX(box), e);
            *fields[i].out = e;
        }
    }

    ctx.err = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ctx.err), 0.0);
    gtk_label_set_wrap(GTK_LABEL(ctx.err), TRUE);
    gtk_widget_add_css_class(ctx.err, "error");
    gtk_box_append(GTK_BOX(box), ctx.err);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("Annuler");
    ok = gtk_button_new_with_label("Importer");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_import_cancel), &ctx);
    g_signal_connect(ok, "clicked", G_CALLBACK(on_import_ok), &ctx);
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(row), ok);
    gtk_box_append(GTK_BOX(box), row);

    ctx.dialog = GTK_WINDOW(win);

    loop = g_main_loop_new(NULL, FALSE);
    g_signal_connect_swapped(win, "destroy", G_CALLBACK(g_main_loop_quit),
                             loop);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(ctx.e_session);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    if (ctx.done) {
        char *msg = g_strdup_printf("Slot importé dans le slot %d.",
                                    ctx.dst_slot);

        core_cdb_announce(t->core, msg);
        g_free(msg);
    } else if (ctx.attempted) {
        core_cdb_announce(t->core, "échec de l'import.");
    }
}

void
llm_chat_clear_dialog(LlmTile *t)
{
    GtkWindow  *parent = tile_window(t);
    GtkTextIter start, end;

    if (t->busy) {
        hist_cdb_announce(t,
            "vidage impossible pendant une requête en cours.");
        return;
    }
    if (!confirm_dialog(parent, "Vider le chat actuel",
                        "Vider la conversation actuelle ?",
                        "Vider", TRUE))
        return;

    llm_history_wipe(t);
    llm_pending_images_clear(t);
    llm_queues_purge(t);
    gtk_text_buffer_get_bounds(t->hist, &start, &end);
    gtk_text_buffer_delete(t->hist, &start, &end);
    md_thinking_reset(t->hist);
    g_string_truncate(t->core->reply, 0);
    t->rendered_len = 0;
    t->in_reasoning = FALSE;
    if (t->reply_mark != NULL) {
        gtk_text_buffer_get_end_iter(t->hist, &end);
        gtk_text_buffer_move_mark(t->hist, t->reply_mark, &end);
    }
    llm_entry_clear(t);
    t->core->cdb_retries = 0;
    t->slot_origin = -1;
    t->turns_since_ref = 0;
    {
        char *b = llm_body_build(t);
        t->ref_body_size = (b != NULL) ? strlen(b) : 0;
        g_free(b);
    }
    llm_slots_title_update(t);

    core_cdb_announce(t->core, "chat actuel vidé.");
}

char *
llm_slots_size_str(gsize bytes)
{
    if (bytes < 1024)
        return g_strdup_printf("%zu o", bytes);
    if (bytes < 1024 * 1024)
        return g_strdup_printf("%.1f Ko", bytes / 1024.0);
    return g_strdup_printf("%.1f Mo", bytes / (1024.0 * 1024.0));
}

void
llm_slots_title_update(LlmTile *t)
{
    char  *body;
    char  *txt;
    char  *sizestr;
    gsize  cur_size;
    long   delta;

    if (t->slots_title == NULL)
        return;

    body = llm_body_build(t);
    cur_size = (body != NULL) ? strlen(body) : 0;
    g_free(body);

    delta = (long)cur_size - (long)t->ref_body_size;
    if (delta < 0)
        delta = 0;

    sizestr = llm_slots_size_str((gsize)delta);

    if (t->slot_origin < 0) {
        if (t->turns_since_ref == 0)
            txt = g_strdup("Non sauvegardé");
        else
            txt = g_strdup_printf("Non sauvegardé — +%s · %d tour%s",
                                  sizestr, t->turns_since_ref,
                                  t->turns_since_ref > 1 ? "s" : "");
    } else {
        if (t->turns_since_ref == 0)
            txt = g_strdup_printf("Slot %d — à jour", t->slot_origin);
        else
            txt = g_strdup_printf("Slot %d — +%s · %d tour%s",
                                  t->slot_origin, sizestr,
                                  t->turns_since_ref,
                                  t->turns_since_ref > 1 ? "s" : "");
    }

    gtk_label_set_text(GTK_LABEL(t->slots_title), txt);
    if (t->slots_btn != NULL)
        gtk_widget_set_tooltip_text(t->slots_btn, txt);
    g_free(txt);
    g_free(sizestr);
}

void
on_slots_pop_mapped(GtkWidget G_GNUC_UNUSED *w, gpointer data)
{
    llm_slots_title_update(data);
}

void
llm_slots_action_run(LlmTile *t, const char *name)
{
    if (g_strcmp0(name, "view") == 0)
        llm_slots_view(t);
    else if (g_strcmp0(name, "save") == 0)
        llm_slots_save_dialog(t);
    else if (g_strcmp0(name, "load") == 0)
        llm_slots_load_dialog(t);
    else if (g_strcmp0(name, "clear") == 0)
        llm_slots_clear_dialog(t);
    else if (g_strcmp0(name, "clear-chat") == 0)
        llm_chat_clear_dialog(t);
    else if (g_strcmp0(name, "import") == 0)
        llm_slots_import_dialog(t);
}

void
on_slots_menu_item_clicked(GtkButton *btn, gpointer data)
{
    LlmTile    *t = data;
    const char *name = g_object_get_data(G_OBJECT(btn), "slot-action");
    GtkWidget  *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn),
                                              GTK_TYPE_POPOVER);

    if (pop != NULL)
        gtk_popover_popdown(GTK_POPOVER(pop));

    llm_slots_action_run(t, name);
}

void
on_llm_send_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    LlmTile    *t = data;
    char       *prompt;

    /* Bouton média : pause pendant une requête = ANNULER. */
    if (t->busy) {
        llm_cancel_current(t);
        return;
    }
    t->busy = TRUE;
    prompt = llm_entry_text(t);
    if (prompt[0] == '\0' &&
        (t->pending_images == NULL || t->pending_images->len == 0)) {
        g_free(prompt);
        t->busy = FALSE;
        return;
    }

    /* Sans modèle actif, pas d'envoi : le menu de la tuile est le seul
     * endroit où l'on choisit un modèle (plus aucun repli implicite). */
    if (t->cfg == NULL || t->cfg->model == NULL ||
        t->cfg->model[0] == '\0') {
        hist_cdb_announce(t,
            "aucun modèle actif : choisissez-en un dans le menu de "
            "modèle (bouton « ? ») au-dessus de la saisie.");
        g_free(prompt);
        t->busy = FALSE;
        return;
    }

    for (guint vi = 0; vi < t->core->views->len; vi++) {
        LlmTile *v = g_ptr_array_index(t->core->views, vi);

        hist_render_actor_header(v, LLMACTOR_USER);
        hist_append(v, prompt);
    }
    /* llm_send ouvre lui-même le tour (llm_turn_new) : pas d'appel ici,
     * sinon l'en-tête « Claude » serait rendu deux fois. */
    llm_entry_clear(t);
    t->core->cdb_retries = 0; /* nouveau tour : compteur malformations reset */
    {
        GPtrArray *images = t->pending_images;

        t->pending_images = g_ptr_array_new_with_free_func(g_free);
        history_push_images(t, LLMACTOR_USER, FALSE, prompt, images);
        llm_live_save(t->core);
    }
    llm_send(t, prompt);
    llm_scroll_to_end(t);
    g_free(prompt); /* copie : l'entry a été vidée */
}

void
llm_scroll_to_end(LlmTile *t)
{
    double upper, page;

    if (!t->follow || t->adj == NULL)
        return;
    upper = gtk_adjustment_get_upper(t->adj);
    page = gtk_adjustment_get_page_size(t->adj);
    gtk_adjustment_set_value(t->adj, upper - page);
}

void
on_llm_scroll(GtkAdjustment *adj, gpointer data)
{
    LlmTile *t = data;
    double   val = gtk_adjustment_get_value(adj);
    double   upper = gtk_adjustment_get_upper(adj);
    double   page = gtk_adjustment_get_page_size(adj);

    t->follow = (val + page >= upper - 20.0);
}

gboolean
on_llm_hist_key(GtkEventControllerKey G_GNUC_UNUSED *ctrl,
                guint keyval,
                guint G_GNUC_UNUSED keycode,
                GdkModifierType state,
                gpointer data)
{
    LlmTile *t = data;
    double   upper, page;

    if (t == NULL || t->adj == NULL)
        return FALSE;

    /* Home/End simples ou Ctrl+Home/Ctrl+End. On ignore les autres
     * combinaisons modifiées pour laisser GTK gérer sélection/actions. */
    if ((state & GDK_SHIFT_MASK) != 0)
        return FALSE;
    if ((state & GDK_CONTROL_MASK) == 0 &&
        (state & ~GDK_CONTROL_MASK) != 0)
        return FALSE;

    switch (keyval) {
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
        gtk_adjustment_set_value(t->adj, 0.0);
        return TRUE;
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
        t->follow = TRUE;
        upper = gtk_adjustment_get_upper(t->adj);
        page = gtk_adjustment_get_page_size(t->adj);
        gtk_adjustment_set_value(t->adj, upper - page);
        return TRUE;
    default:
        return FALSE;
    }
}

void
on_scrollbar_pressed(GtkGestureClick G_GNUC_UNUSED *g,
                     int G_GNUC_UNUSED n,
                     double G_GNUC_UNUSED x, double G_GNUC_UNUSED y,
                     gpointer data)
{
    LlmTile *t = data;

    t->follow = FALSE;
}

void
on_llm_entry_changed(GtkTextBuffer G_GNUC_UNUSED *buf, gpointer data)
{
    llm_entry_resize(data);
}

/* Rejoue la conversation du core dans cette vue (attachement) :
 * chaque message avec son en-tête d'acteur, puis le fragment de
 * réponse en cours s'il existe. Appelée après création du buffer. */
static void
llm_tile_replay_history(LlmTile *t)
{
    LlmCore    *c = t->core;
    GtkTextIter eit;

    if (c == NULL)
        return;

    for (guint i = 0; i < c->history->len; i++) {
        LlmMsg *m = &g_array_index(c->history, LlmMsg, i);

        hist_render_actor_header(t, m->actor);
        gtk_text_buffer_get_end_iter(t->hist, &eit);

        if (m->kind == LLM_MSG_ASSISTANT_TOOL_CALLS) {
            md_insert(t->hist, &eit, m->content != NULL ? m->content : "");

            for (guint k = 0; m->tool_calls != NULL &&
                 k < m->tool_calls->len; k++) {
                LlmToolCall *tc = g_ptr_array_index(m->tool_calls, k);
                char         *line = g_strdup_printf(
                    "\n〔tool〕 %s %s",
                    tc->name != NULL ? tc->name : "?",
                    tc->arguments_json != NULL
                        ? tc->arguments_json : "{}");

                hist_append(t, line);
                g_free(line);
            }
        } else if (m->kind == LLM_MSG_TOOL_RESULT) {
            const char *shown = m->content != NULL
                                    ? m->content
                                    : "(aucun contenu nouveau)";

            gtk_text_buffer_insert_with_tags_by_name(
                t->hist, &eit, shown, -1, "voice-cdb", NULL);
        } else if (m->actor == LLMACTOR_LLM) {
            md_insert(t->hist, &eit, m->content != NULL ? m->content : "");
        } else if (m->actor == LLMACTOR_CDB) {
            gtk_text_buffer_insert_with_tags_by_name(
                t->hist, &eit, m->content != NULL ? m->content : "", -1,
                "voice-cdb", NULL);
        } else {
            gtk_text_buffer_insert(t->hist, &eit,
                                   m->content != NULL ? m->content : "", -1);
        }
        hist_append(t, "\n");
    }

    if (c->reply->len > 0) {
        /* Stream en cours : rattrape le fragment (rendered_len a zero). */
        hist_render_actor_header(t, LLMACTOR_LLM);
        t->rendered_len = 0;
        hist_flush_reply(t);
    }

    gtk_text_buffer_get_end_iter(t->hist, &eit);
    if (t->reply_mark == NULL)
        t->reply_mark = gtk_text_buffer_create_mark(t->hist, NULL,
                                                    &eit, TRUE);
    else
        gtk_text_buffer_move_mark(t->hist, t->reply_mark, &eit);

    llm_tile_decision_render(t);
    llm_scroll_to_end(t);
}


GtkWidget *
llm_tile_new(LlmCore *core, const LlmConfig *cfg, GActionGroup *actions,
             GListStore *roots, GHashTable *multi_paths,
             int *modal_count)
{
    GtkWidget *box;
    GtkWidget *scroll;
    LlmTile   *t;

    if (cfg == NULL) {
        /* Pas de config : aide au lieu du chat. */
        GtkWidget *lbl = gtk_label_new(
            "LLM non configuré.\n\n"
            "Settings → LLM → Providers : renseignez un provider\n"
            "(clé API), puis choisissez un modèle dans le menu\n"
            "ci-dessous.");

        gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
        return lbl;
    }

    t = g_new0(LlmTile, 1);
    t->core = core;
    /* Attachement : le core diffuse vers toutes les vues listées. */
    g_ptr_array_add(core->views, t);
    t->cfg = (LlmConfig *)cfg;
    t->actions = actions != NULL ? g_object_ref(actions) : NULL;
    t->modal_count = modal_count; /* emprunté à App */
    /* Projet courant pour [PROJET]/[CHEMIN] : mêmes références que
     * BashPanel (possédées par App, vivent plus longtemps que la tuile). */
    t->roots = roots;
    t->multi_paths = multi_paths;

    t->hist = gtk_text_buffer_new(NULL);
    t->view = gtk_text_view_new_with_buffer(t->hist);
    t->hist_view = t->view;
    md_thinking_attach(t->hist, t->hist_view);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(t->view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(t->view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(t->view), FALSE);
    llm_tile_replay_history(t);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), t->view);
    /* Suivi auto tant que le user reste en bas de la vue. L'adjustment
     * est gardé : llm_scroll_to_end pilote la position mécaniquement. */
    t->follow = TRUE;
    t->adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    g_signal_connect(t->adj, "value-changed",
                     G_CALLBACK(on_llm_scroll), t);

    /* Home/End : scroll explicite de l'historique. */
    {
        GtkEventController *key = gtk_event_controller_key_new();

        gtk_event_controller_set_propagation_phase(
            key, GTK_PHASE_CAPTURE);
        g_signal_connect(key, "key-pressed",
                         G_CALLBACK(on_llm_hist_key), t);
        gtk_widget_add_controller(t->hist_view, key);
    }
    /* Toute pression sur la scrollbar détache le suivi immédiatement. */
    {
        GtkWidget         *sb =
            gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(scroll));
        GtkGesture *gc = gtk_gesture_click_new();

        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), 0);
        g_signal_connect(gc, "pressed",
                         G_CALLBACK(on_scrollbar_pressed), t);
        gtk_widget_add_controller(sb, GTK_EVENT_CONTROLLER(gc));
    }

    /* Saisie multi-lignes : GtkTextView dans un scrolled-window (1 ligne
     * au repos, jusqu'à 8, puis scroll interne). Entrée = saut de ligne ;
     * l'envoi passe par le bouton de la rangée outils. */
    t->entry_buf = gtk_text_buffer_new(NULL);
    t->entry = gtk_text_view_new_with_buffer(t->entry_buf);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(t->entry), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(t->entry), FALSE);
    gtk_widget_add_css_class(t->entry, "llm-compose-entry");
    g_signal_connect(t->entry_buf, "changed",
                     G_CALLBACK(on_llm_entry_changed), t);
    g_signal_connect(t->entry, "paste-clipboard",
                     G_CALLBACK(on_llm_entry_paste), t);

    t->entry_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(t->entry_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(t->entry_scroll),
                                  t->entry);
    llm_entry_resize(t);

    t->send_btn = gtk_button_new_from_icon_name(
        "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(t->send_btn, "Envoyer");
    gtk_widget_add_css_class(t->send_btn, "flat");
    gtk_widget_add_css_class(t->send_btn, "cdb-flat");
    gtk_widget_add_css_class(t->send_btn, "llm-compose-send");
    gtk_widget_set_valign(t->send_btn, GTK_ALIGN_CENTER);
    g_signal_connect(t->send_btn, "clicked",
                     G_CALLBACK(on_llm_send_clicked), t);

    /* Bouton persistance (slots JSON) : popover custom à 6 actions,
     * même langage visuel que le sélecteur de modèle. */
    {
        GtkWidget *slots_pop;
        GtkWidget *slots_box;
        static const struct {
            const char *label;
            const char *action;
        } items[] = {
            { "Voir le JSON envoyé…",      "view" },
            { "Sauvegarder dans un slot…", "save" },
            { "Charger un slot…",          "load" },
            { "Vider un slot…",            "clear" },
            { "Vider le chat actuel…",     "clear-chat" },
            { "Importer d'une session…",   "import" },
        };

        slots_pop = gtk_popover_new();
        gtk_popover_set_has_arrow(GTK_POPOVER(slots_pop), FALSE);
        gtk_widget_add_css_class(slots_pop, "cdb-pop");

        slots_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        /* Titre d'état du slot. */
        t->slots_title = gtk_label_new("Non sauvegardé");
        gtk_label_set_xalign(GTK_LABEL(t->slots_title), 0.0);
        gtk_widget_add_css_class(t->slots_title, "cdb-pop-title");
        gtk_box_append(GTK_BOX(slots_box), t->slots_title);

        for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
            GtkWidget *lbl = gtk_label_new(items[i].label);
            GtkWidget *b = gtk_button_new();

            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_button_set_child(GTK_BUTTON(b), lbl);
            gtk_widget_add_css_class(b, "flat");
            gtk_widget_add_css_class(b, "cdb-pop-item");
            gtk_widget_set_hexpand(b, TRUE);
            gtk_widget_set_halign(b, GTK_ALIGN_FILL);
            g_object_set_data(G_OBJECT(b), "slot-action",
                              (gpointer)items[i].action);
            g_signal_connect(b, "clicked",
                             G_CALLBACK(on_slots_menu_item_clicked), t);
            gtk_box_append(GTK_BOX(slots_box), b);
        }

        gtk_popover_set_child(GTK_POPOVER(slots_pop), slots_box);
        gtk_widget_set_size_request(slots_pop, 300, -1);
        g_signal_connect(slots_pop, "map",
                         G_CALLBACK(on_slots_pop_mapped), t);

        t->slots_btn = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(t->slots_btn),
                                      "folder-templates-symbolic");
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(t->slots_btn), slots_pop);
        gtk_widget_set_tooltip_text(t->slots_btn,
                                    gtk_label_get_text(GTK_LABEL(t->slots_title)));
        gtk_widget_add_css_class(t->slots_btn, "flat");
        gtk_widget_add_css_class(t->slots_btn, "cdb-flat");
        gtk_widget_set_valign(t->slots_btn, GTK_ALIGN_CENTER);
    }

    {
        GtkWidget *model_pop;
        GtkWidget *model_scroll;
        GtkWidget *model_outer;
        GtkWidget *configure;

        /* Sélecteur multi-provider : recherche + sections + Configurer.
         * Peuplement AU DÉMARRAGE de la tuile (pas au premier map du
         * popover) : les noms longs models.dev arrivent en tâche de fond
         * et le label phrasique est complet sans ouvrir le menu. */
        t->sections = g_ptr_array_new_with_free_func(model_section_free);

        t->model_search = gtk_search_entry_new();
        gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(t->model_search),
                                              "Sélectionner un modèle…");
        g_signal_connect(t->model_search, "search-changed",
                         G_CALLBACK(on_llm_model_search_changed), t);

        t->rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        model_scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(model_scroll),
                                       GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_min_content_height(
            GTK_SCROLLED_WINDOW(model_scroll), 48);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(model_scroll), 380);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(model_scroll), TRUE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(model_scroll),
                                      t->rows_box);

        model_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_append(GTK_BOX(model_outer), t->model_search);
        gtk_box_append(GTK_BOX(model_outer), model_scroll);
        configure = gtk_button_new_with_label("Configurer…");
        gtk_widget_add_css_class(configure, "flat");
        gtk_widget_add_css_class(configure, "llm-configure");
        gtk_widget_set_halign(configure, GTK_ALIGN_FILL);
        g_signal_connect(configure, "clicked",
                         G_CALLBACK(on_llm_configure_clicked), t);
        gtk_box_append(GTK_BOX(model_outer), configure);

        model_pop = gtk_popover_new();
        /* Pas de flèche grise vers le bouton : GtkMenuButton ne la
         * désactive que pour ses popovers internes, pas le nôtre. */
        gtk_popover_set_has_arrow(GTK_POPOVER(model_pop), FALSE);
        gtk_popover_set_child(GTK_POPOVER(model_pop), model_outer);
        gtk_widget_add_css_class(model_pop, "cdb-pop");
        g_signal_connect(model_pop, "map",
                         G_CALLBACK(llm_model_pop_mapped), t);
        t->model_pop = model_pop;

        t->model_btn = gtk_menu_button_new();
        gtk_widget_add_css_class(t->model_btn, "flat");
        gtk_widget_add_css_class(t->model_btn, "llm-model-btn");
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(t->model_btn),
                                    model_pop);
        /* Child custom : phrase + chevron dynamique (▾/▴). Remplace le
         * label intégré ET l'icône flèche du GtkMenuButton — plus de
         * triangle gris, jamais bien centré. */
        {
            GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            GtkWidget *phrase = gtk_label_new(NULL);

            t->chevron = gtk_image_new_from_icon_name("pan-down-symbolic");
            gtk_menu_button_set_child(GTK_MENU_BUTTON(t->model_btn), btn_box);
            /* Le label phrase est mis à jour par llm_model_button_refresh
             * via le label intégré… qui n'existe plus en mode child : on
             * le remplace par une réf directe. */
            t->model_phrase = phrase;
            gtk_box_append(GTK_BOX(btn_box), phrase);
            gtk_box_append(GTK_BOX(btn_box), t->chevron);
        }
        /* Largeur minimale : phrase courte (« ? ») → popover quand même
         * lisible ; la synchro à l'ouverture l'aligne sur le bouton. */
        gtk_widget_set_size_request(model_pop, 320, -1);
        llm_model_button_refresh(t);
        /* Fetch immédiat des modèles : noms longs disponibles sans
         * ouvrir le popover (le menu reste paresseux à l'affichage). */
        llm_model_menu_ensure(t);
        g_signal_connect(model_pop, "map",
                         G_CALLBACK(llm_model_chevron_update), t);
        g_signal_connect(model_pop, "unmap",
                         G_CALLBACK(llm_model_chevron_update), t);

        /* Barre de composition : bloc plein légèrement plus sombre que
         * la tuile (classe .llm-compose), deux rangées — saisie au-dessus,
         * outils en dessous (modèle à gauche, envoi à droite). */
        {
            GtkWidget *compose = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

            t->compose = compose;
            gtk_widget_add_css_class(compose, "llm-compose");

            /* Activité / bilan façon ZED. */
            {
                GtkWidget *up_icon;
                GtkWidget *down_icon;
                GtkWidget *context_icon;

                t->status_rev = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                t->status_logo = gtk_label_new(
                    LLM_STATUS_FRAMES[0]);
                t->status_label = gtk_label_new("");
                t->status_sent_label = gtk_label_new("");
                t->status_received_label = gtk_label_new("");
                t->status_context_label = gtk_label_new("");

                gtk_widget_add_css_class(t->status_rev, "llm-status");
                gtk_widget_set_halign(t->status_label, GTK_ALIGN_START);
                gtk_widget_set_size_request(t->status_logo, 16, 16);
                gtk_widget_set_valign(t->status_logo, GTK_ALIGN_CENTER);
                gtk_widget_add_css_class(t->status_logo, "llm-status-logo");
                gtk_widget_set_valign(t->status_label, GTK_ALIGN_CENTER);
                gtk_widget_set_valign(t->status_sent_label, GTK_ALIGN_CENTER);
                gtk_widget_set_valign(t->status_received_label,
                                      GTK_ALIGN_CENTER);
                gtk_widget_set_valign(t->status_context_label,
                                      GTK_ALIGN_CENTER);
                gtk_label_set_xalign(GTK_LABEL(t->status_context_label),
                                     GTK_ALIGN_START);
                gtk_widget_set_visible(t->status_rev, FALSE);

                up_icon = gtk_image_new_from_icon_name("pan-up-symbolic");
                down_icon = gtk_image_new_from_icon_name("pan-down-symbolic");
                context_icon = gtk_image_new_from_icon_name(
                    "go-first-symbolic-rtl");
                gtk_image_set_pixel_size(GTK_IMAGE(up_icon), 12);
                gtk_image_set_pixel_size(GTK_IMAGE(down_icon), 12);
                gtk_image_set_pixel_size(GTK_IMAGE(context_icon), 12);
                gtk_widget_set_valign(up_icon, GTK_ALIGN_CENTER);
                gtk_widget_set_valign(down_icon, GTK_ALIGN_CENTER);
                gtk_widget_set_valign(context_icon, GTK_ALIGN_CENTER);

                gtk_box_append(GTK_BOX(t->status_rev), t->status_logo);
                gtk_box_append(GTK_BOX(t->status_rev), t->status_label);
                gtk_box_append(GTK_BOX(t->status_rev), up_icon);
                gtk_box_append(GTK_BOX(t->status_rev), t->status_sent_label);
                gtk_box_append(GTK_BOX(t->status_rev), down_icon);
                gtk_box_append(GTK_BOX(t->status_rev), t->status_received_label);
                gtk_box_append(GTK_BOX(t->status_rev), context_icon);
                gtk_box_append(GTK_BOX(t->status_rev), t->status_context_label);
                gtk_box_append(GTK_BOX(compose), t->status_rev);
            }

            gtk_box_append(GTK_BOX(compose), t->entry_scroll);
            gtk_widget_set_hexpand(t->model_btn, FALSE);
            gtk_box_append(GTK_BOX(tools), t->model_btn);
            /* Ressort : le bouton d'envoi collé à droite. */
            {
                GtkWidget *spring = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

                gtk_widget_set_hexpand(spring, TRUE);
                gtk_box_append(GTK_BOX(tools), spring);
            }
            /* Persistance juste à gauche du play/pause. */
            gtk_box_append(GTK_BOX(tools), t->slots_btn);
            gtk_box_append(GTK_BOX(tools), t->send_btn);
            gtk_widget_set_margin_start(tools, 6);
            gtk_widget_set_margin_end(tools, 6);
            gtk_widget_set_margin_bottom(tools, 6);
            gtk_box_append(GTK_BOX(compose), tools);

            gtk_widget_set_margin_start(compose, 6);
            gtk_widget_set_margin_end(compose, 6);
            gtk_widget_set_margin_top(compose, 6);
            gtk_widget_set_margin_bottom(compose, 6);

            box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_box_append(GTK_BOX(box), scroll);
            gtk_widget_set_vexpand(scroll, TRUE);
            gtk_box_append(GTK_BOX(box), compose);
        }
    }

    /* Anti-hang : pas de données pendant 120 s = abandon. */
    g_object_set(t->core->soup, "timeout", 120, "idle-timeout", 180, NULL);
    t->pending_images = g_ptr_array_new_with_free_func(g_free);
    t->slot_origin = -1;
    g_object_set_data(G_OBJECT(box), "cdb-llm-core", core);
    g_object_set_data_full(G_OBJECT(box), "cdb-llm-tile", t, llm_tile_free);
    return box;
}

/* ----- Décision /CDB:: : rendu par vue (l'état vit au core) ----- */

void
llm_tile_decision_render(LlmTile *t)
{
    LlmCore     *c = t->core;
    CdbDecision *d;
    GtkTextIter  end;
    GtkTextChildAnchor *anch;
    GtkWidget   *hbar, *b_ok, *b_no;

    if (c == NULL || c->decision == NULL)
        return;
    if (t->shown_decision == c->decision)
        return; /* déjà rendue dans cette vue */

    d = c->decision;
    {
        gchar *cmd_line = g_strdup_printf("bash-%d $ %s\n",
                                          d->tab, d->cmd != NULL
                                              ? d->cmd : "(cmd NULL)");

        hist_render_actor_header(t, LLMACTOR_CDB);
        hist_ensure_voice_tags(t);
        gtk_text_buffer_get_end_iter(t->hist, &end);
        gtk_text_buffer_insert_with_tags_by_name(t->hist, &end,
                                                 cmd_line, -1,
                                                 "voice-cdb", NULL);
        g_free(cmd_line);
    }

    t->shown_decision = d;

    gtk_text_buffer_get_end_iter(t->hist, &end);
    anch = gtk_text_buffer_create_child_anchor(t->hist, &end);

    hbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b_ok = gtk_button_new_with_label("Exécuter");
    b_no = gtk_button_new_with_label("Refuser");
    t->approval_bar = hbar;
    t->approval_ok = b_ok;
    t->approval_no = b_no;
    gtk_widget_add_css_class(b_ok, "flat");
    gtk_widget_add_css_class(b_no, "flat");
    gtk_widget_set_focusable(b_ok, FALSE);
    gtk_widget_set_focusable(b_no, FALSE);
    g_object_set_data(G_OBJECT(b_ok), "decision", d);
    g_object_set_data(G_OBJECT(b_no), "decision", d);
    g_signal_connect(b_ok, "clicked",
                     G_CALLBACK(on_cdb_approve_clicked), d);
    g_signal_connect(b_no, "clicked",
                     G_CALLBACK(on_cdb_refuse_clicked), d);
    gtk_box_append(GTK_BOX(hbar), b_ok);
    gtk_box_append(GTK_BOX(hbar), b_no);
    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(t->hist_view),
                                      hbar, anch);
    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_insert(t->hist, &end, "\n", -1);
}

void
llm_tile_decision_lock(LlmTile *t)
{
    if (t->approval_bar == NULL)
        return;
    gtk_widget_set_sensitive(t->approval_ok, FALSE);
    gtk_widget_set_sensitive(t->approval_no, FALSE);
}

void
llm_cdb_say_display(LlmTile *t, const char *text)
{
    GtkTextIter end;

    hist_render_actor_header(t, LLMACTOR_CDB);
    hist_ensure_voice_tags(t);
    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_insert_with_tags_by_name(t->hist, &end, text, -1,
                                             "voice-cdb", NULL);
}
