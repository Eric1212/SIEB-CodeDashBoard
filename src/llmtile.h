#ifndef CDB_LLMTILE_H
#define CDB_LLMTILE_H

/* Prototypes generes - split C0 */

void
llm_tile_free(gpointer data)
;

void
model_section_free(gpointer data)
;

gboolean
llm_model_matches(const char *query, const char *id)
;

void
llm_model_button_refresh(LlmTile *t)
;

void
llm_model_section_refresh(LlmTile *t, ModelSection *sec)
;

void
on_llm_model_row_activated(GtkListBox G_GNUC_UNUSED *lb,
                           GtkListBoxRow *row, gpointer data)
;

void
llm_model_menu_apply_filter(LlmTile *t)
;

void
on_llm_model_search_changed(GtkSearchEntry G_GNUC_UNUSED *entry,
                            gpointer data)
;

void
on_llm_configure_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
;

void
on_section_models_fetched(LlmModelInfo *models, gpointer data)
;

void
llm_model_pop_mapped(GtkWidget G_GNUC_UNUSED *w, gpointer data)
;

void
llm_model_pop_width_sync(LlmTile *t)
;

void
llm_model_chevron_update(GtkWidget *popover, gpointer data)
;

void
llm_model_menu_ensure(LlmTile *t)
;

char *
llm_persona_load(LlmTile *t)
;

void
hist_ensure_voice_tags(LlmTile *t)
;

void
hist_render_actor_header(LlmTile *t, LlmActor actor)
;

void
hist_cdb_announce(LlmTile *t, const char *text)
;

void
hist_append(LlmTile *t, const char *text)
;

void
hist_update_reply(LlmTile *t)
;

void
hist_flush_reply(LlmTile *t)
;

void
llm_busy_set(LlmTile *t, gboolean busy)
;

gboolean
llm_status_tick(gpointer data)
;

void
llm_status_start(LlmTile *t)
;

void
llm_status_stop(LlmTile *t)
;

void
llm_status_update(LlmTile *t)
;

void
cdb_approval_destroy(GtkWidget G_GNUC_UNUSED *w, gpointer data)
;

LlmTile *
cdb_tile_from_button(GtkButton *btn)
;

void
on_cdb_refuse_clicked(GtkButton *btn, gpointer data)
;

void
on_cdb_approve_clicked(GtkButton *btn, gpointer data)
;

void
hist_cdb_say(LlmTile *t, const char *text)
;

void
llm_cdb_ask(LlmTile *t, int tab, const char *cmd)
;

void
llm_turn_new(LlmTile *t)
;

char *
llm_entry_text(LlmTile *t)
;

void
llm_entry_clear(LlmTile *t)
;

void
llm_entry_resize(LlmTile *t)
;

void
on_llm_clip_texture(GObject *source_object,
                    GAsyncResult *res,
                    gpointer data)
;

void
on_llm_entry_paste(GtkTextView *view, gpointer data)
;

void
llm_pending_images_clear(LlmTile *t)
;

GtkWindow *
tile_window(LlmTile *t)
;

void
on_digits_only_insert(GtkEditable *editable, const char *text, gint len,
                      gint G_GNUC_UNUSED *position, gpointer G_GNUC_UNUSED data)
;

void
on_num_pick_ok(GtkButton G_GNUC_UNUSED *b, gpointer data)
;

void
on_num_pick_cancel(GtkButton G_GNUC_UNUSED *b, gpointer data)
;

void
on_num_pick_activate(GtkEntry G_GNUC_UNUSED *e, gpointer data)
;

int
num_pick_dialog(GtkWindow *parent, const char *title, const char *label)
;

void
on_confirm_yes(GtkButton G_GNUC_UNUSED *b, gpointer data)
;

void
on_confirm_no(GtkButton G_GNUC_UNUSED *b, gpointer data)
;

gboolean
confirm_dialog(GtkWindow *parent, const char *title, const char *msg,
               const char *ok_label, gboolean destructive)
;

void
on_view_copy_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
;

void
llm_slots_view(LlmTile *t)
;

void
llm_slots_save_dialog(LlmTile *t)
;

void
llm_slots_load_dialog(LlmTile *t)
;

void
llm_slots_clear_dialog(LlmTile *t)
;

int
entry_to_int(GtkWidget *entry, gboolean *ok)
;

void
on_import_ok(GtkButton G_GNUC_UNUSED *b, gpointer data)
;

void
on_import_cancel(GtkButton G_GNUC_UNUSED *b, gpointer data)
;

void
llm_slots_import_dialog(LlmTile *t)
;

void
llm_chat_clear_dialog(LlmTile *t)
;

char *
llm_slots_size_str(gsize bytes)
;

void
llm_slots_title_update(LlmTile *t)
;

void
on_slots_pop_mapped(GtkWidget G_GNUC_UNUSED *w, gpointer data)
;

void
llm_slots_action_run(LlmTile *t, const char *name)
;

void
on_slots_menu_item_clicked(GtkButton *btn, gpointer data)
;

void
on_llm_send_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
;

void
llm_scroll_to_end(LlmTile *t)
;

void
on_llm_scroll(GtkAdjustment *adj, gpointer data)
;

gboolean
on_llm_hist_key(GtkEventControllerKey G_GNUC_UNUSED *ctrl,
                guint keyval,
                guint G_GNUC_UNUSED keycode,
                GdkModifierType state,
                gpointer data)
;

void
on_scrollbar_pressed(GtkGestureClick G_GNUC_UNUSED *g,
                     int G_GNUC_UNUSED n,
                     double G_GNUC_UNUSED x, double G_GNUC_UNUSED y,
                     gpointer data)
;

void
on_llm_entry_changed(GtkTextBuffer G_GNUC_UNUSED *buf, gpointer data)
;

GtkWidget *
llm_tile_new(const LlmConfig *cfg, GActionGroup *actions,
             GListStore *roots, GHashTable *multi_paths,
             int *modal_count)
;

#endif
