#ifndef CDB_LLMTILE_H
#define CDB_LLMTILE_H

/* Prototypes generes - split C0 */

void
llm_tile_free(gpointer data);

void
model_section_free(gpointer data);

gboolean
llm_model_matches(const char *query, const char *id);

void
llm_model_button_refresh(LlmTile *t);

void
llm_model_section_refresh(LlmTile *t, ModelSection *sec);

void
on_llm_model_row_activated(GtkListBox G_GNUC_UNUSED *lb,
                           GtkListBoxRow *row, gpointer data)
;

void
llm_model_menu_apply_filter(LlmTile *t);

void
on_llm_model_search_changed(GtkSearchEntry G_GNUC_UNUSED *entry,
                            gpointer data)
;

void
on_llm_configure_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data);

void
on_section_models_fetched(LlmModelInfo *models, gpointer data);

void
llm_model_pop_mapped(GtkWidget G_GNUC_UNUSED *w, gpointer data);

void
llm_model_pop_width_sync(LlmTile *t);

void
llm_model_chevron_update(GtkWidget *popover, gpointer data);

void
llm_model_menu_ensure(LlmTile *t);

/* Repose le latch du menu : llm.json a changé, la liste des fournisseurs et
 * leurs modèles est périmée. Ne reconstruit rien — ensure() le fera au
 * prochain ouvert du popover, et il sait déjà le faire. */
void
llm_model_menu_invalidate(LlmTile *t);

/* llm_model_menu_invalidate sur TOUTES les vues du core (loi du miroir). */
void
llm_views_config_changed(LlmCore *core);

/* Les noms des acteurs ont changé (Settings → Noms) : chaque vue vide et
 * rejoue l'historique pour que les en-têtes « — <nom> — » déjà rendus
 * suivent le renommage (loi du miroir). */
void
llm_views_names_changed(LlmCore *core);
char *
llm_persona_load(LlmTile *t);

void
hist_ensure_voice_tags(LlmTile *t);

void
hist_render_actor_header(LlmTile *t, LlmActor actor);

void
hist_cdb_announce(LlmTile *t, const char *text);

void
hist_append(LlmTile *t, const char *text);

void
hist_update_reply(LlmTile *t);

void
hist_flush_reply(LlmTile *t);

void
llm_busy_set(LlmTile *t, gboolean busy);

gboolean
llm_status_tick(gpointer data);

void
llm_status_start(LlmTile *t);

void
llm_status_stop(LlmTile *t);

void
llm_status_update(LlmTile *t);

void
cdb_approval_destroy(GtkWidget G_GNUC_UNUSED *w, gpointer data);

LlmTile *
cdb_tile_from_button(GtkButton *btn);

void
hist_cdb_say(LlmTile *t, const char *text);

void
llm_cdb_ask(LlmTile *t, int tab, const char *cmd);

void
llm_turn_new(LlmTile *t);

char *
llm_entry_text(LlmTile *t);

void
llm_entry_clear(LlmTile *t);

void
llm_entry_resize(LlmTile *t);

void
on_llm_clip_texture(GObject *source_object,
                    GAsyncResult *res,
                    gpointer data)
;

void
on_llm_entry_paste(GtkTextView *view, gpointer data);

void
llm_pending_images_clear(LlmTile *t);

GtkWindow *
tile_window(LlmTile *t);

void
on_digits_only_insert(GtkEditable *editable, const char *text, gint len,
                      gint G_GNUC_UNUSED *position, gpointer G_GNUC_UNUSED data)
;

void
on_num_pick_ok(GtkButton G_GNUC_UNUSED *b, gpointer data);

void
on_num_pick_cancel(GtkButton G_GNUC_UNUSED *b, gpointer data);

void
on_num_pick_activate(GtkEntry G_GNUC_UNUSED *e, gpointer data);

int
num_pick_dialog(GtkWindow *parent, const char *title, const char *label);

void
on_confirm_yes(GtkButton G_GNUC_UNUSED *b, gpointer data);

void
on_confirm_no(GtkButton G_GNUC_UNUSED *b, gpointer data);

gboolean
confirm_dialog(GtkWindow *parent, const char *title, const char *msg,
               const char *ok_label, gboolean destructive)
;

void
on_view_copy_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data);

void
llm_slots_view(LlmTile *t);

void
llm_slots_save_dialog(LlmTile *t);

void
llm_slots_load_dialog(LlmTile *t);

void
llm_slots_clear_dialog(LlmTile *t);

int
entry_to_int(GtkWidget *entry, gboolean *ok);

void
on_import_ok(GtkButton G_GNUC_UNUSED *b, gpointer data);

void
on_import_cancel(GtkButton G_GNUC_UNUSED *b, gpointer data);

void
llm_slots_import_dialog(LlmTile *t);

void
llm_chat_clear_dialog(LlmTile *t);

char *
llm_slots_size_str(gsize bytes);

void
llm_slots_title_update(LlmTile *t);

void
on_slots_pop_mapped(GtkWidget G_GNUC_UNUSED *w, gpointer data);

void
llm_slots_action_run(LlmTile *t, const char *name);

void
on_slots_menu_item_clicked(GtkButton *btn, gpointer data);

void
on_llm_send_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data);

void
llm_scroll_to_end(LlmTile *t);

void
on_llm_scroll(GtkAdjustment *adj, gpointer data);

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
on_llm_entry_changed(GtkTextBuffer G_GNUC_UNUSED *buf, gpointer data);

GtkWidget *
llm_tile_new(LlmCore *core, const LlmConfig *cfg, GActionGroup *actions,
             GListStore *roots, GHashTable *multi_paths,
             int *modal_count)
;

void llm_tile_decision_render(LlmTile *t);

/* Étiquette/infobulle du bouton profil (refresh après changement). */
void llm_tile_profile_refresh(LlmTile *t);
/* Icône/teinte/infobulle du bouton effort (refresh après changement).
 * Trois teintes : Default grise, None rouge (error), les autres bleues
 * (accent) — classes libadwaita, aucune couleur codée. */
void llm_tile_effort_refresh(LlmTile *t);
/* Label/infobulle/titre du bouton trim (refresh après changement du
 * choix global — le core diffuse sur toutes les vues). */
void llm_tile_trim_refresh(LlmTile *t);
/* Résond la boîte portant CET ID à l'état de la décision (vert/rouge).
 * Appelé sur TOUTES les vues par le core : la décision vit au core, chaque
 * vue n'en possède que le rendu. L'ID est indispensable depuis qu'un tour
 * peut laisser plusieurs boîtes ouvertes. Idempotent. */
void llm_tile_decision_resolve(LlmTile *t, const char *tool_call_id,
                               CdbApprovalState state);

/* Ouvre la boîte d'une demande ACCEPTÉE D'AVANCE (outil en ALLOW ou
 * ALLOW+). Le core l'appelle sur chaque vue avant d'exécuter : la demande
 * ne passe pas par Éric, mais elle reste une demande et reste visible —
 * zone verte, libellée « autorisé », jamais « exécuté ». L'output y
 * arrivera ensuite par llm_tile_box_result, sous le même tool_call_id. */
void llm_tile_box_auto(LlmTile *t, const char *summary,
                       const char *tool_call_id, gboolean allowplus);

/* Verse le résultat d'un outil dans la boîte qui l'attend. TRUE si la vue
 * a absorbé le texte (ne pas l'écrire en clair) ; FALSE sinon (aucune
 * boîte pour cet ID dans cette vue) → le core rend le texte au fil. */
gboolean llm_tile_box_result(LlmTile *t, const char *tool_call_id,
                             const char *text);
void llm_cdb_say_display(LlmTile *t, const char *text);

void llm_tile_turn_reset(LlmTile *t);

#endif
