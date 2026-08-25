#ifndef CDB_LLMCORE_H
#define CDB_LLMCORE_H

/* Prototypes generes - split C0 */

char *
llm_config_path(void)
;

const char *
llm_provider_default_url(const char *provider)
;

LlmModelInfo *
llm_models_copy(const LlmModelInfo *models)
;

void
llm_models_free(LlmModelInfo *models)
;

void
md_enrich(LlmModelInfo *models, const char *provider)
;

void
md_deliver(ModelsFetch *f, LlmModelInfo *models)
;

MdPending *
md_deferred_new(ModelsFetch *f, LlmModelInfo *models)
;

void
md_load_done(GObject *source, GAsyncResult *res,
             gpointer G_GNUC_UNUSED data)
;

void
md_load_start(void)
;

void
models_fetch_done(GObject *source, GAsyncResult *res, gpointer data)
;

void
llm_models_fetch(const char *provider, LlmModelsCallback cb,
                 gpointer user_data)
;

JsonObject *
llm_config_provider_object(const char *provider, JsonNode **root_node)
;

char *
llm_config_get_api_key(const char *provider)
;

char *
llm_config_get_allowed_models(const char *provider)
;

void
llm_config_set_allowed_models(const char *provider, const char *filter)
;

gboolean
llm_model_allowed(const char *filter, const char *id)
;

void
llm_retry429_load(LlmRetry429 *out)
;

void
llm_config_save_retry429(gboolean retry, int max_retries, int delay_ms)
;

void
llm_retry5xx_load(LlmRetry5xx *out)
;

void
llm_config_save_retry5xx(gboolean retry, int max_retries, int delay_ms)
;

char **
llm_config_provider_names(void)
;

void
llm_config_switch_active(LlmConfig *cfg, const char *provider,
                         const char *model)
;

void
llm_config_free(LlmConfig *cfg)
;

LlmConfig *
llm_config_load(void)
;

void
llm_config_save_provider(const char *provider, const char *api_key)
;

const char *
llm_msg_wire_role(LlmActor a)
;

char *
str_replace_all(const char *s, const char *old_s, const char *new_s)
;

char *
llm_persona_raw(void)
;

void
llm_persona_save(const char *text)
;

void
history_push_images(LlmTile *t, LlmActor actor, gboolean local,
                    const char *content, GPtrArray *images)
;

void
history_push(LlmTile *t, LlmActor actor, gboolean local, const char *content)
;

long
llm_json_int(JsonObject *obj, const char *member, long fallback)
;

void
llm_handle_sse_line(LlmTile *t, const char *line)
;

void
llm_request_detach(LlmTile *t)
;

void
llm_cancel_current(LlmTile *t)
;

void
llm_request_free(LlmRequest *req)
;

void
llm_process_bytes(LlmRequest *req, const char *bytes, gssize n)
;

void
llm_stream_read(GObject G_GNUC_UNUSED *source, GAsyncResult *res,
                gpointer data)
;

void
cdb_poll_register(CdbPoll *pl)
;

void
cdd_poll_unregister(CdbPoll *pl)
;

void
llm_cdb_polls_purge(LlmTile *t)
;

void
cdb_poll_finish(CdbPoll *pl, const char *text)
;

gboolean
cdb_spawn_wait_tick(gpointer data)
;

gboolean
cdb_poll_tick(gpointer data)
;

void
llm_send_done(GObject *source, GAsyncResult *res, gpointer data)
;

void
llm_send_attempt(LlmRequest *req)
;

gboolean
llm_retry_tick(gpointer data)
;

void
llm_cdb_deliver(LlmTile *t, const char *text)
;

void
llm_cdb_requery(LlmTile *t)
;

void
llm_cdb_results_flush(LlmTile *t)
;

void
llm_cdb_next(LlmTile *t)
;

char *
llm_scan_text(const char *reply)
;

gboolean
llm_cdb_malformed(const char *reply)
;

gboolean
llm_agent_detect(LlmTile *t, const char *reply)
;

char *
llm_body_build(LlmTile *t)
;

void
llm_send(LlmTile *t, const char G_GNUC_UNUSED *prompt)
;

void
llm_history_wipe(LlmTile *t)
;

void
llm_queues_purge(LlmTile *t)
;

#endif
