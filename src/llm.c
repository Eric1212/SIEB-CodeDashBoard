/*
 * llm.c : tuile LLM — chat avec un provider OpenAI-compatible (SSE).
 *
 * Requête : POST {api_url}/chat/completions, stream=true.
 * Réponse : SSE « data: {…} », chaque chunk porte un delta de contenu ;
 * fin par « data: [DONE] ». La lecture incrémentale (libsoup async)
 * met à jour le GtkTextBuffer de l'historique au fil de l'eau.
 */

#define _POSIX_C_SOURCE 200809L
#include "llm.h"
#include "session.h"
#include "mdview.h"
#include "bashpanel.h"
#include "modal.h"
#include "llmslots.h"
#include "roots.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <string.h>
#include <gdk/gdkkeysyms.h>

/* ------------------------------------------------------------------ */
/* Config                                                             */
/* ------------------------------------------------------------------ */

static char *
llm_config_path(void)
{
    return session_config_path("llm.json");
}

/* URL de base d'un provider connu ; NULL si inconnu. */
const char *
llm_provider_default_url(const char *provider)
{
    if (g_strcmp0(provider, "OpenRouter") == 0)
        return "https://openrouter.ai/api/v1";
    if (g_strcmp0(provider, "OpenCode") == 0)
        return "https://opencode.ai/zen/v1";
    if (g_strcmp0(provider, "HyperCharm") == 0)
        return "https://hyper.charm.land/v1";
    return NULL;
}

/* ------------------------------------------------ */
/* Liste des modèles (GET {api_url}/models)          */
/* ------------------------------------------------ */

typedef struct {
    LlmModelsCallback cb;
    gpointer          user_data;
    SoupSession      *soup;
    char             *provider;
} ModelsFetch;

LlmModelInfo *
llm_models_copy(const LlmModelInfo *models)
{
    guint n = 0;

    while (models[n].id != NULL)
        n++;
    LlmModelInfo *copy = g_new0(LlmModelInfo, n + 1);

    for (guint i = 0; i < n; i++) {
        copy[i].id = g_strdup(models[i].id);
        copy[i].name = g_strdup(models[i].name);
    }
    return copy;
}

void
llm_models_free(LlmModelInfo *models)
{
    if (models == NULL)
        return;
    for (guint i = 0; models[i].id != NULL; i++) {
        g_free(models[i].id);
        g_free(models[i].name);
    }
    g_free(models);
}

/* ------------------------------------------------ */
/* models.dev : noms lisibles par provider           */
/*                                                   */
/* Certains providers (OpenCode Zen) ne renvoient    */
/* aucun nom dans /models. Leur client puise les     */
/* métadonnées dans models.dev/api.json : clé        */
/* provider → { models : { slug : { name } } }.      */
/* On charge ce JSON une fois, puis on enrichit les  */
/* listes avant de livrer les callbacks.             */
/* ------------------------------------------------ */

static GHashTable *md_names = NULL;   /* <provider_lower, GHashTable<slug,name>> */
static gboolean    md_started = FALSE;
static GSList     *md_pending = NULL; /* requêtes en attente du chargement */

typedef struct {
    ModelsFetch  *f;
    LlmModelInfo *models;
} MdPending;

static void md_deliver(ModelsFetch *f, LlmModelInfo *models);

/* Complète les noms manquants depuis le cache models.dev. */
static void
md_enrich(LlmModelInfo *models, const char *provider)
{
    char       *key;
    GHashTable *inner;

    if (md_names == NULL || provider == NULL)
        return;
    key = g_ascii_strdown(provider, -1);
    inner = g_hash_table_lookup(md_names, key);
    g_free(key);
    if (inner == NULL)
        return;
    for (guint i = 0; models[i].id != NULL; i++) {
        if (models[i].name != NULL)
            continue;
        {
            const char *nm = g_hash_table_lookup(inner,
                                                 models[i].id);

            if (nm != NULL)
                models[i].name = g_strdup(nm);
        }
    }
}

static void
md_deliver(ModelsFetch *f, LlmModelInfo *models)
{
    md_enrich(models, f->provider);
    f->cb(models, f->user_data);
    llm_models_free(models);
    g_free(f->provider);
    g_object_unref(f->soup);
    g_free(f);
}

static MdPending *
md_deferred_new(ModelsFetch *f, LlmModelInfo *models)
{
    MdPending *p = g_new0(MdPending, 1);

    p->f = f;
    p->models = models;
    return p;
}

/* Réception de api.json : construit le cache <provider, slug→name>. */
static void
md_load_done(GObject *source, GAsyncResult *res,
             gpointer G_GNUC_UNUSED data)
{
    GBytes *bytes;
    GError *err = NULL;

    bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res,
                                              &err);
    if (bytes != NULL) {
        JsonParser *parser = json_parser_new();
        gsize       len = g_bytes_get_size(bytes);

        if (json_parser_load_from_data(parser, g_bytes_get_data(bytes, NULL),
                                       (gssize)len, NULL)) {
            JsonObject *root =
                json_node_get_object(json_parser_get_root(parser));

            if (root != NULL) {
                GList *pm = json_object_get_members(root);

                md_names = g_hash_table_new_full(
                    g_str_hash, g_str_equal, g_free,
                    (GDestroyNotify)g_hash_table_unref);
                for (GList *l = pm; l != NULL; l = l->next) {
                    const char *pkey = l->data;
                    JsonNode   *pn = json_object_get_member(root, pkey);
                    JsonObject *po, *mo;
                    char       *lk;
                    GHashTable *inner;
                    GList      *sm;

                    if (pn == NULL || !JSON_NODE_HOLDS_OBJECT(pn))
                        continue;
                    po = json_node_get_object(pn);
                    if (!json_object_has_member(po, "models"))
                        continue;
                    mo = json_object_get_object_member(po, "models");
                    if (mo == NULL)
                        continue;

                    lk = g_ascii_strdown(pkey, -1);
                    inner = g_hash_table_new_full(
                        g_str_hash, g_str_equal, g_free, g_free);
                    g_hash_table_replace(md_names, lk, inner);

                    sm = json_object_get_members(mo);
                    for (GList *s = sm; s != NULL; s = s->next) {
                        const char *slug = s->data;
                        JsonNode   *sn = json_object_get_member(mo,
                                                                slug);
                        JsonObject *so;

                        if (sn == NULL || !JSON_NODE_HOLDS_OBJECT(sn))
                            continue;
                        so = json_node_get_object(sn);
                        if (json_object_has_member(so, "name")) {
                            const char *nm =
                                json_object_get_string_member(so,
                                                              "name");

                            if (nm != NULL && nm[0] != '\0')
                                g_hash_table_insert(inner,
                                                    g_strdup(slug),
                                                    g_strdup(nm));
                        }
                    }
                }
            }
        }
        g_object_unref(parser);
        g_bytes_unref(bytes);
    } else {
        g_printerr("SIEB: models.dev échoué : %s\n", err->message);
        g_error_free(err);
    }

    /* Livre tout ce qui attendait (avec ou sans enrichment). */
    for (GSList *l = md_pending; l != NULL; l = l->next) {
        MdPending *p = l->data;

        md_deliver(p->f, p->models);
        g_free(p);
    }
    g_slist_free(md_pending);
    md_pending = NULL;
}

static void
md_load_start(void)
{
    SoupSession *soup = soup_session_new();
    SoupMessage *msg =
        soup_message_new("GET", "https://models.dev/api.json");

    /* Le message appartient à la session après l'appel. */
    soup_session_send_and_read_async(soup, msg, G_PRIORITY_DEFAULT,
                                     NULL, md_load_done, NULL);
}

static void
models_fetch_done(GObject *source, GAsyncResult *res, gpointer data)
{
    ModelsFetch   *f = data;
    GBytes        *bytes;
    GError        *err = NULL;
    LlmModelInfo  *models = NULL;

    bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res,
                                              &err);
    if (bytes != NULL) {
        JsonParser *parser = json_parser_new();
        gsize       len = g_bytes_get_size(bytes);

        if (json_parser_load_from_data(parser, g_bytes_get_data(bytes, NULL),
                                       (gssize)len, NULL)) {
            JsonObject *root =
                json_node_get_object(json_parser_get_root(parser));

            if (root != NULL && json_object_has_member(root, "data")) {
                JsonArray *arr = json_object_get_array_member(root, "data");
                guint      n = json_array_get_length(arr);

                models = g_new0(LlmModelInfo, n + 1);
                for (guint i = 0; i < n; i++) {
                    JsonObject *m = json_array_get_object_element(arr, i);

                    /* Nom lisible si le provider en fournit un
                     * (OpenRouter : « name », HyperCharm :
                     * « display_name »), sinon NULL → slug. */
                    models[i].id = g_strdup(
                        json_object_get_string_member(m, "id"));
                    if (json_object_has_member(m, "name")) {
                        const char *nm =
                            json_object_get_string_member(m, "name");

                        if (nm != NULL && nm[0] != '\0')
                            models[i].name = g_strdup(nm);
                    }
                    if (models[i].name == NULL &&
                        json_object_has_member(m, "display_name")) {
                        const char *nm =
                            json_object_get_string_member(m, "display_name");

                        if (nm != NULL && nm[0] != '\0')
                            models[i].name = g_strdup(nm);
                    }
                }
            }
        }
        g_object_unref(parser);
        g_bytes_unref(bytes);
    } else {
        g_printerr("SIEB: /models échoué : %s\n", err->message);
        g_error_free(err);
    }

    /* Livraison différée : enrichir d'abord depuis models.dev
     * (certains providers — OpenCode Zen — ne renvoient aucun nom). */
    if (!md_started) {
        md_started = TRUE;
        md_load_start();
    }
    if (md_names != NULL)
        md_deliver(f, models);
    else
        md_pending = g_slist_append(md_pending,
                                    md_deferred_new(f, models));
}

void
llm_models_fetch(const char *provider, LlmModelsCallback cb,
                 gpointer user_data)
{
    const char   *base = llm_provider_default_url(provider);
    ModelsFetch  *f;
    SoupMessage  *msg;
    char         *url;

    if (base == NULL || cb == NULL)
        return;

    f = g_new0(ModelsFetch, 1);
    f->cb = cb;
    f->user_data = user_data;
    f->provider = g_strdup(provider);
    f->soup = soup_session_new();

    url = g_strdup_printf("%s/models", base);
    msg = soup_message_new("GET", url);
    g_free(url);
    /* La session possède msg après l'appel : NE PAS unref ici.
     * Variante « read » : tout le corps en mémoire (les /models sont
     * petits) — le finish correspondant est send_and_read_finish. */
    soup_session_send_and_read_async(f->soup, msg, G_PRIORITY_DEFAULT,
                                     NULL, models_fetch_done, f);
}

/* ------------------------------------------------ */
/* Filtre de modèles autorisés                       */
/* ------------------------------------------------ */

/* Ouvre llm.json et renvoie l'objet du provider (ou NULL). root_node
 * est copié : à libérer par l'appelant (json_node_unref) si non-NULL. */
static JsonObject *
llm_config_provider_object(const char *provider, JsonNode **root_node)
{
    char       *path = llm_config_path();
    JsonParser *parser;
    JsonObject *root, *provs;

    *root_node = NULL;
    if (provider == NULL)
        return NULL;
    parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, NULL)) {
        g_object_unref(parser);
        g_free(path);
        return NULL;
    }
    g_free(path);
    if (json_parser_get_root(parser) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        g_object_unref(parser);
        return NULL;
    }
    root_node[0] = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser);
    root = json_node_get_object(*root_node);
    if (!json_object_has_member(root, "providers"))
        return NULL;
    provs = json_object_get_object_member(root, "providers");
    if (provs == NULL || !json_object_has_member(provs, provider))
        return NULL;
    return json_object_get_object_member(provs, provider);
}

/* Clé API d'un provider (indépendant du provider actif). */
char *
llm_config_get_api_key(const char *provider)
{
    JsonObject *prov;
    JsonNode   *root_node = NULL;
    char       *key;

    prov = llm_config_provider_object(provider, &root_node);
    key = (prov != NULL && json_object_has_member(prov, "api_key"))
              ? g_strdup(json_object_get_string_member(prov, "api_key"))
              : NULL;
    if (root_node != NULL)
        json_node_unref(root_node);
    return key;
}

char *
llm_config_get_allowed_models(const char *provider)
{
    JsonObject *prov;
    JsonNode   *root_node = NULL;
    char       *filter;

    prov = llm_config_provider_object(provider, &root_node);
    filter = (prov != NULL && json_object_has_member(prov, "allowed_models"))
                 ? g_strdup(json_object_get_string_member(prov,
                                                          "allowed_models"))
                 : NULL;
    if (root_node != NULL)
        json_node_unref(root_node);
    return filter;
}

void
llm_config_set_allowed_models(const char *provider, const char *filter)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root, *provs, *prov;
    JsonNode   *work_root = NULL;
    JsonGenerator *gen;
    gchar      *text;
    GError     *error = NULL;
    gboolean    existed;

    existed = json_parser_load_from_file(parser, path, NULL);
    if (existed && json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        work_root = json_node_copy(json_parser_get_root(parser));
        root = json_node_get_object(work_root);
    } else {
        root = json_object_new();
        work_root = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(work_root, root);
    }
    if (!json_object_has_member(root, "providers")) {
        json_object_set_object_member(root, "providers", json_object_new());
    } else if (json_object_get_object_member(root, "providers") == NULL) {
        json_object_set_object_member(root, "providers", json_object_new());
    }
    provs = json_object_get_object_member(root, "providers");
    prov = json_object_has_member(provs, provider)
               ? json_object_get_object_member(provs, provider)
               : NULL;
    if (prov == NULL) {
        prov = json_object_new();
        json_object_set_string_member(prov, "api_url",
                                      llm_provider_default_url(provider) !=
                                              NULL
                                          ? llm_provider_default_url(
                                                provider)
                                          : "");
        json_object_set_string_member(prov, "api_key", "");
        json_object_set_string_member(prov, "allowed_models", filter);
        json_object_set_object_member(provs, provider, prov);
    } else {
        json_object_set_string_member(prov, "allowed_models", filter);
    }

    gen = json_generator_new();
    json_generator_set_root(gen, work_root);
    text = json_to_string(work_root, TRUE);
    if (!g_file_set_contents(path, text, -1, &error)) {
        g_printerr("SIEB: écriture allowed_models : %s\n", error->message);
        g_error_free(error);
    }
    g_free(text);
    g_object_unref(gen);
    json_node_unref(work_root);
    g_object_unref(parser);
    g_free(path);
}

gboolean
llm_model_allowed(const char *filter, const char *id)
{
    gchar **toks;
    gboolean ok = FALSE;

    if (filter == NULL || filter[0] == '\0')
        return TRUE; /* pas de filtre : tout passe */
    toks = g_strsplit(filter, ",", -1);
    for (int i = 0; toks[i] != NULL && !ok; i++) {
        char *tok = g_strstrip(toks[i]);

        ok = tok[0] != '\0' && strcmp(tok, id) == 0;
    }
    g_strfreev(toks);
    return ok;
}

/* Noms des providers connus : clés de la map « providers ». */
/* ------------------------------------------------ */
/* Retry 429 (section Harness)                       */
/* ------------------------------------------------ */

void
llm_retry429_load(LlmRetry429 *out)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();

    *out = LLM_RETRY429_DEFAULTS;
    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *root =
            json_node_get_object(json_parser_get_root(parser));

        if (root != NULL && json_object_has_member(root, "harness")) {
            JsonObject *h =
                json_object_get_object_member(root, "harness");

            if (h != NULL) {
                if (json_object_has_member(h, "retry_429"))
                    out->retry = json_object_get_boolean_member(
                        h, "retry_429");
                if (json_object_has_member(h, "max_retries_429"))
                    out->max_retries = (int)json_object_get_int_member(
                        h, "max_retries_429");
                if (json_object_has_member(h, "delay_ms_429"))
                    out->delay_ms = (int)json_object_get_int_member(
                        h, "delay_ms_429");
            }
        }
    }
    g_object_unref(parser);
    g_free(path);
}

void
llm_config_save_retry429(gboolean retry, int max_retries, int delay_ms)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root;
    JsonNode   *work = NULL;
    JsonObject *harness;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        work = json_node_copy(json_parser_get_root(parser));
        root = json_node_get_object(work);
    } else {
        root = json_object_new();
        work = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(work, root);
    }

    if (!json_object_has_member(root, "harness") ||
        json_object_get_object_member(root, "harness") == NULL)
        json_object_set_object_member(root, "harness",
                                      json_object_new());
    harness = json_object_get_object_member(root, "harness");
    json_object_set_boolean_member(harness, "retry_429", retry);
    json_object_set_int_member(harness, "max_retries_429", max_retries);
    json_object_set_int_member(harness, "delay_ms_429", delay_ms);

    {
        JsonGenerator *gen = json_generator_new();
        gchar         *text = json_to_string(work, TRUE);
        GError        *error = NULL;

        json_generator_set_root(gen, work);
        text = json_to_string(work, TRUE);
        if (!g_file_set_contents(path, text, -1, &error)) {
            g_printerr("SIEB: écriture retry429 : %s\n", error->message);
            g_error_free(error);
        }
        g_free(text);
        g_object_unref(gen);
    }
    json_node_unref(work);
    g_object_unref(parser);
    g_free(path);
}

void
llm_retry5xx_load(LlmRetry5xx *out)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();

    *out = LLM_RETRY5XX_DEFAULTS;
    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *root =
            json_node_get_object(json_parser_get_root(parser));

        if (root != NULL && json_object_has_member(root, "harness")) {
            JsonObject *h =
                json_object_get_object_member(root, "harness");

            if (h != NULL) {
                if (json_object_has_member(h, "retry_5xx"))
                    out->retry = json_object_get_boolean_member(
                        h, "retry_5xx");
                if (json_object_has_member(h, "max_retries_5xx"))
                    out->max_retries = (int)json_object_get_int_member(
                        h, "max_retries_5xx");
                if (json_object_has_member(h, "delay_ms_5xx"))
                    out->delay_ms = (int)json_object_get_int_member(
                        h, "delay_ms_5xx");
            }
        }
    }
    g_object_unref(parser);
    g_free(path);
}

void
llm_config_save_retry5xx(gboolean retry, int max_retries, int delay_ms)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root;
    JsonNode   *work = NULL;
    JsonObject *harness;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        work = json_node_copy(json_parser_get_root(parser));
        root = json_node_get_object(work);
    } else {
        root = json_object_new();
        work = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(work, root);
    }

    if (!json_object_has_member(root, "harness") ||
        json_object_get_object_member(root, "harness") == NULL)
        json_object_set_object_member(root, "harness",
                                      json_object_new());
    harness = json_object_get_object_member(root, "harness");
    json_object_set_boolean_member(harness, "retry_5xx", retry);
    json_object_set_int_member(harness, "max_retries_5xx", max_retries);
    json_object_set_int_member(harness, "delay_ms_5xx", delay_ms);

    {
        JsonGenerator *gen = json_generator_new();
        gchar         *text = json_to_string(work, TRUE);
        GError        *error = NULL;

        json_generator_set_root(gen, work);
        if (!g_file_set_contents(path, text, -1, &error)) {
            g_printerr("SIEB: écriture retry5xx : %s\n", error->message);
            g_error_free(error);
        }
        g_free(text);
        g_object_unref(gen);
    }
    json_node_unref(work);
    g_object_unref(parser);
    g_free(path);
}

char **
llm_config_provider_names(void)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root, *provs;
    char      **names = NULL;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        root = json_node_get_object(json_parser_get_root(parser));

        if (json_object_has_member(root, "providers")) {
            provs = json_object_get_object_member(root, "providers");

            if (provs != NULL) {
                GList *members = json_object_get_members(provs);
                guint  n = g_list_length(members);
                guint  i = 0;

                names = g_new0(char *, n + 1);
                for (GList *l = members; l != NULL; l = l->next)
                    names[i++] = g_strdup(l->data);
            }
        }
    }
    g_object_unref(parser);
    g_free(path);
    return names;
}

/* Bascule provider + modèle actifs : écrit « active » et rafraîchit la
 * config vivante (api_url/api_key repris du provider choisi). Les clés
 * des autres providers sont préservées. */
void
llm_config_switch_active(LlmConfig *cfg, const char *provider,
                         const char *model)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root = NULL, *active;
    JsonNode   *work = NULL;
    JsonGenerator *gen;
    gchar      *text;
    GError     *error = NULL;
    JsonObject *provs = NULL, *prov = NULL;
    const char *new_url = NULL;
    char       *new_key = NULL;

    if (cfg == NULL || provider == NULL || model == NULL) {
        g_object_unref(parser);
        g_free(path);
        return;
    }

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        work = json_node_copy(json_parser_get_root(parser));
        root = json_node_get_object(work);
    } else {
        root = json_object_new();
        work = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(work, root);
    }

    /* api_url/api_key du provider choisi (s'il est déjà connu). */
    if (json_object_has_member(root, "providers") &&
        json_object_get_object_member(root, "providers") != NULL) {
        provs = json_object_get_object_member(root, "providers");

        if (json_object_has_member(provs, provider))
            prov = json_object_get_object_member(provs, provider);
    }
    if (prov != NULL && json_object_has_member(prov, "api_url")) {
        const char *u = json_object_get_string_member(prov, "api_url");

        new_url = (u != NULL && u[0] != '\0') ? u
                    : llm_provider_default_url(provider);
    } else {
        new_url = llm_provider_default_url(provider);
    }
    new_key = (prov != NULL && json_object_has_member(prov, "api_key"))
                  ? g_strdup(json_object_get_string_member(prov,
                                                           "api_key"))
                  : g_strdup("");

    /* active.{provider,model} — les providers ne sont pas touchés. */
    active = json_object_new();
    json_object_set_string_member(active, "provider", provider);
    json_object_set_string_member(active, "model", model);
    json_object_set_object_member(root, "active", active);

    /* COPIE immédiate : la chaîne vit dans l'arbre JSON qui sera libéré
     * plus bas (json_node_unref) — garder le pointeur serait un UAF. */
    {
        char *url_copy = (new_url != NULL) ? g_strdup(new_url) : NULL;

        gen = json_generator_new();
        text = json_to_string(work, TRUE);
        if (!g_file_set_contents(path, text, -1, &error)) {
            g_printerr("SIEB: écriture switch active : %s\n",
                       error->message);
            g_error_free(error);
        }
        g_free(text);
        g_object_unref(gen);
        json_node_unref(work);
        g_object_unref(parser);
        g_free(path);

        /* Rafraîchit la config vivante pour les prochains envois. */
        g_free(cfg->provider);
        cfg->provider = g_strdup(provider);
        g_free(cfg->model);
        cfg->model = g_strdup(model);
        if (url_copy != NULL && url_copy[0] != '\0') {
            g_free(cfg->api_url);
            cfg->api_url = url_copy;
        } else {
            g_free(url_copy); /* pas d'URL connue : on garde l'actuelle */
        }
        g_free(cfg->api_key);
        cfg->api_key = new_key; /* possession transférée */
    }
}

void
llm_config_free(LlmConfig *cfg)
{
    if (cfg == NULL)
        return;
    g_free(cfg->provider);
    g_free(cfg->model);
    g_free(cfg->api_url);
    g_free(cfg->api_key);
    g_free(cfg);
}

LlmConfig *
llm_config_load(void)
{
    char       *path = llm_config_path();
    JsonParser *parser;
    LlmConfig  *cfg = NULL;
    JsonObject *root, *active, *prov_obj;
    const char *prov_name;

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return NULL;
    }
    parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, NULL))
        goto out;
    root = json_node_get_object(json_parser_get_root(parser));
    if (root == NULL || !json_object_has_member(root, "active"))
        goto out;
    active = json_object_get_object_member(root, "active");
    if (active == NULL || !json_object_has_member(active, "provider"))
        goto out;
    prov_name = json_object_get_string_member(active, "provider");

    cfg = g_new0(LlmConfig, 1);
    cfg->provider = g_strdup(prov_name);

    /* Modèle actif : uniquement active.model. AUCUN repli — pas de
     * « default_model » ni de modèle injecté d'office. */
    cfg->model = json_object_has_member(active, "model")
                 ? g_strdup(json_object_get_string_member(active, "model"))
                 : NULL;

    /* Provider actif : api_url + api_key. */
    if (!json_object_has_member(root, "providers")) {
        llm_config_free(cfg);
        cfg = NULL;
        goto out;
    }
    {
        JsonObject *provs = json_object_get_object_member(root, "providers");

        if (provs == NULL || !json_object_has_member(provs, prov_name)) {
            llm_config_free(cfg);
            cfg = NULL;
            goto out;
        }
        prov_obj = json_object_get_object_member(provs, prov_name);
        cfg->api_url = json_object_has_member(prov_obj, "api_url")
                       ? g_strdup(json_object_get_string_member(prov_obj,
                                                                "api_url"))
                       : NULL;
        cfg->api_key = json_object_has_member(prov_obj, "api_key")
                       ? g_strdup(json_object_get_string_member(prov_obj,
                                                                "api_key"))
                       : NULL;
    }

    /* Config incomplète = pas de chat. La clé est OPTIONNELLE
     * (providers gratuits type OpenCode Zen). Le modèle peut être
     * absent : il se choisit dans le menu de la tuile — la tuile
     * reste utilisable, l'envoi est refusé tant qu'aucun modèle
     * n'est actif. */
    if (cfg->api_url == NULL) {
        llm_config_free(cfg);
        cfg = NULL;
    }
out:
    g_object_unref(parser);
    g_free(path);
    return cfg;
}

/* Sauvegarde (création/màj) d'un provider dans llm.json : la clé.
 * Le fichier existant est rechargé en arbre, modifié, réécrit — les
 * autres providers sont préservés. « active » n'est JAMAIS réécrit
 * (sauf première création, sans modèle) : le provider/modèle actifs
 * se choisissent dans le menu de la tuile LLM (switch_active). */
void
llm_config_save_provider(const char *provider, const char *api_key)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonObject *root, *provs, *prov;
    JsonNode   *root_node = NULL;
    JsonGenerator *gen;
    gchar      *text;
    GError     *error = NULL;

    if (json_parser_load_from_file(parser, path, NULL)) {
        root_node = json_node_copy(json_parser_get_root(parser));
        root = json_node_get_object(root_node);
        if (!json_object_has_member(root, "providers"))
            json_object_set_object_member(root, "providers",
                                          json_object_new());
        provs = json_object_get_object_member(root, "providers");
    } else {
        /* Fichier absent/invalide : nouvelle racine. */
        root = json_object_new();
        provs = json_object_new();
        json_object_set_object_member(root, "providers", provs);
        root_node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(root_node, root);
    }

    prov = json_object_has_member(provs, provider)
           ? json_object_get_object_member(provs, provider)
           : NULL;
    if (prov == NULL) {
        prov = json_object_new();
        json_object_set_string_member(prov, "api_url", "");
        json_object_set_object_member(provs, provider, prov);
    }
    json_object_set_string_member(prov, "api_key", api_key);
    /* Un éventuel « default_model » hérité de l'ancienne config est
     * retiré : le mécanisme de repli n'existe plus. */
    if (json_object_has_member(prov, "default_model"))
        json_object_remove_member(prov, "default_model");

/* URL par défaut si absente, selon le provider. */
    if (g_strcmp0(json_object_get_string_member(prov, "api_url"), "") == 0) {
        const char *def = llm_provider_default_url(provider);

        if (def != NULL)
            json_object_set_string_member(prov, "api_url", def);
    }

    /* « active » n'existe pas encore (première sauvegarde) : poser le
     * provider, SANS modèle — il sera choisi dans le menu de la tuile.
     * S'il existe : on n'y touche PAS (un Enregistrer dans les Settings
     * ne doit jamais changer ce avec quoi on chatte). */
    if (!json_object_has_member(root, "active") ||
        json_object_get_object_member(root, "active") == NULL) {
        JsonObject *active = json_object_new();

        json_object_set_string_member(active, "provider", provider);
        json_object_set_string_member(active, "model", "");
        json_object_set_object_member(root, "active", active);
    }

    gen = json_generator_new();
    json_generator_set_root(gen, root_node);
    json_generator_set_pretty(gen, TRUE);
    text = json_generator_to_data(gen, NULL);
    if (!g_file_set_contents(path, text, -1, &error)) {
        g_printerr("CDB: écriture llm.json : %s\n", error->message);
        g_error_free(error);
    }
    g_free(text);
    g_object_unref(gen);
    json_node_unref(root_node);
    g_object_unref(parser);
    g_free(path);
}

/* ------------------------------------------------------------------ */
/* Tuile chat                                                         */
/* ------------------------------------------------------------------ */

/* Les trois acteurs du fil CDB. */
typedef enum {
    LLMACTOR_USER,   /* les mots d'Éric, verbatim */
    LLMACTOR_LLM,    /* la réponse du modèle */
    LLMACTOR_CDB,    /* la voix de l'IDE (annonces locales, résultats) */
} LlmActor;

/* Un échange de l'historique de conversation.
 * local = TRUE : affiché dans le fil mais JAMAIS envoyé au modèle
 * (annonces CDB : erreurs HTTP, changements d'état…). */
typedef struct {
    LlmActor actor;
    gboolean local;
    char    *content;
} LlmMsg;

static const char *
llm_msg_wire_role(LlmActor a)
{
    switch (a) {
    case LLMACTOR_LLM:
        return "assistant";
    case LLMACTOR_CDB:
        return "system";
    case LLMACTOR_USER:
    default:
        return "user";
    }
}

/* Requête en cours : définie plus bas, référencée par LlmTile. */
typedef struct LlmRequest LlmRequest;

typedef struct {
    GtkWidget   *view;      /* historique (GtkTextView, non éditable) */
    GtkTextBuffer *hist;    /* buffer de l'historique */
    GtkWidget   *entry;     /* saisie multi-lignes (GtkTextView) */
    GtkTextBuffer *entry_buf; /* buffer de la saisie */
    GtkWidget   *entry_scroll; /* fenêtre scrollée de la saisie */
    GtkWidget   *compose;      /* bloc de composition (réf pour largeur pop) */
    GtkWidget   *chevron;      /* label ▾/▴ du sélecteur de modèle */
    GtkWidget   *model_phrase; /* label phrasique du sélecteur */
    GtkWidget   *send_btn;
    GCancellable *cancel;   /* annulation de la requête en cours */
    LlmRequest  *cur_req;   /* requête active (annulation pendant flux) */
    gboolean     stop_requested; /* pause cliquée : jette tout entrant */
    GtkWidget   *hist_view; /* la vue (pour les boutons dans le fil) */
    LlmConfig   *cfg;
    SoupSession *soup;
    gboolean     busy;      /* requête en cours */
    gboolean     in_reasoning; /* delta courant = thinking */
    gboolean     follow;     /* scroll auto actif (user en bas) */
    GtkAdjustment *adj;      /* adjustment vertical de l'historique :
                              * scroll mécanique (set_value), sans
                              * déplacer le curseur ni dépendre du
                              * layout asynchrone de la vue */
    GString     *reply;     /* réponse en cours d'accumulation */
    gsize        rendered_len; /* nb d'octets de reply déjà rendus dans
                               * le fil (rendu incrémental) */
    GtkTextMark *reply_mark;/* marque de fin de la réponse en streaming */
    GArray      *history;   /* LlmMsg[] : fil de conversation envoyé */
    GtkWidget   *model_btn; /* sélecteur de modèle (menu, label = actif) */
    GtkWidget   *slots_btn; /* bouton menu persistance (slots JSON) */
    int         *modal_count; /* compteur de modales d'App (emprunté) */
    GtkWidget   *model_pop; /* popover : recherche + sections provider */
    GtkWidget   *model_search; /* filtre live des rangées */
    GtkWidget   *rows_box;  /* conteneur vertical des sections */
    GPtrArray   *sections;  /* ModelSection[] par provider */
    gboolean     menu_built;/* popover peuplé au premier ouvert */
    GActionGroup *actions;  /* pour « Configurer… » (ref ; emprunté sinon) */
    GListStore  *roots;     /* résolution du projet courant (empruntés, */
    GHashTable  *multi_paths; /* comme BashPanel) */
    GQueue      *cmd_queue; /* commandes /CDB:: valides en attente */
    GQueue      *cdb_results; /* résultats pendants {label,text} */
    int          cdb_retries; /* malformations consécutives (max 3) */
    int          slot_origin;     /* -1 = non sauvegardé, sinon numéro du slot */
    int          turns_since_ref; /* tours utilisateur depuis la référence */
    gsize        ref_body_size;   /* taille du body à la référence */
    GtkWidget   *slots_title;     /* label titre du popover slots */
} LlmTile;

/* Résultat d'exécution en attente de livraison. */
typedef struct {
    char *label; /* « bash-N » */
    char *text;
} CdbResult;

/* Spécification d'une commande /CDB:: parsée. */
typedef struct {
    int   tab;
    char *cmd;
} CdbCmdSpec;

#define CDB_RETRY_MAX 3

/* Une section de provider dans le sélecteur : en-tête + listbox. */
typedef struct {
    char          *provider;
    GtkWidget     *header;
    GtkWidget     *list;   /* GtkListBox single-click */
    LlmModelInfo  *models; /* tableau NULL-terminé (copie possédée) */
} ModelSection;

static void on_llm_send_clicked(GtkButton *btn, gpointer data);
static void llm_stream_read(GObject *source, GAsyncResult *res, gpointer data);
static void llm_scroll_to_end(LlmTile *t);
static void on_llm_scroll(GtkAdjustment *adj, gpointer data);
static void llm_model_section_refresh(LlmTile *t, ModelSection *sec);
static void llm_model_menu_apply_filter(LlmTile *t);
static void llm_model_menu_ensure(LlmTile *t);
static void llm_model_button_refresh(LlmTile *t);
static void llm_model_pop_width_sync(LlmTile *t);
static void llm_model_chevron_update(GtkWidget *popover, gpointer data);
static void llm_cdb_polls_purge(LlmTile *t);
static void llm_cdb_deliver(LlmTile *t, const char *text);
static gboolean llm_cdb_malformed(const char *reply);

/* Persistance slots (menu du bouton folder-templates). */
static void llm_slots_view(LlmTile *t);
static void llm_slots_save_dialog(LlmTile *t);
static void llm_slots_load_dialog(LlmTile *t);
static void llm_slots_clear_dialog(LlmTile *t);
static void llm_slots_import_dialog(LlmTile *t);
static void llm_chat_clear_dialog(LlmTile *t);
static void llm_slots_title_update(LlmTile *t);

static void
llm_tile_free(gpointer data)
{
    LlmTile *t = data;

    llm_cdb_polls_purge(t); /* polls actifs rattachés à cette tuile */
    if (t->cancel != NULL)
        g_object_unref(t->cancel);
    if (t->soup != NULL)
        g_object_unref(t->soup);
    if (t->reply != NULL)
        g_string_free(t->reply, TRUE);
    if (t->history != NULL) {
        for (guint i = 0; i < t->history->len; i++) {
            LlmMsg *m = &g_array_index(t->history, LlmMsg, i);

            g_free(m->content);
        }
        g_array_free(t->history, TRUE);
    }
    /* cfg EMPRUNTÉE à App (app->llm_cfg) : PAS libérée ici — main()
     * la libère une seule fois en fin de programme. */
    if (t->cmd_queue != NULL) {
        for (GList *l = t->cmd_queue->head; l != NULL; l = l->next) {
            CdbCmdSpec *s = l->data;

            g_free(s->cmd);
            g_free(s);
        }
        g_queue_free(t->cmd_queue);
    }
    if (t->cdb_results != NULL) {
        for (GList *l = t->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            g_free(r->label);
            g_free(r->text);
            g_free(r);
        }
        g_queue_free(t->cdb_results);
    }
    if (t->sections != NULL)
        g_ptr_array_unref(t->sections); /* libère les ModelSection */
    if (t->actions != NULL)
        g_object_unref(t->actions);
    g_free(t);
}

/* Libère une section de provider (free func du GPtrArray). */
static void
model_section_free(gpointer data)
{
    ModelSection *sec = data;

    g_free(sec->provider);
    llm_models_free(sec->models);
    g_free(sec);
}

/* La requête de recherche correspond-elle à l'id ? Vide = tout. */
static gboolean
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

/* Reconstruit les rangées d'une section : filtre « autorisés » +
 * ✓ devant le modèle actif (provider ET id). */

/* Label phrasique du bouton modèle : « [Nom long] par [provider] ·
 * slug [slug] ». Le nom long vient des sections fetchées (models.dev) ;
 * tant qu'elles ne sont pas chargées, le slug seul tient lieu de nom. */
static void
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

static void
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

/* Clic sur un modèle : bascule provider + modèle ensemble (persisté),
 * rafraîchit tous les ✓ puis referme le popover. */
static void
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
    llm_model_button_refresh(t);
    for (guint i = 0; i < t->sections->len; i++)
        llm_model_section_refresh(t, g_ptr_array_index(t->sections, i));
    llm_model_menu_apply_filter(t);
    gtk_popover_popdown(GTK_POPOVER(t->model_pop));
}

/* Recherche live : masque les rangées non-correspondantes et les
 * sections qui deviennent vides. */
static void
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

static void
on_llm_model_search_changed(GtkSearchEntry G_GNUC_UNUSED *entry,
                            gpointer data)
{
    llm_model_menu_apply_filter(data);
}

/* « Configurer… » : ouvre la fenêtre Settings (action de app->win). */
static void
on_llm_configure_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    LlmTile *t = data;

    if (t->actions != NULL)
        g_action_group_activate_action(t->actions, "settings", NULL);
}

/* Fetch des /models d'UNE section. La ref sur l'ancre garantit que la
 * tuile est vivante au callback, même après un re-rendu du layout. */
typedef struct {
    LlmTile      *t;
    GtkWidget    *anchor; /* ref possédée pendant le vol */
    ModelSection *sec;
} SectionFetchCtx;

static void
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

static void
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

/* Le popover termine juste à droite du chevron : même largeur que le
 * bouton sélecteur (le chevron en est le dernier élément), ancré à
 * gauche sur ce même bouton. */
static void
llm_model_pop_width_sync(LlmTile *t)
{
    int width;

    if (t->model_btn == NULL || t->model_pop == NULL)
        return;
    width = gtk_widget_get_width(t->model_btn);
    if (width > 100)
        gtk_widget_set_size_request(t->model_pop, width, -1);
}

/* Chevron dynamique : ▾ menu fermé (il s'ouvre vers le bas), ▴ menu
 * ouvert (déployé). Remplace l'icône flèche du GtkMenuButton — le child
 * custom du bouton porte la phrase + ce label. */
static void
llm_model_chevron_update(GtkWidget *popover, gpointer data)
{
    LlmTile *t = data;

    if (t->chevron != NULL)
        gtk_image_set_from_icon_name(GTK_IMAGE(t->chevron),
            gtk_widget_get_mapped(popover) ? "pan-up-symbolic"
                                           : "pan-down-symbolic");
}

/* Peuple le menu au premier affichage : une section par provider connu
 * de llm.json, chacune fetchant ses /models (les sections apparaissent
 * progressivement). */
static void
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

/* ------------------------------------------------ */
/* Persona CDB (prompts/default.txt par session)     */
/* ------------------------------------------------ */

/* Remplace toutes les occurrences de old par new dans s. */
static char *
str_replace_all(const char *s, const char *old_s, const char *new_s)
{
    gchar **parts = g_strsplit(s, old_s, -1);

    if (parts == NULL || parts[0] == NULL) {
        g_strfreev(parts);
        return g_strdup(s);
    }
    {
        char *joined = g_strjoinv(new_s, parts);

        g_strfreev(parts);
        return joined;
    }
}

#define LLM_INITPROMPT_DEFAULT /* « Init-Prompt » */ \
    "Bonjour Claude. Je suis CodeDashBoard(CDB), un logiciel IDE qui "     \
    "sert de relais entre toi et Éric Boucher. Éric t'a engagé pour tes "  \
    "compétences de programmeur senior en système, en simulation et jeux " \
    "vidéo ainsi que ton éthique de travail.\n\n"                          \
    "Tu travailles exclusivement à distance. Ton employeur ne te voit "    \
    "jamais : l'intégralité de votre relation de travail passe par CDB, "  \
    "qui fonctionne sur le poste de travail qui vous est à tous deux "     \
    "assignés.\n\n"                                                       \
    "Projet : [PROJET] ([CHEMIN]).\n\n"                                   \
    "## Contrôle à distance des terminaux\n"                              \
    "En tout temps, tu contrôles les terminaux de CDB en écrivant dans "   \
    "ta réponse une commande au format exact :\n"                          \
    "/CDB::bash-N::\"//\"CDB-IN\"//COMMANDE-VOULU//\"CDB-OUT\"//\"\n"      \
    "- N : numéro du terminal (0 à 9 ; il est créé s'il n'existe pas)\n"   \
    "- COMMANDE-VOULU : la commande shell, écrite telle quelle entre\n"    \
    "  les marqueurs //\"CDB-IN\"// et //\"CDB-OUT\"// — guillemets\n"     \
    "  doubles et sauts de ligne y sont autorisés sans échappement.\n"     \
    "Le résultat est restitué intégralement (fenêtre de "                  \
    "100000 lignes). Pour paginer, utilise head/tail/sed DANS la "         \
    "commande.\n\n"                                                        \
    "Chaque commande est soumise à l'approbation d'Éric avant exécution. " \
    "Si Éric refuse, CDB te l'indiquera clairement dans le fil : ce n'est " \
    "pas un bug, c'est une décision — adapte-toi et propose autre chose. " \
    "Après exécution, CDB répondra dans le fil avec le résultat demandé : " \
    "continue ton travail à partir de là.\n"

/* Texte BRUT du prompt (sans substitutions) : fichier s'il existe,
 * sinon le défaut intégré. Pour l'éditeur Settings → Harness.
 * Chaîne à libérer (g_free). */
char *
llm_persona_raw(void)
{
    char *path = session_config_path("prompts/default.txt");
    char *txt = NULL;

    if (g_file_test(path, G_FILE_TEST_EXISTS))
        g_file_get_contents(path, &txt, NULL, NULL);
    if (txt == NULL)
        txt = g_strdup(LLM_INITPROMPT_DEFAULT);
    g_free(path);
    return txt;
}

void
llm_persona_save(const char *text)
{
    char       *path = session_config_path("prompts/default.txt");
    char       *dir = g_path_get_dirname(path);
    GError     *error = NULL;

    g_mkdir_with_parents(dir, 0755);
    if (!g_file_set_contents(path, text != NULL ? text : "", -1, &error)) {
        g_printerr("SIEB: écriture prompts/default.txt : %s\n",
                   error->message);
        g_error_free(error);
    }
    g_free(dir);
    g_free(path);
}

/* Texte final pour l'envoi : raw + substitutions [PROJET]/[CHEMIN]
 * résolues depuis le projet actuellement sélectionné (comme les
 * terminaux au spawn) ; à défaut, le répertoire courant. */
static char *
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

static void
history_push(LlmTile *t, LlmActor actor, gboolean local, const char *content)
{
    LlmMsg m = { actor, local, g_strdup(content) };

    g_array_append_vals(t->history, &m, 1);
}

/* Tags de voix (créés une fois par buffer). */
static void
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

/* Rendu de l'en-tête d'acteur dans la vue historique. */
static void
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

/* Annonce CDB locale : affichée dans le fil, JAMAIS envoyée au
 * modèle. C'est la voix propre du troisième acteur. */
static void
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

/* Ajoute du texte à l'historique. */
static void
hist_append(LlmTile *t, const char *text)
{
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_insert(t->hist, &end, text, -1);
}

/* Livre dans le fil la portion de reply pas encore rendue (rendu
 * INCREMENTAL : plus jamais de delete+réinsertion massive, donc plus
 * de flicker, et un coût par chunk constant même sur des Mo). Seules
 * les lignes complètes sont rendues : le fragment sans \n peut encore
 * changer au chunk suivant ; hist_flush_reply le livre à la fin. */
static void
hist_update_reply(LlmTile *t)
{
    GtkTextIter end;
    const char *s = t->reply->str;
    gsize       len = t->reply->len;
    gsize       safe;

    if (len < t->rendered_len) {
        /* Incohérence (ne devrait jamais arriver) : repli sûr —
         * re-rendu complet depuis la marque du tour. */
        GtkTextIter start;

        gtk_text_buffer_get_iter_at_mark(t->hist, &start, t->reply_mark);
        gtk_text_buffer_get_end_iter(t->hist, &end);
        gtk_text_buffer_delete(t->hist, &start, &end);
        gtk_text_buffer_get_end_iter(t->hist, &end);
        md_insert(t->hist, &end, t->reply->str);
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
    llm_scroll_to_end(t);
}

/* Livre le fragment final non rendu (fin de stream, annulation) : la
 * dernière ligne sans \n rejoint l'affichage, rendered_len rattrape. */
static void
hist_flush_reply(LlmTile *t)
{
    GtkTextIter end;

    if (t->rendered_len >= t->reply->len)
        return;
    gtk_text_buffer_get_end_iter(t->hist, &end);
    md_insert_append(t->hist, &end, t->reply->str + t->rendered_len,
                     t->reply->len - t->rendered_len, TRUE);
    t->rendered_len = t->reply->len;
}

/* Traite une ligne SSE « data: … ». */
static void
llm_handle_sse_line(LlmTile *t, const char *line)
{
    const char *payload;
    JsonParser *parser;
    JsonObject *obj, *choices0, *delta;
    JsonArray  *choices;

    if (g_str_has_prefix(line, "data: "))
        payload = line + 6;
    else if (g_strcmp0(line, "data:[DONE]") == 0
             || g_str_has_prefix(line, "data:"))
        payload = line + 5;
    else
        return;

    if (g_strcmp0(payload, "[DONE]") == 0)
        return;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, payload, -1, NULL)) {
        g_object_unref(parser);
        return;
    }
    obj = json_node_get_object(json_parser_get_root(parser));
    if (obj != NULL && json_object_has_member(obj, "choices")
        && (choices = json_object_get_array_member(obj, "choices")) != NULL
        && json_array_get_length(choices) > 0
        && (choices0 = json_array_get_object_element(choices, 0)) != NULL
        && json_object_has_member(choices0, "delta")
        && (delta = json_object_get_object_member(choices0, "delta")) != NULL) {
        const char *piece = NULL;

        /* Contenu final ; en repli, le reasoning (thinking) du modèle. */
        if (json_object_has_member(delta, "content")) {
            piece = json_object_get_string_member(delta, "content");
            if (piece == NULL || piece[0] == '\0')
                piece = NULL;
        }
        if (piece == NULL) {
            /* Champ reasoning : « reasoning » (OpenRouter) ou
             * « reasoning_content » (style DeepSeek) selon le
             * fournisseur — les deux sont acceptés, sinon le thinking
             * disparaît silencieusement selon le modèle choisi. */
            const char *rfield =
                json_object_has_member(delta, "reasoning") ? "reasoning"
                : json_object_has_member(delta, "reasoning_content")
                    ? "reasoning_content" : NULL;

            if (rfield != NULL) {
                piece = json_object_get_string_member(delta, rfield);
                if (piece != NULL && piece[0] != '\0') {
                    /* Première apparition du reasoning : tag d'ouverture. */
                    if (!t->in_reasoning) {
                        g_string_append(t->reply, "〔thinking〕 ");
                        t->in_reasoning = TRUE;
                    }
                } else {
                    piece = NULL;
                }
            }
        }
        if (piece != NULL) {
            gboolean is_content = json_object_has_member(delta, "content")
                                  && piece == json_object_get_string_member(
                                                 delta, "content");

            if (t->in_reasoning && is_content) {
                /* Transition thinking → contenu : tag de fermeture. */
                g_string_append(t->reply, " 〔/thinking〕\n\n");
                t->in_reasoning = FALSE;
            }
            g_string_append(t->reply, piece);
            hist_update_reply(t);
        }
    }
    g_object_unref(parser);
}

struct LlmRequest {
    LlmTile      *tile;
    SoupMessage  *msg;
    GInputStream *stream;
    char          scratch[4096]; /* buffer du read en cours */
    GString      *pending;      /* lignes SSE partielles (dynamique :
                                  * une ligne data: peut dépasser 8 Ko
                                  * quand le serveur agrège les deltas) */
    int           done;         /* garde anti double-libération */
    char         *url;          /* pour reconstruire les essais 429 */
    char         *body;         /* corps JSON de la requête */
    char         *auth;         /* header Authorization ou NULL */
    int           attempt;      /* numéro d'essai courant (0 = premier) */
};

static void llm_send_attempt(LlmRequest *req);
static gboolean llm_agent_detect(LlmTile *t, const char *reply);
static void     llm_send(LlmTile *t, const char *prompt);
static gboolean llm_retry_tick(gpointer data);
static void     llm_busy_set(LlmTile *t, gboolean busy);

/* État busy centralisé : sensibilité + ICÔNE du bouton (play = envoyer,
 * pause = annuler). Tous les chemins de fin de requête passent ici —
 * plus aucun risque d'oublier de remettre le play. */
static void
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
}

/* Clic sur le bouton média : play = envoyer, pause = annuler la
 * requête en cours (le flux se termine en erreur G_IO_ERROR_CANCELLED,
 * capturée silencieusement par les chemins de lecture). */
static void
llm_cancel_current(LlmTile *t)
{
    /* 1. Requête réseau en cours : annule le flux ET FERME LA CONNEXION.
     * Le cancellable seul interrompt notre lecture locale, mais libsoup
     * peut continuer à drainer le socket en arrière-plan — le serveur
     * continue alors de générer et la réponse « arrive quand même ».
     * g_input_stream_close force le close TCP : le serveur voit la
     * déconnexion et stoppe sa génération (comportement Zed/OpenCode).
     * Le flag stop_requested complète le dispositif : tout chunk déjà
     * en vol est JETÉ à réception au lieu d'être affiché. */
    t->stop_requested = TRUE;
    if (t->cancel != NULL)
        g_cancellable_cancel(t->cancel);
    if (t->cur_req != NULL && t->cur_req->stream != NULL)
        g_input_stream_close_async(t->cur_req->stream, G_PRIORITY_DEFAULT,
                                   NULL, NULL, NULL);

    /* 2. Boucle agentique en attente (approbation, exécution bash,
     * re-requête) : rien n'écoute le cancellable — on vide la file et
     * on rend la main. Les polls bash en cours se termineront mais leur
     * résultat ne déclenchera plus de re-requête (file vide → flush →
     * requery est court-circuité par busy=FALSE ci-dessous). */
    if (t->cmd_queue != NULL && !g_queue_is_empty(t->cmd_queue)) {
        for (GList *l = t->cmd_queue->head; l != NULL; l = l->next) {
            CdbCmdSpec *s = l->data;

            g_free(s->cmd);
            g_free(s);
        }
        g_queue_free(t->cmd_queue);
        t->cmd_queue = NULL;
        hist_cdb_announce(t, "〔annulé〕 file de commandes vidée.");
    }
    /* 3. Résultats pendants non livrés : jetés (le user a dit stop). */
    if (t->cdb_results != NULL) {
        for (GList *l = t->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            g_free(r->label);
            g_free(r->text);
            g_free(r);
        }
        g_queue_free(t->cdb_results);
        t->cdb_results = NULL;
    }
    /* 4. Si aucun flux réseau n'était actif (attente approbation/poll),
     * personne ne remettra busy à FALSE : on le fait ici. Si un flux
     * était actif, son callback de fin le fera — double appel inoffensif. */
    llm_busy_set(t, FALSE);
}

/* Libère la requête une seule fois (les callbacks de complétion
 * peuvent arriver en double selon l'état du flux). */
static void
llm_request_free(LlmRequest *req)
{
    /* La requête courante de la tuile meurt : plus rien à annuler. */
    if (req->tile != NULL && req->tile->cur_req == req)
        req->tile->cur_req = NULL;
    g_free(req->url);
    g_free(req->body);
    g_free(req->auth);
    if (req->pending != NULL)
        g_string_free(req->pending, TRUE);
    if (req->done)
        return;
    req->done = 1;
    if (req->stream != NULL)
        g_object_unref(req->stream);
    g_free(req);
}

/* Réassemblage des lignes SSE. Le buffer est DYNAMIQUE (GString) : une
 * ligne « data: … » peut faire bien plus de 8 Ko quand le serveur agrège
 * de gros deltas dans un seul événement — l'ancien buffer fixe jetait
 * alors le début de ligne sans log, et un fragment perdu dans la zone
 * d'un marqueur /CDB:: rendait la commande indétectable ET non
 * condamnable (silence total de la boucle agentique, bug constaté). */
static void
llm_process_bytes(LlmRequest *req, const char *bytes, gssize n)
{
    LlmTile *t = req->tile;
    char    *nl;

    if ((gsize)n <= 0)
        return;
    g_string_append_len(req->pending, bytes, (gssize)n);
    while ((nl = strchr(req->pending->str, '\n')) != NULL) {
        gsize consumed = (gsize)(nl - req->pending->str) + 1;

        *nl = '\0';
        if (req->pending->str[0] != '\0')
            llm_handle_sse_line(t, req->pending->str);
        g_string_erase(req->pending, 0, consumed);
    }
}

static void
llm_stream_read(GObject G_GNUC_UNUSED *source, GAsyncResult *res,
                gpointer data)
{
    LlmRequest *req = data;
    LlmTile    *t = req->tile;
    gssize      n;
    GError     *error = NULL;

    n = g_input_stream_read_finish(req->stream, res, &error);

    /* Pause cliquée : tout entrant est jeté, fin immédiate. Le flag
     * couvre le cas où cancellable/close n'interrompent pas la lecture
     * assez vite (chunks en vol, drain libsoup) — constaté : 100 % de
     * la réponse arrivait APRÈS le clic. */
    if (t->stop_requested) {
        if (error != NULL)
            g_error_free(error);
        hist_flush_reply(t);
        history_push(t, LLMACTOR_LLM, FALSE, t->reply->str);
        llm_slots_title_update(t);
        hist_append(t, "\n〔annulé〕\n");
        llm_busy_set(t, FALSE);
        llm_request_free(req);
        return;
    }

    if (error != NULL) {
        gboolean cancelled = g_error_matches(error, G_IO_ERROR,
                                             G_IO_ERROR_CANCELLED);

        /* Annulation (pause) : la réponse partielle rejoint quand même
         * l'historique — le modèle a dit ce qu'il a dit — puis fin
         * silencieuse. */
        if (cancelled) {
            g_error_free(error);
            hist_flush_reply(t);
            history_push(t, LLMACTOR_LLM, FALSE, t->reply->str);
            llm_slots_title_update(t);
            hist_append(t, "\n〔annulé〕\n");
            llm_busy_set(t, FALSE);
            llm_request_free(req);
            return;
        }
        hist_append(t, error->message);
        g_error_free(error);
        llm_request_free(req);
        return;
    }

    if (n <= 0) {
        /* Fin du flux : le fragment final rejoint l'affichage, la
         * réponse complète rejoint l'historique (sans les tags
         * thinking, qui ne sont que de l'affichage). */
        hist_flush_reply(t);
        history_push(t, LLMACTOR_LLM, FALSE, t->reply->str);
        llm_slots_title_update(t);
        hist_append(t, "\n");

        /* Boucle agentique : des commandes /CDB:: dans la réponse ?
         * Si oui, busy RESTE actif — la suite passe par l'approbation
         * puis le résultat injecté (re-requête automatique).
         * /CDB:: présent mais MAL FORMÉ → feedback au modèle + retry
         * automatique (borné, anti-boucle infinie). */
        if (llm_agent_detect(t, t->reply->str)) {
            t->cdb_retries = 0;
            llm_request_free(req);
            return;
        }
        if (strstr(t->reply->str, "/CDB::") != NULL &&
            llm_cdb_malformed(t->reply->str)) {
            if (t->cdb_retries < CDB_RETRY_MAX) {
                char *note;

                t->cdb_retries++;
                note = g_strdup_printf(
                    "COMMANDE MAL FORMÉE (tentative %d/%d) : "
                    "le protocole est exactement "
                    "/CDB::bash-N::\"//\"CDB-IN\"//COMMANDE"
                    "//\"CDB-OUT\"//\" — N entre 0 et 9, "
                    "commande complète entre les deux marqueurs "
                    "(les \" internes sont autorisés tels quels). "
                    "Réécris-la proprement.",
                    t->cdb_retries, CDB_RETRY_MAX);
                llm_cdb_deliver(t, note);
                g_free(note);
                llm_request_free(req);
                return;
            }
            hist_cdb_announce(t,
                "trois commandes mal formées d'affilée : j'abandonne "
                "cette boucle. Réponds en texte ou reformule entièrement.");
        }

        llm_busy_set(t, FALSE);
        llm_request_free(req);
        return;
    }

    llm_process_bytes(req, req->scratch, n);
    g_input_stream_read_async(req->stream, req->scratch,
                              sizeof(req->scratch), G_PRIORITY_DEFAULT,
                              NULL, llm_stream_read, req);
}

/* ------------------------------------------------ */
/* Acteur CDB : contrôle à distance des terminaux    */
/*                                                   */
/* Le modèle écrit /CDB::bash-N::"//"CDB-IN"//cmd//"CDB-OUT"//" dans */
/* sa réponse ; CDB demande l'approbation d'Éric,    */
/* exécute DANS l'onglet bash N visible (sortie      */
/* déroutée vers un fichier-sentinelle), livre le    */
/* résultat au fil puis re-interroge le modèle.      */
/* ------------------------------------------------ */

typedef struct {
    LlmTile *t;
    char    *cmd;
    int      tab;
    /* Références pour se détacher proprement à la mort de la rangée
     * (re-rendu du layout = tuile détruite, boutons fantômes interdits). */
    GtkWidget *b_ok;
    GtkWidget *b_no;
} CdbApproval;

/* Libère l'approbation et débranche ses handlers. Connecté au signal
 * « destroy » de la rangée de boutons : si la tuile meurt, les boutons
 * meurent avec elle et `a` est libéré au même moment — plus jamais de
 * clic sur une structure déjà libérée (segfault bash-1467761504). */
static void
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

/* Résout la tuile depuis le widget émetteur (garanti vivant pendant
 * l'émission) au lieu du pointeur stocké — retourne NULL si la tuile
 * n'est plus accrochée à un fil vivant. */
static LlmTile *
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

typedef struct {
    LlmTile *t;
    char    *tab_label;
    int      tab;        /* index d'onglet surveillé */
    gchar   *prev_tail;  /* dernière ligne du round précédent */
    int      rounds;     /* rounds consécutifs finissant par un prompt */
    char    *pending_cmd; /* commande en attente du spawn du shell */
} CdbPoll;

#define CDB_POLL_MS   250    /* cadence de surveillance (décision Éric) */
#define CDB_ROUND_MIN 2      /* prompt vu 2 rounds de suite = terminé */
#define CDB_TAIL_LINES 100000 /* fenêtre de restitution = scrollback */
#define CDB_SPAWN_WAIT_MAX 120 /* ticks max d'attente du spawn (30 s) */

/* Prompt shell : [user]@[host]:[n'importe quoi]$ suivi UNIQUEMENT
 * d'espaces (le padding VTE remplit la fin de ligne ; une sortie du
 * type « user@host:$ Bonjour » ne matche PAS — il y a du texte après).
 * La ligne est donc évaluée BRUTE, sans retrait des espaces.
 * SANS ancre ^ : un prompt collé derrière une sortie sans \n final
 * (« fooeric@host:~$ ») compte aussi comme fin de commande — la
 * capture rogne alors le morceau prompt et garde le texte avant. */
#define CDB_PROMPT_RE \
    "[A-Za-z0-9_.@-]+@[A-Za-z0-9_.-]+:[^\\n]*\\$[ ]*$"

/* Registre des polls actifs : chaque tick valide son appartenance avant
 * de toucher pl (la tuile peut être détruite par un re-rendu du layout
 * pendant la surveillance) ; la mort d'une tuile purge ses polls. */
static GPtrArray *cdb_polls = NULL;

static void
cdb_poll_register(CdbPoll *pl)
{
    if (cdb_polls == NULL)
        cdb_polls = g_ptr_array_new();
    g_ptr_array_add(cdb_polls, pl);
}

static void
cdd_poll_unregister(CdbPoll *pl)
{
    if (cdb_polls != NULL)
        g_ptr_array_remove_fast(cdb_polls, pl);
}

/* Purge les polls rattachés à une tuile mourante (appelé par
 * llm_tile_free) : libère pl ; le tick suivant verra son pointeur
 * retiré du registre et se retirera silencieusement. */
static void
llm_cdb_polls_purge(LlmTile *t)
{
    if (cdb_polls == NULL)
        return;
    for (guint i = 0; i < cdb_polls->len; ) {
        CdbPoll *pl = g_ptr_array_index(cdb_polls, i);

        if (pl->t == t) {
            g_ptr_array_remove_index_fast(cdb_polls, i);
            g_free(pl->prev_tail);
            g_free(pl->tab_label);
            g_free(pl->pending_cmd);
            g_free(pl);
        } else
            i++;
    }
}

static void llm_cdb_deliver(LlmTile *t, const char *text);
static void llm_cdb_next(LlmTile *t);
static gboolean cdb_poll_tick(gpointer data);
static gboolean cdb_spawn_wait_tick(gpointer data);

static void
cdb_poll_finish(CdbPoll *pl, const char *text)
{
    CdbResult *r;

    cdd_poll_unregister(pl);
    bash_panel_set_busy((guint)pl->tab, FALSE); /* éteint le point */

    /* Anti-spam (loi d'Éric) : pas de livraison immédiate — le résultat
     * attend la fin de la file ; les résultats contenus à 100 % dans un
     * plus récent du même bash seront éliminés au flush. */
    r = g_new0(CdbResult, 1);
    r->label = g_steal_pointer(&pl->tab_label);
    r->text = g_strdup(text);
    if (pl->t->cdb_results == NULL)
        pl->t->cdb_results = g_queue_new();
    g_queue_push_tail(pl->t->cdb_results, r);

    llm_cdb_next(pl->t);

    g_free(pl->prev_tail);
    g_free(pl);
}

static void
on_cdb_refuse_clicked(GtkButton *btn, gpointer data)
{
    CdbApproval *a = data;
    LlmTile     *t = cdb_tile_from_button(btn);
    char        *note;

    if (t == NULL || a->t != t)
        return; /* bouton fantôme : tuile détruite depuis */

    /* Une demande = une décision : verrouille immédiatement. */
    gtk_widget_set_sensitive(a->b_ok, FALSE);
    gtk_widget_set_sensitive(a->b_no, FALSE);

    note = g_strdup_printf(
        "Éric a REFUSÉ cette commande. Ce n'est pas un bug : "
        "c'est une décision. Adapte-toi et propose autre chose.");

    llm_cdb_deliver(t, note);
    g_free(note);
    /* `a` reste propriété de la rangée : libéré à son destroy. */
}

static void
on_cdb_approve_clicked(GtkButton *btn, gpointer data)
{
    CdbApproval *a = data;
    LlmTile     *t = cdb_tile_from_button(btn);
    CdbPoll     *pl;

    if (t == NULL || a->t != t)
        return; /* bouton fantôme : tuile détruite depuis */

    /* Une demande = une décision : verrouille immédiatement. */
    gtk_widget_set_sensitive(a->b_ok, FALSE);
    gtk_widget_set_sensitive(a->b_no, FALSE);

    pl = g_new0(CdbPoll, 1);

    pl->t = t;
    pl->tab = a->tab;
    pl->tab_label = g_strdup_printf("bash-%d", a->tab);

    bash_panel_ensure_tabs((guint)(a->tab + 1));

    /* Spawn ASYNCHRONE : un onglet fraîchement créé n'a pas encore de PTY.
     * Injecter tout de suite = commande perdue (le shell ne lit pas encore
     * son entrée) et le poll verrait le prompt initial comme « terminé ».
     * On attend donc que le shell soit prêt avant d'injecter. */
    if (!bash_panel_term_ready((guint)a->tab)) {
        if (!bash_panel_exec_tab_possible()) {
            char *note = g_strdup_printf(
                "terminal %s indisponible (panneau bash absent ?)",
                pl->tab_label);

            hist_cdb_announce(t, note);
            g_free(note);
            g_free(pl->tab_label);
            g_free(pl);
            return;
        }
        pl->pending_cmd = g_strdup(a->cmd);
        cdb_poll_register(pl);
        g_timeout_add(CDB_POLL_MS, cdb_spawn_wait_tick, pl);
        return;
    }

    if (!bash_panel_exec_tab((guint)a->tab, a->cmd)) {
        char *note = g_strdup_printf(
            "terminal %s indisponible (panneau bash absent ?)",
            pl->tab_label);

        hist_cdb_announce(t, note);
        g_free(note);
        g_free(pl->tab_label);
        g_free(pl);
        return;
    }
    bash_panel_set_busy((guint)a->tab, TRUE); /* point orange */

    /* Surveillance du buffer VTE : prompt vu CDB_ROUND_MIN fois de
     * suite en fin de buffer = terminé (spécification Éric). Pas de
     * timeout (Ctrl+C humain). Busy reste verrouillé. `a` reste
     * propriété de la rangée. */
    cdb_poll_register(pl);
    g_timeout_add(CDB_POLL_MS, cdb_poll_tick, pl);
}

/* Attend que le shell d'un onglet fraîchement créé finisse son spawn
 * (PTY attaché), puis injecte la commande approuvée. Le poll normal ne
 * démarre qu'APRÈS l'injection : le prompt initial du shell ne peut plus
 * être confondu avec une fin de commande. */
static gboolean
cdb_spawn_wait_tick(gpointer data)
{
    CdbPoll *pl = data;
    int      waits;

    /* Poll purgé (tuile détruite) : se retire silencieusement. */
    if (cdb_polls == NULL || !g_ptr_array_find(cdb_polls, pl, NULL)) {
        g_free(pl->pending_cmd);
        g_free(pl);
        return G_SOURCE_REMOVE;
    }

    if (bash_panel_term_ready((guint)pl->tab)) {
        char *cmd = pl->pending_cmd;

        pl->pending_cmd = NULL; /* transféré */
        cdd_poll_unregister(pl); /* quitte le registre d'attente… */
        cdb_poll_register(pl);   /* …et y revient comme poll normal */
        bash_panel_exec_tab((guint)pl->tab, cmd);
        bash_panel_set_busy((guint)pl->tab, TRUE); /* point orange */
        g_free(cmd);
        g_timeout_add(CDB_POLL_MS, cdb_poll_tick, pl);
        return G_SOURCE_REMOVE;
    }

    waits = pl->rounds++;
    if (waits >= CDB_SPAWN_WAIT_MAX) {
        char *note = g_strdup_printf(
            "le shell %s ne démarre pas (spawn en échec ?).",
            pl->tab_label);

        cdb_poll_finish(pl, note);
        g_free(note);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
cdb_poll_tick(gpointer data)
{
    static GRegex *prompt_re = NULL;
    CdbPoll  *pl = data;
    gchar    *tail = NULL;
    gchar    *bounded = NULL;
    char     *note;
    gboolean  matched;

    /* Poll purgé (tuile détruite) : se retire silencieusement. */
    if (cdb_polls == NULL ||
        !g_ptr_array_find(cdb_polls, pl, NULL))
        return G_SOURCE_REMOVE;

    if (prompt_re == NULL)
        prompt_re = g_regex_new(CDB_PROMPT_RE, 0, 0, NULL);

    if (!bash_panel_term_alive((guint)pl->tab)) {
        note = g_strdup_printf(
            "le terminal %s a été fermé pendant l'exécution.",
            pl->tab_label);
        cdb_poll_finish(pl, note);
        g_free(note);
        return G_SOURCE_REMOVE;
    }

    /* Détection (spécification Éric) : la dernière ligne est un
     * prompt ET identique au round précédent. L'écho de la commande
     * chasse naturellement le vieux prompt de la queue à l'injection,
     * donc aucun état « avant » n'est nécessaire. */
    tail = bash_panel_last_line((guint)pl->tab);
    matched = tail != NULL && g_regex_match(prompt_re, tail, 0, NULL);

    if (!matched) {
        pl->rounds = 0;
        g_free(pl->prev_tail);
        pl->prev_tail = NULL;
        g_free(tail);
        return G_SOURCE_CONTINUE;
    }
    if (pl->prev_tail != NULL && strcmp(pl->prev_tail, tail) == 0)
        pl->rounds++;
    else
        pl->rounds = 1;
    g_free(pl->prev_tail);
    pl->prev_tail = g_steal_pointer(&tail);

    if (pl->rounds < CDB_ROUND_MIN)
        return G_SOURCE_CONTINUE;
    g_free(pl->prev_tail);
    pl->prev_tail = NULL;

    /* Terminé. Capture (spécification Éric) : dump du terminal en RAM,
     * on garde les CDB_TAIL_LINES dernières lignes, puis on jette
     * tout. */
    {
        gchar    *full = bash_panel_text((guint)pl->tab);
        gchar    **lines = NULL;
        guint     n;
        GString   *acc = g_string_new(NULL);

        if (full != NULL)
            lines = g_strsplit(full, "\n", -1);
        n = lines != NULL ? g_strv_length(lines) : 0;

        /* retire les lignes vides finales puis le prompt de fin :
         * ligne = prompt exact → jetée ; prompt collé derrière une
         * sortie (regex sans ^) → rogné au match, le texte avant est
         * conservé. */
        while (n > 0 && lines[n - 1][strspn(lines[n - 1], " \r")] == '\0')
            n--;
        if (n > 0) {
            GMatchInfo *mi = NULL;

            if (g_regex_match(prompt_re, lines[n - 1], 0, &mi)) {
                int pos = 0;

                g_match_info_fetch_pos(mi, 0, &pos, NULL);
                lines[n - 1][pos] = '\0';
                if (lines[n - 1][strspn(lines[n - 1], " \r")] == '\0')
                    n--; /* la ligne ne portait que le prompt */
            }
            g_match_info_free(mi);
        }

        /* fenêtre : les CDB_TAIL_LINES dernières lignes du corps,
         * débarrassées du padding d'espaces de fin de ligne */
        {
            guint from = n > (guint)CDB_TAIL_LINES
                ? n - (guint)CDB_TAIL_LINES : 0;

            for (guint i = from; i < n; i++) {
                char *ln = lines[i];
                gsize len = strlen(ln);

                while (len > 0 && (ln[len - 1] == ' ' ||
                                   ln[len - 1] == '\r'))
                    ln[--len] = '\0';
                g_string_append_printf(acc, "%s\n", ln);
            }
        }
        bounded = g_string_free(acc, FALSE);

        g_strfreev(lines);
        g_free(full);
    }

    note = g_strdup_printf(
        "résultat de %s :\n%s",
        pl->tab_label,
        bounded != NULL && bounded[0] != '\0'
            ? bounded : "(aucune sortie)");
    cdb_poll_finish(pl, note);

    g_free(note);
    g_free(bounded);
    return G_SOURCE_REMOVE;
}

/* Réponse initiale reçue : gère 429 (retry), erreurs HTTP,
 * puis démarre la lecture du flux SSE. */
static void
llm_send_done(GObject *source, GAsyncResult *res, gpointer data)
{
    LlmRequest   *req = data;
    LlmTile      *t = req->tile;
    GError       *error = NULL;
    GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source),
                                                    res, &error);

    if (error != NULL) {
        gboolean cancelled = g_error_matches(error, G_IO_ERROR,
                                             G_IO_ERROR_CANCELLED);

        /* Annulation utilisateur : silencieuse (la réponse partielle
         * déjà affichée reste dans le fil). */
        if (!cancelled) {
            hist_append(t, "\n[erreur : ");
            hist_append(t, error->message);
            hist_append(t, "]\n");
        }
        g_error_free(error);
        llm_busy_set(t, FALSE);
        llm_request_free(req);
        return;
    }
    {
        guint status = soup_message_get_status(req->msg);

        /* Erreurs transitoires : 429 (rate limit, rythme rapide) et
         * 500-504 (upstream indisponible, rythme lent — un serveur en
         * 503 a besoin de temps, pas d'insistance). Chacune sa config
         * harness ; 505+ exclu (réessayer est inutile). */
        if (status == 429 ||
            (status >= 500 && status <= 504)) {
            gboolean    is_429 = status == 429;
            gboolean    infinite;
            int         max_retries, delay_ms;
            gboolean    retry_on;

            if (stream != NULL) {
                g_object_unref(stream); /* corps d'erreur consommé */
                /* CRUCIAL : si les retries sont épuisés plus bas, on
                 * CHUTE vers le bloc `status != 200` — sans cette mise
                 * à NULL, il relirait puis re-libérerait le flux déjà
                 * mort (use-after-free + double free : crash
                 * « malloc(): unaligned tcache » constaté). */
                stream = NULL;
            }

            if (is_429) {
                LlmRetry429 rc;

                llm_retry429_load(&rc);
                retry_on = rc.retry;
                max_retries = rc.max_retries;
                delay_ms = rc.delay_ms;
            } else {
                LlmRetry5xx rc;

                llm_retry5xx_load(&rc);
                retry_on = rc.retry;
                max_retries = rc.max_retries;
                delay_ms = rc.delay_ms;
            }
            infinite = max_retries == 0;

            if (retry_on && (infinite || req->attempt < max_retries)) {
                req->attempt++;
                /* Réassemblage SSE repart à zéro pour l'essai suivant. */
                g_string_truncate(req->pending, 0);
                if (req->attempt == 1) {
                    char *note = g_strdup_printf(
                        "\n[CDB] HTTP %u — nouvelles tentatives en "
                        "cours…\n", status);

                    hist_append(t, note);
                    g_free(note);
                }
                g_timeout_add((guint)delay_ms, llm_retry_tick, req);
                return; /* busy reste actif ; req vit pour l'essai suivant */
            }
        }

        if (status != 200) {
            char msg[128];
            char err_body[1024] = "";
            gsize nerr = 0;

            /* Corps d'erreur (message JSON d'OpenRouter) pour diagnostic. */
            if (stream != NULL)
                g_input_stream_read_all(stream, err_body,
                                        sizeof(err_body) - 1, &nerr, NULL,
                                        NULL);
            g_snprintf(msg, sizeof(msg), "\n[HTTP %u] %.*s\n", status,
                       (int)nerr, err_body);
            hist_append(t, msg);
            if (stream != NULL)
                g_object_unref(stream);
            llm_busy_set(t, FALSE);
            llm_request_free(req);
            return;
        }
    }
    req->stream = stream; /* transfert : libéré par llm_request_free */
    g_input_stream_read_async(req->stream, req->scratch,
                              sizeof(req->scratch), G_PRIORITY_DEFAULT,
                              req->tile->cancel, llm_stream_read, req);
}

/* Un essai d'envoi : reconstruit un SoupMessage neuf depuis la
 * requête stockée (la session possède le message après send_async,
 * donc chaque tentative repart d'une instance fraîche). */
static void
llm_send_attempt(LlmRequest *req)
{
    SoupMessage *msg = soup_message_new("POST", req->url);

    if (req->auth != NULL)
        soup_message_headers_append(soup_message_get_request_headers(msg),
                                    "Authorization", req->auth);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Accept", "text/event-stream");

    soup_message_set_request_body_from_bytes(
        msg, "application/json",
        g_bytes_new_take((guint8 *)g_strdup(req->body),
                         strlen(req->body)));

    llm_busy_set(req->tile, TRUE); /* icône pause = annuler */
    req->msg = msg;
    soup_session_send_async(req->tile->soup, msg, G_PRIORITY_DEFAULT,
                            req->tile->cancel, llm_send_done, req);
}

static gboolean
llm_retry_tick(gpointer data)
{
    llm_send_attempt((LlmRequest *)data);
    return G_SOURCE_REMOVE;
}

/* Voix CDB dans le fil : rendu + historique. SANS avance de boucle. */
static void
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

/* Livraison immédiate (décisions d'Éric, malformations) puis avance. */
static void
llm_cdb_deliver(LlmTile *t, const char *text)
{
    hist_cdb_say(t, text);
    llm_cdb_next(t);
}

/* Demande d'approbation : rangée de boutons DANS le fil (ancre). */
static void
llm_cdb_ask(LlmTile *t, int tab, const char *cmd)
{
    GtkTextIter         end;
    GtkTextChildAnchor *anch;
    GtkWidget          *hbar;
    GtkWidget          *b_ok;
    GtkWidget          *b_no;
    CdbApproval        *a = g_new0(CdbApproval, 1);

    a->t = t;
    a->cmd = g_strdup(cmd);
    a->tab = tab;

    gtk_text_buffer_get_end_iter(t->hist, &end);
    anch = gtk_text_buffer_create_child_anchor(t->hist, &end);

    hbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b_ok = gtk_button_new_with_label("Exécuter");
    b_no = gtk_button_new_with_label("Refuser");
    a->b_ok = b_ok;
    a->b_no = b_no;
    gtk_widget_add_css_class(b_ok, "flat");
    gtk_widget_add_css_class(b_no, "flat");
    gtk_widget_set_focusable(b_ok, FALSE);
    gtk_widget_set_focusable(b_no, FALSE);
    g_signal_connect(b_ok, "clicked",
                     G_CALLBACK(on_cdb_approve_clicked), a);
    g_signal_connect(b_no, "clicked",
                     G_CALLBACK(on_cdb_refuse_clicked), a);
    g_signal_connect(hbar, "destroy", G_CALLBACK(cdb_approval_destroy), a);
    gtk_box_append(GTK_BOX(hbar), b_ok);
    gtk_box_append(GTK_BOX(hbar), b_no);
    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(t->hist_view),
                                      hbar, anch);
}

/* Ouvre un nouveau tour de réponse du modèle : en-tête acteur, buffer
 * de réponse remis à zéro, marque de streaming déplacée en fin de fil.
 * INDISPENSABLE à la boucle agentique : sans remise à zéro, t->reply
 * ré-accumule les réponses précédentes et llm_agent_detect re-compte
 * indéfiniment les mêmes commandes /CDB::. Le déplacement de la marque
 * évite aussi que hist_update_reply n'écrase les résultats CDB déjà
 * livrés entre deux tours. */
static void
llm_turn_new(LlmTile *t)
{
    GtkTextIter end;

    hist_render_actor_header(t, LLMACTOR_LLM);
    g_string_truncate(t->reply, 0);
    md_thinking_reset(t->hist);
    t->rendered_len = 0; /* nouveau tour : rendu incrémental repart à zéro */
    t->in_reasoning = FALSE;
    gtk_text_buffer_get_end_iter(t->hist, &end);
    if (t->reply_mark == NULL)
        t->reply_mark = gtk_text_buffer_create_mark(t->hist, NULL,
                                                    &end, TRUE);
    else
        gtk_text_buffer_move_mark(t->hist, t->reply_mark, &end);
}

/* Re-interrogation du modèle après livraison des résultats. L'ouverture
 * du tour neuf est faite par llm_send elle-même : chaque départ de
 * requête réinitialise t->reply — c'est LE correctif du re-comptage
 * infini des commandes /CDB::. */
static void
llm_cdb_requery(LlmTile *t)
{
    llm_send(t, NULL);
}

/* Loi d'Éric (anti-spam) : quand la file de commandes se vide, un
 * résultat antérieur contenu à 100 % dans un résultat plus récent du
 * même bash est jeté — seule la version la plus longue est livrée.
 * Aucune perte : la capture lit tout le buffer, donc le plus récent
 * inclut fatalement les précédents du même terminal. */
static void
llm_cdb_results_flush(LlmTile *t)
{
    GQueue   *q = t->cdb_results;
    guint     n;
    gboolean *drop;
    guint     i = 0;

    /* Boucle annulée par l'utilisateur : plus de re-requête. Les
     * résultats tardifs d'un poll bash encore actif sont jetés. */
    if (!t->busy) {
        if (q != NULL) {
            for (GList *l = q->head; l != NULL; l = l->next) {
                CdbResult *r = l->data;

                g_free(r->label);
                g_free(r->text);
                g_free(r);
            }
            g_queue_free(q);
            t->cdb_results = NULL;
        }
        return;
    }

    t->cdb_results = NULL;
    if (q == NULL) {
        llm_cdb_requery(t);
        return;
    }
    if (g_queue_is_empty(q)) {
        g_queue_free(q);
        llm_cdb_requery(t);
        return;
    }

    n = g_queue_get_length(q);
    drop = g_new0(gboolean, n);
    for (GList *li = q->head; li != NULL; li = li->next, i++) {
        CdbResult *ri = li->data;
        GList     *lj = li->next;

        while (lj != NULL && !drop[i]) {
            CdbResult *rj = lj->data;

            if (g_strcmp0(ri->label, rj->label) == 0 &&
                strstr(rj->text, ri->text) != NULL)
                drop[i] = TRUE;
            lj = lj->next;
        }
    }

    i = 0;
    for (GList *l = q->head; l != NULL; l = l->next, i++) {
        CdbResult *r = l->data;

        if (!drop[i])
            hist_cdb_say(t, r->text);
        g_free(r->label);
        g_free(r->text);
        g_free(r);
    }
    g_free(drop);
    g_queue_free(q);

    llm_cdb_requery(t);
}

/* Avance la file : commande suivante → approbation ; vide →
 * livraison des résultats pendants (dédupliqués), puis
 * re-interrogation du modèle. */
static void
llm_cdb_next(LlmTile *t)
{
    if (t->cmd_queue == NULL || g_queue_is_empty(t->cmd_queue)) {
        llm_cdb_results_flush(t);
        return;
    }
    CdbCmdSpec *s = g_queue_pop_head(t->cmd_queue);

    llm_cdb_ask(t, s->tab, s->cmd);
    g_free(s->cmd);
    g_free(s);
}

/* Texte reply sans les blocs thinking : le modèle y rédige souvent des
 * brouillons de commandes qu'il ne faut ni exécuter ni condamner. Les
 * deux scans du protocole (détection + malformation) partent de là. */
static char *
llm_scan_text(const char *reply)
{
    static GRegex *think_re = NULL;

    if (think_re == NULL)
        /* DOTALL indispensable : le thinking s'étale sur des dizaines
         * de lignes ; sans lui, .*? s'arrête en fin de première ligne,
         * le tag fermant n'est jamais atteint (bug constaté : 3
         * exécutions). */
        think_re = g_regex_new("〔thinking〕.*?〔/thinking〕",
                               G_REGEX_DOTALL, 0, NULL);
    return g_regex_replace_literal(think_re, reply, -1, 0, " ", 0, NULL);
}

/* Y a-t-il une VRAIE malformation /CDB:: dans la réponse ? Une mention
 * du protocole dans la prose (ex. citation littérale « bash-N » avec le
 * N majuscule) n'est PAS une tentative — c'est du texte explicatif.
 * Deux vraies tentatives bâclées :
 * - type A : /CDB::bash-<digit> non immédiatement suivi du marqueur IN
 *   (::"//"CDB-IN"// — guillemets simples, N à deux chiffres…) ;
 * - type B : marqueur IN présent sans OUT après lui (réponse coupée ou
 *   gabarit incomplet) — sinon la commande resterait silencieuse. */
static gboolean
llm_cdb_malformed(const char *reply)
{
    static GRegex *re_a = NULL;
    static GRegex *re_b = NULL;
    GMatchInfo    *mi = NULL;
    gboolean      bad = FALSE;
    char          *scan;

    scan = llm_scan_text(reply);
    if (re_a == NULL)
        re_a = g_regex_new("/CDB::bash-(\\d)(?!::\"//\"CDB-IN\"//)",
                           0, 0, NULL);
    if (re_b == NULL)
        re_b = g_regex_new("/CDB::bash-(\\d)::\"//\"CDB-IN\"//"
                           "(?!.*?//\"CDB-OUT\"//)",
                           G_REGEX_DOTALL, 0, NULL);
    if (g_regex_match(re_a, scan != NULL ? scan : reply, 0, &mi))
        bad = TRUE;
    g_match_info_free(mi);
    if (!bad && g_regex_match(re_b, scan != NULL ? scan : reply, 0, &mi))
        bad = TRUE;
    g_match_info_free(mi);
    g_free(scan);
    return bad;
}

/* Détection des commandes /CDB:: d'une réponse. Protocole à 3 champs :
 * /CDB::bash-N::"//"CDB-IN"//commande//"CDB-OUT"//"
 * La commande est prise BRUTE entre marqueurs — guillemets doubles et
 * sauts de ligne y sont autorisés (heredocs, scripts inline) ; la
 * pagination se fait dans la commande (head/tail/sed). TOUTES les
 * commandes valides partient en file.
 *
 * Anti-double-détection : on scanne le texte SANS les blocs thinking
 * (voir llm_scan_text), et on saute tout doublon exact déjà présent
 * dans la file (même bash + même commande). */
static gboolean
llm_agent_detect(LlmTile *t, const char *reply)
{
    static GRegex *re = NULL;
    GMatchInfo    *mi = NULL;
    gboolean      found = FALSE;
    char          *scan;

    if (re == NULL)
        re = g_regex_new("/CDB::bash-(\\d)::\"//\"CDB-IN\"//(.*?)"
                         "//\"CDB-OUT\"//\"", G_REGEX_DOTALL, 0, NULL);
    scan = llm_scan_text(reply);
    if (g_regex_match(re, scan != NULL ? scan : reply, 0, &mi)) {
        found = TRUE;
        do {
            CdbCmdSpec *s;
            char       *cmd = g_match_info_fetch(mi, 2);
            char       *tabstr = g_match_info_fetch(mi, 1);
            int         tab = atoi(tabstr);
            gboolean    dup = FALSE;

            g_free(tabstr);

            /* Doublon exact déjà en file (ou en cours) : on ignore. */
            if (t->cmd_queue != NULL)
                for (GList *l = t->cmd_queue->head; l != NULL; l = l->next) {
                    CdbCmdSpec *q = l->data;

                    if (q->tab == tab && g_strcmp0(q->cmd, cmd) == 0) {
                        dup = TRUE;
                        break;
                    }
                }
            if (!dup) {
                s = g_new0(CdbCmdSpec, 1);

                s->tab = tab;
                s->cmd = cmd;
                if (t->cmd_queue == NULL)
                    t->cmd_queue = g_queue_new();
                g_queue_push_tail(t->cmd_queue, s);
            } else
                g_free(cmd);
        } while (g_match_info_next(mi, NULL));
    }
    g_match_info_free(mi);
    g_free(scan);

    if (found)
        llm_cdb_next(t);

    return found;
}

/* Construit le body chat/completions TEL qu'il serait envoyé à
 * l'instant T : persona re-résolu avec le projet courant + tout le
 * fil non-local. String g_strdup (à g_free). Utilisé par llm_send()
 * ET par le menu slots (voir / sauvegarder) — garantie d'identité
 * octet pour octet avec la requête réseau. */
static char *
llm_body_build(LlmTile *t)
{
    JsonBuilder *builder;
    JsonNode    *root_node;
    char        *out;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, t->cfg->model);
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);

    /* [0] Persona CDB (projet courant résolu à CHAQUE envoi). */
    {
        char *persona = llm_persona_load(t);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "role");
        json_builder_add_string_value(builder, "system");
        json_builder_set_member_name(builder, "content");
        json_builder_add_string_value(builder, persona);
        json_builder_end_object(builder);
        g_free(persona);
    }

    /* Le fil des trois acteurs. */
    for (guint i = 0; i < t->history->len; i++) {
        LlmMsg     *m = &g_array_index(t->history, LlmMsg, i);
        const char *wire = llm_msg_wire_role(m->actor);

        if (m->local)
            continue;
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "role");
        json_builder_add_string_value(builder, wire);
        if (m->actor == LLMACTOR_CDB) {
            char *framed = g_strdup_printf("[CDB] %s", m->content);

            json_builder_set_member_name(builder, "content");
            json_builder_add_string_value(builder, framed);
            g_free(framed);
        } else {
            json_builder_set_member_name(builder, "content");
            json_builder_add_string_value(builder, m->content);
        }
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    root_node = json_builder_get_root(builder);
    out = json_to_string(root_node, FALSE);
    json_node_unref(root_node);
    g_object_unref(builder);
    return out;
}

/* Construit et envoie la requête chat/completions (stream=true).
 * Ouvre un NOUVEAU tour de réponse : en-tête acteur, t->reply remis à
 * zéro, marque de streaming déplacée en fin de fil. Sans cette remise
 * à zéro, la boucle agentique ré-accumulait les réponses précédentes
 * dans t->reply et llm_agent_detect redétectait indéfiniment les MÊMES
 * commandes /CDB:: (comptage infini, exécutions multiples). */
static void
llm_send(LlmTile *t, const char G_GNUC_UNUSED *prompt)
{
    LlmRequest *req = g_new0(LlmRequest, 1);

    req->pending = g_string_new(NULL); /* réassemblage des lignes SSE */
    llm_turn_new(t);
    /* Cancellable FRAIS par requête : l'ancien peut rester dans l'état
     * « annulé » (un GCancellable annulé le reste). Flag stop aussi. */
    t->stop_requested = FALSE;
    if (t->cancel != NULL)
        g_object_unref(t->cancel);
    t->cancel = g_cancellable_new();
    t->cur_req = req;
    req->tile = t;
    req->attempt = 0;
    req->url = g_strdup_printf("%s/chat/completions", t->cfg->api_url);

    if (t->cfg->api_key != NULL && t->cfg->api_key[0] != '\0')
        req->auth = g_strdup_printf("Bearer %s", t->cfg->api_key);

    /* Le body est construit par llm_body_build (même code que la vue
     * slots) puis persisté dans last.json : trace exacte de l'envoi. */
    req->body = llm_body_build(t);
    llm_slots_last_save(req->body);
    t->turns_since_ref++;
    llm_slots_title_update(t);

    llm_send_attempt(req);
}

/* Texte de la saisie multi-lignes (g_strdup, vide = ""). */
static char *
llm_entry_text(LlmTile *t)
{
    GtkTextIter start, end;

    gtk_text_buffer_get_start_iter(t->entry_buf, &start);
    gtk_text_buffer_get_end_iter(t->entry_buf, &end);
    return gtk_text_buffer_get_text(t->entry_buf, &start, &end, FALSE);
}

/* Vide la saisie et ramène la zone à une ligne. */
static void
llm_entry_clear(LlmTile *t)
{
    gtk_text_buffer_set_text(t->entry_buf, "", -1);
}

/* Hauteur de la saisie : nb de lignes du buffer, borné [1..8]. Le
 * scrolled-window reçoit cette hauteur ; au-delà de 8 lignes il scrolle
 * en interne. Recalculé à chaque changement du buffer. */
#define CDB_ENTRY_MAX_LINES 8
static void
llm_entry_resize(LlmTile *t)
{
    int n = gtk_text_buffer_get_line_count(t->entry_buf);
    int lines = n < 1 ? 1 : (n > CDB_ENTRY_MAX_LINES ? CDB_ENTRY_MAX_LINES : n);

    /* ~19 px par ligne : hauteur de ligne + padding (mesuré GTK défaut). */
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(t->entry_scroll), lines * 19 + 12);
}

/* ------------------------------------------------------------------ */
/* Persistance slots (JSON envoyé)                                    */
/* ------------------------------------------------------------------ */

/* Fenêtre top-level de la tuile (parent des dialogues). */
static GtkWindow *
tile_window(LlmTile *t)
{
    GtkRoot *root = gtk_widget_get_root(t->view);

    return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

/* Filtre insert-text : chiffres uniquement (sélecteurs 0-999). */
static void
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

typedef struct {
    GtkWindow *dialog;
    int        result; /* -1 = annulé / invalide */
} NumPickCtx;

static void
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

static void
on_num_pick_cancel(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    NumPickCtx *ctx = data;

    ctx->result = -1;
    gtk_window_destroy(GTK_WINDOW(ctx->dialog));
}

static void
on_num_pick_activate(GtkEntry G_GNUC_UNUSED *e, gpointer data)
{
    on_num_pick_ok(NULL, data);
}

/* Dialogue BLOQUANT : un numéro 0-999. Renvoie -1 si annulé. */
static int
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

/* Dialogue BLOQUANT de confirmation. `destructive` style le bouton OK. */
typedef struct {
    GtkWindow *dialog;
    gboolean   ok;
} ConfirmCtx;

static void
on_confirm_yes(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    ConfirmCtx *ctx = data;

    ctx->ok = TRUE;
    gtk_window_destroy(ctx->dialog);
}

static void
on_confirm_no(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    ConfirmCtx *ctx = data;

    gtk_window_destroy(ctx->dialog);
}

static gboolean
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

/* Bouton « Copier » de la modale Voir : presse-papier = JSON BRUT
 * (pas la version pretty). */
static void
on_view_copy_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    GtkWindow    *win = data;
    const char   *raw = g_object_get_data(G_OBJECT(win), "raw-json");

    if (raw == NULL)
        return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(win)), raw);
}

/* 1 — Voir le JSON tel qu'il serait envoyé à l'instant T (Q2 : body
 * reconstruit live, identique octet pour octet à l'envoi). */
static void
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

/* 2 — Sauvegarder le body courant (llm_body_build) dans un slot. */
static void
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
        hist_cdb_announce(t, msg);
    } else {
        msg = g_strdup_printf("Échec de la sauvegarde du slot %d.", slot);
        hist_cdb_announce(t, msg);
    }
    g_free(msg);
    g_free(body);
}

/* Vide t->history (les contenus) sans toucher au GArray. */
static void
llm_history_wipe(LlmTile *t)
{
    for (guint i = 0; i < t->history->len; i++) {
        LlmMsg *m = &g_array_index(t->history, LlmMsg, i);

        g_free(m->content);
    }
    g_array_set_size(t->history, 0);
}

/* Purge les files /CDB:: (elles référençaient l'ancien fil). */
static void
llm_queues_purge(LlmTile *t)
{
    if (t->cmd_queue != NULL) {
        for (GList *l = t->cmd_queue->head; l != NULL; l = l->next) {
            CdbCmdSpec *s = l->data;

            g_free(s->cmd);
            g_free(s);
        }
        g_queue_free(t->cmd_queue);
        t->cmd_queue = NULL;
    }
    if (t->cdb_results != NULL) {
        for (GList *l = t->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            g_free(r->label);
            g_free(r->text);
            g_free(r);
        }
        g_queue_free(t->cdb_results);
        t->cdb_results = NULL;
    }
}

/* 3 — Charger un slot : IMPERSONNALISATION — le fil d'avant n'existe
 * plus. Wipe complet (history, vue, saisie, files /CDB::, état de
 * streaming) puis repeuplement depuis le tableau messages :
 *   system [0]   → sauté (persona ré-injecté live à chaque envoi) ;
 *   system autre → CDB, préfixe « [CDB] » retiré (fidélité du
 *                  round-trip : llm_body_build le rajoutera) ;
 *   assistant    → LLM (rendu markdown) ;
 *   user/autre   → USER. */
static void
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
        hist_cdb_announce(t, msg);
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
        hist_cdb_announce(t, msg);
        g_free(msg);
        return;
    }
    g_free(json);
    root = json_node_get_object(json_parser_get_root(parser));
    msgs = root != NULL ? json_object_get_array_member(root, "messages")
                        : NULL;
    if (msgs == NULL) {
        g_object_unref(parser);
        hist_cdb_announce(t, "pas de tableau « messages » dans ce slot.");
        return;
    }

    /* --- Wipe complet : comme si le fil n'avait jamais existé. --- */
    llm_history_wipe(t);
    llm_queues_purge(t);
    gtk_text_buffer_get_bounds(t->hist, &start, &end);
    gtk_text_buffer_delete(t->hist, &start, &end);
    md_thinking_reset(t->hist);
    g_string_truncate(t->reply, 0);
    t->rendered_len = 0;
    t->in_reasoning = FALSE;
    if (t->reply_mark != NULL) {
        gtk_text_buffer_get_end_iter(t->hist, &end);
        gtk_text_buffer_move_mark(t->hist, t->reply_mark, &end);
    }
    llm_entry_clear(t);
    t->cdb_retries = 0;

    /* --- Repeuplement + rendu du fil importé. --- */
    n = json_array_get_length(msgs);
    for (guint i = 0; i < n; i++) {
        JsonObject *m = json_array_get_object_element(msgs, i);
        const char *role = m != NULL
                           ? json_object_get_string_member(m, "role") : NULL;
        const char *content = m != NULL
                              ? json_object_get_string_member(m, "content")
                              : NULL;
        GtkTextIter eit;

        if (content == NULL)
            continue;
        if (g_strcmp0(role, "system") == 0) {
            if (i == 0)
                continue; /* persona : ré-injecté live à chaque envoi */
            if (g_str_has_prefix(content, "[CDB] "))
                content += 6; /* retire le cadre (rajouté à l'envoi) */
            history_push(t, LLMACTOR_CDB, FALSE, content);
            hist_render_actor_header(t, LLMACTOR_CDB);
            hist_ensure_voice_tags(t);
            gtk_text_buffer_get_end_iter(t->hist, &eit);
            gtk_text_buffer_insert_with_tags_by_name(
                t->hist, &eit, content, -1, "voice-cdb", NULL);
            hist_append(t, "\n");
        } else if (g_strcmp0(role, "assistant") == 0) {
            history_push(t, LLMACTOR_LLM, FALSE, content);
            hist_render_actor_header(t, LLMACTOR_LLM);
            gtk_text_buffer_get_end_iter(t->hist, &eit);
            md_insert(t->hist, &eit, content); /* rendu markdown */
            hist_append(t, "\n");
        } else {
            /* user + tout rôle inconnu. */
            history_push(t, LLMACTOR_USER, FALSE, content);
            hist_render_actor_header(t, LLMACTOR_USER);
            hist_append(t, content);
            hist_append(t, "\n");
        }
    }
    g_object_unref(parser);
    llm_scroll_to_end(t);

    t->slot_origin = slot;
    t->turns_since_ref = 0;
    {
        char *b = llm_body_build(t);
        t->ref_body_size = (b != NULL) ? strlen(b) : 0;
        g_free(b);
    }
    llm_slots_title_update(t);

    msg = g_strdup_printf("Slot %d chargé — le fil a été remplacé.", slot);
    hist_cdb_announce(t, msg);
    g_free(msg);
}

/* 4 — Vider un slot. */
static void
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
        hist_cdb_announce(t, msg);
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
    hist_cdb_announce(t, msg);
    g_free(msg);
}

/* 5 — Importer un slot d'une autre session (3 champs : session
 * source, slot source, slot cible). Erreurs inline, dialogue reste
 * ouvert tant que non validé. */
typedef struct {
    GtkWindow *dialog;
    GtkWidget *e_session;
    GtkWidget *e_src;
    GtkWidget *e_dst;
    GtkWidget *err;
    gboolean   attempted; /* OK cliqué au moins une fois (vs annuler) */
    gboolean   done;
    int        dst_slot;
} ImportCtx;

static int
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

static void
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

static void
on_import_cancel(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    ImportCtx *ctx = data;

    gtk_window_destroy(ctx->dialog);
}

static void
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

        hist_cdb_announce(t, msg);
        g_free(msg);
    } else if (ctx.attempted) {
        hist_cdb_announce(t, "échec de l'import.");
    }
}

/* 6 — Vider le chat actuel : wipe complet du fil vivant (historique,
 * vue, saisie, files CDB, état de streaming). */
static void
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
    llm_queues_purge(t);
    gtk_text_buffer_get_bounds(t->hist, &start, &end);
    gtk_text_buffer_delete(t->hist, &start, &end);
    md_thinking_reset(t->hist);
    g_string_truncate(t->reply, 0);
    t->rendered_len = 0;
    t->in_reasoning = FALSE;
    if (t->reply_mark != NULL) {
        gtk_text_buffer_get_end_iter(t->hist, &end);
        gtk_text_buffer_move_mark(t->hist, t->reply_mark, &end);
    }
    llm_entry_clear(t);
    t->cdb_retries = 0;
    t->slot_origin = -1;
    t->turns_since_ref = 0;
    {
        char *b = llm_body_build(t);
        t->ref_body_size = (b != NULL) ? strlen(b) : 0;
        g_free(b);
    }
    llm_slots_title_update(t);

    hist_cdb_announce(t, "chat actuel vidé.");
}

/* Formate une taille en octets / Ko / Mo. */
static char *
llm_slots_size_str(gsize bytes)
{
    if (bytes < 1024)
        return g_strdup_printf("%zu o", bytes);
    if (bytes < 1024 * 1024)
        return g_strdup_printf("%.1f Ko", bytes / 1024.0);
    return g_strdup_printf("%.1f Mo", bytes / (1024.0 * 1024.0));
}

/* Met à jour le titre d'état du popover slots. */
static void
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

/* Popover slots ouvert : rafraîchit le titre d'état. */
static void
on_slots_pop_mapped(GtkWidget G_GNUC_UNUSED *w, gpointer data)
{
    llm_slots_title_update(data);
}

/* Dispatch commun des actions de persistance du menu slots. */
static void
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

/* Item du popover slots : ferme le popover puis exécute l'action. */
static void
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

static void
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
    if (prompt[0] == '\0') {
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

    hist_render_actor_header(t, LLMACTOR_USER);
    hist_append(t, prompt);
    /* llm_send ouvre lui-même le tour (llm_turn_new) : pas d'appel ici,
     * sinon l'en-tête « Claude » serait rendu deux fois. */
    llm_entry_clear(t);
    t->cdb_retries = 0; /* nouveau tour : compteur malformations reset */
    history_push(t, LLMACTOR_USER, FALSE, prompt);
    llm_send(t, prompt);
    llm_scroll_to_end(t);
    g_free(prompt); /* copie : l'entry a été vidée */
}

/* Garde la vue collée en bas pendant/après le streaming — sauf si
 * l'utilisateur a remonté (il relit une zone : on ne le vole pas).
 * Le suivi reprend dès qu'il revient en bas.
 * Mécanique : set_value(upper - page) sur l'adjustment. Contrairement
 * à scroll_to_mark(yalign=1.0), rien d'asynchrone, aucun déplacement
 * du curseur d'insertion (les sélections survivent), et le geste en
 * cours de l'utilisateur est naturellement respecté par GTK. */
static void
llm_scroll_to_end(LlmTile *t)
{
    double upper, page;

    if (!t->follow || t->adj == NULL)
        return;
    upper = gtk_adjustment_get_upper(t->adj);
    page = gtk_adjustment_get_page_size(t->adj);
    gtk_adjustment_set_value(t->adj, upper - page);
}

/* Le user a scrolle : suivi auto seulement s'il est (presque) en bas. */
static void
on_llm_scroll(GtkAdjustment *adj, gpointer data)
{
    LlmTile *t = data;
    double   val = gtk_adjustment_get_value(adj);
    double   upper = gtk_adjustment_get_upper(adj);
    double   page = gtk_adjustment_get_page_size(adj);

    t->follow = (val + page >= upper - 20.0);
}

/* Home/End dans l'historique : scroll haut/bas, sans déplacer le
 * curseur ni casser une sélection. La phase CAPTURE intercepte la
 * touche avant les bindings par défaut de GtkTextView. */
static gboolean
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

/* Détachement FRANC : dès qu'Éric touche la scrollbar, le suivi auto
 * est coupé — pas de course entre le geste et les rappels du stream
 * (avec un long contexte, le seuil de 20 px seul laisse le collage
 * arracher la vue vers le bas pendant les premiers pixels du geste). */
static void
on_scrollbar_pressed(GtkGestureClick G_GNUC_UNUSED *g,
                     int G_GNUC_UNUSED n,
                     double G_GNUC_UNUSED x, double G_GNUC_UNUSED y,
                     gpointer data)
{
    LlmTile *t = data;

    t->follow = FALSE;
}

/* Le buffer de saisie a changé : recalcule la hauteur de la zone. */
static void
on_llm_entry_changed(GtkTextBuffer G_GNUC_UNUSED *buf, gpointer data)
{
    llm_entry_resize(data);
}

GtkWidget *
llm_tile_new(const LlmConfig *cfg, GActionGroup *actions,
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

    t->soup = soup_session_new();
    /* Anti-hang : pas de données pendant 120 s = abandon. */
    g_object_set(t->soup, "timeout", 120, "idle-timeout", 180, NULL);
    t->reply = g_string_new("");
    t->history = g_array_new(FALSE, FALSE, sizeof(LlmMsg));
    t->slot_origin = -1;
    g_object_set_data_full(G_OBJECT(box), "cdb-llm-tile", t, llm_tile_free);
    return box;
}
