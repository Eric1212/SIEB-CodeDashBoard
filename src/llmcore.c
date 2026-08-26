/*
 * llmcore.c : etat conversationnel LLM (LlmCore) — reseau SSE,
 * historique, boucle agentique /CDB::, retries 429/5xx, annonces.
 *
 * Requete : POST {api_url}/chat/completions, stream=true.
 * Reponse : SSE data: {...} ; fin par data: [DONE].
 * Le core vit sans vue : les tuiles (llmtile.c) miroitent la
 * meme conversation (buffers par vue + diffusion).
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

/* ------------------------------------------------------------------ */
/* Config                                                             */
/* ------------------------------------------------------------------ */

char *
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

static gboolean    md_started = FALSE;
GHashTable *md_names = NULL;
GSList     *md_pending = NULL;



/* Complète les noms manquants depuis le cache models.dev. */
void
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

void
md_deliver(ModelsFetch *f, LlmModelInfo *models)
{
    md_enrich(models, f->provider);
    f->cb(models, f->user_data);
    llm_models_free(models);
    g_free(f->provider);
    g_object_unref(f->soup);
    g_free(f);
}

MdPending *
md_deferred_new(ModelsFetch *f, LlmModelInfo *models)
{
    MdPending *p = g_new0(MdPending, 1);

    p->f = f;
    p->models = models;
    return p;
}

/* Réception de api.json : construit le cache <provider, slug→name>. */
void
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
        g_printerr("CDB: models.dev échoué : %s\n", err->message);
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

void
md_load_start(void)
{
    SoupSession *soup = soup_session_new();
    SoupMessage *msg =
        soup_message_new("GET", "https://models.dev/api.json");

    /* Le message appartient à la session après l'appel. */
    soup_session_send_and_read_async(soup, msg, G_PRIORITY_DEFAULT,
                                     NULL, md_load_done, NULL);
}

void
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
        g_printerr("CDB: /models échoué : %s\n", err->message);
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
JsonObject *
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
        g_printerr("CDB: écriture allowed_models : %s\n", error->message);
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
            g_printerr("CDB: écriture retry429 : %s\n", error->message);
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
            g_printerr("CDB: écriture retry5xx : %s\n", error->message);
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
            g_printerr("CDB: écriture switch active : %s\n",
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

/* Un échange de l'historique de conversation.
 * local = TRUE : affiché dans le fil mais JAMAIS envoyé au modèle
 * (annonces CDB : erreurs HTTP, changements d'état…). */

const char *
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

/* ------------------------------------------------ */
/* Persona CDB (prompts/default.txt par session)     */
/* ------------------------------------------------ */

/* Remplace toutes les occurrences de old par new dans s. */
char *
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
        g_printerr("CDB: écriture prompts/default.txt : %s\n",
                   error->message);
        g_error_free(error);
    }
    g_free(dir);
    g_free(path);
}

void
history_push_images(LlmTile *t, LlmActor actor, gboolean local,
                    const char *content, GPtrArray *images)
{
    LlmMsg m;

    m.actor = actor;
    m.local = local;
    m.content = g_strdup(content);
    m.images = images; /* transfert de propriété */

    g_array_append_vals(t->core->history, &m, 1);
}

void
history_push(LlmTile *t, LlmActor actor, gboolean local, const char *content)
{
    history_push_images(t, actor, local, content, NULL);
}

/* Lit un entier JSON en tolérant int64/double/chaîne numérique. */
long
llm_json_int(JsonObject *obj, const char *member, long fallback)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, member))
        return fallback;
    node = json_object_get_member(obj, member);
    if (!JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    switch (json_node_get_value_type(node)) {
    case G_TYPE_INT64:
        return (long)json_node_get_int(node);
    case G_TYPE_DOUBLE:
        return (long)json_node_get_double(node);
    case G_TYPE_STRING:
        return strtol(json_node_get_string(node), NULL, 10);
    default:
        return fallback;
    }
}

/* Traite une ligne SSE « data: … ». */
void
llm_handle_sse_line(LlmCore *c, const char *line)
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
    /* Bilan tokens : par vue (bandeau propre à chacune). */
    for (guint vi = 0; c->views != NULL && vi < c->views->len; vi++) {
        LlmTile *t = g_ptr_array_index(c->views, vi);
        if (obj != NULL && json_object_has_member(obj, "usage")) {
        JsonObject *usage = json_object_get_object_member(obj, "usage");

        t->tokens_sent = llm_json_int(usage, "prompt_tokens",
                                      t->tokens_sent);
        t->tokens_received = llm_json_int(usage, "completion_tokens",
                                          t->tokens_received);
        t->tokens_context = llm_json_int(usage, "total_tokens",
                                         t->tokens_sent +
                                         t->tokens_received);
        t->tokens_estimated = FALSE;
        llm_status_update(t);
        }
    }

    if (obj != NULL && json_object_has_member(obj, "choices")
        && (choices = json_object_get_array_member(obj, "choices")) != NULL
        && json_array_get_length(choices) > 0
        && (choices0 = json_array_get_object_element(choices, 0)) != NULL
        && json_object_has_member(choices0, "delta")
        && (delta = json_object_get_object_member(choices0, "delta")) != NULL) {
        const char *piece = NULL;

        /* Contenu final ; en repli, le reasoning (thinking). */
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
                    if (!c->in_reasoning) {
                        g_string_append(c->reply, "〔thinking〕 ");
                        c->in_reasoning = TRUE;
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

            if (c->in_reasoning && is_content) {
                /* Transition thinking → contenu : tag de fermeture. */
                g_string_append(c->reply, " 〔/thinking〕\n\n");
                c->in_reasoning = FALSE;
            }
            g_string_append(c->reply, piece);
        }
    }

    /* Diffusion : chaque vue rattrape à son rythme (rendered_len). */
    for (guint vi = 0; c->views != NULL && vi < c->views->len; vi++) {
        LlmTile *t = g_ptr_array_index(c->views, vi);

        hist_update_reply(t);
    }
    g_object_unref(parser);
}




/* Clic sur le bouton média : play = envoyer, pause = annuler la
 * requête en cours (le flux se termine en erreur G_IO_ERROR_CANCELLED,
 * capturée silencieusement par les chemins de lecture). */
void
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
    t->core->stop_requested = TRUE;
    if (t->core->cancel != NULL)
        g_cancellable_cancel(t->core->cancel);
    if (t->core->cur_req != NULL && t->core->cur_req->stream != NULL)
        g_input_stream_close_async(t->core->cur_req->stream, G_PRIORITY_DEFAULT,
                                   NULL, NULL, NULL);

    /* 2. Boucle agentique en attente (approbation, exécution bash,
     * re-requête) : rien n'écoute le cancellable — on vide la file et
     * on rend la main. Les polls bash en cours se termineront mais leur
     * résultat ne déclenchera plus de re-requête (file vide → flush →
     * requery est court-circuité par busy=FALSE ci-dessous). */
    if (t->core->cmd_queue != NULL && !g_queue_is_empty(t->core->cmd_queue)) {
        for (GList *l = t->core->cmd_queue->head; l != NULL; l = l->next) {
            CdbCmdSpec *s = l->data;

            g_free(s->cmd);
            g_free(s);
        }
        g_queue_free(t->core->cmd_queue);
        t->core->cmd_queue = NULL;
        core_cdb_announce(t->core, "〔annulé〕 file de commandes vidée.");
    }
    /* 3. Résultats pendants non livrés : jetés (le user a dit stop). */
    if (t->core->cdb_results != NULL) {
        for (GList *l = t->core->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            g_free(r->label);
            g_free(r->text);
            g_free(r);
        }
        g_queue_free(t->core->cdb_results);
        t->core->cdb_results = NULL;
    }
    /* 4. Si aucun flux réseau n'était actif (attente approbation/poll),
     * personne ne remettra busy à FALSE : on le fait ici. Si un flux
     * était actif, son callback de fin le fera — double appel inoffensif. */
    for (guint vi = 0; vi < t->core->views->len; vi++)
        llm_busy_set(g_ptr_array_index(t->core->views, vi), FALSE);
}

/* Libère la requête une seule fois (les callbacks de complétion
 * peuvent arriver en double selon l'état du flux). */
void
llm_request_free(LlmRequest *req)
{
    /* La requête courante de la tuile meurt : plus rien à annuler. */
    if (req->core != NULL && req->core->cur_req == req)
        req->core->cur_req = NULL;
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
void
llm_process_bytes(LlmRequest *req, const char *bytes, gssize n)
{
    LlmCore *c = req->core;
    char    *nl;

    if ((gsize)n <= 0)
        return;
    g_string_append_len(req->pending, bytes, (gssize)n);
    while ((nl = strchr(req->pending->str, '\n')) != NULL) {
        gsize consumed = (gsize)(nl - req->pending->str) + 1;

        *nl = '\0';
        if (req->pending->str[0] != '\0')
            llm_handle_sse_line(c, req->pending->str);
        g_string_erase(req->pending, 0, consumed);
    }
}

/* Poussée d'historique au niveau CORE : fonctionne même sans vue
 * attachée (la conversation survit à la fermeture de la tuile). */
static void
core_history_push(LlmCore *c, LlmActor actor, gboolean local,
                  const char *content)
{
    LlmMsg m;

    m.actor = actor;
    m.local = local;
    m.content = g_strdup(content);
    m.images = NULL;
    g_array_append_vals(c->history, &m, 1);
    llm_live_save(c);
}


void
llm_stream_read(GObject G_GNUC_UNUSED *source, GAsyncResult *res,
                gpointer data)
{
    LlmRequest *req = data;
    LlmCore    *c = req->core;
    guint       vi;
    gssize      n;
    GError     *error = NULL;

    n = g_input_stream_read_finish(req->stream, res, &error);

    /* Pause cliquée : tout entrant est jeté, fin immédiate. */
    if (c->stop_requested) {
        if (error != NULL)
            g_error_free(error);
        core_history_push(c, LLMACTOR_LLM, FALSE, c->reply->str);
        for (vi = 0; vi < c->views->len; vi++) {
            LlmTile *v = g_ptr_array_index(c->views, vi);

            hist_flush_reply(v);
            llm_slots_title_update(v);
            hist_append(v, "\n〔annulé〕\n");
            llm_busy_set(v, FALSE);
        }
        llm_request_free(req);
        return;
    }

    if (error != NULL) {
        gboolean cancelled = g_error_matches(error, G_IO_ERROR,
                                             G_IO_ERROR_CANCELLED);

        if (cancelled) {
            g_error_free(error);
            core_history_push(c, LLMACTOR_LLM, FALSE, c->reply->str);
            for (vi = 0; vi < c->views->len; vi++) {
                LlmTile *v = g_ptr_array_index(c->views, vi);

                hist_flush_reply(v);
                llm_slots_title_update(v);
                hist_append(v, "\n〔annulé〕\n");
                llm_busy_set(v, FALSE);
            }
            llm_request_free(req);
            return;
        }
        for (vi = 0; vi < c->views->len; vi++)
            hist_append(g_ptr_array_index(c->views, vi), error->message);
        g_error_free(error);
        llm_request_free(req);
        return;
    }

    if (n <= 0) {
        core_history_push(c, LLMACTOR_LLM, FALSE, c->reply->str);
        for (vi = 0; vi < c->views->len; vi++) {
            LlmTile *v = g_ptr_array_index(c->views, vi);

            hist_flush_reply(v);
            llm_slots_title_update(v);
            hist_append(v, "\n");
        }

        if (llm_agent_detect(c, c->reply->str)) {
            c->cdb_retries = 0;
            llm_request_free(req);
            return;
        }
        if (strstr(c->reply->str, "/CDB::") != NULL &&
            llm_cdb_malformed(c->reply->str)) {
            if (c->cdb_retries < CDB_RETRY_MAX) {
                char *note;

                c->cdb_retries++;
                note = g_strdup_printf(
                    "COMMANDE MAL FORMÉE (tentative %d/%d) : "
                    "le protocole est exactement "
                    "/CDB::bash-N::\"//\"CDB-IN\"//COMMANDE"
                    "//\"CDB-OUT\"//\" — N entre 0 et 9, "
                    "commande complète entre les deux marqueurs "
                    "(les \" internes sont autorisés tels quels). "
                    "Réécris-la proprement.",
                    c->cdb_retries, CDB_RETRY_MAX);
                core_cdb_deliver(c, note);
                g_free(note);
                llm_request_free(req);
                return;
            }
            core_history_push(c, LLMACTOR_CDB, TRUE,
                "trois commandes mal formées d'affilée : j'abandonne "
                "cette boucle. Réponds en texte ou reformule entièrement.");
            for (guint vi = 0; vi < c->views->len; vi++)
                llm_cdb_say_display(g_ptr_array_index(c->views, vi),
                    "trois commandes mal formées d'affilée : j'abandonne "
                    "cette boucle. Réponds en texte ou reformule entièrement.");
        }

        for (vi = 0; vi < c->views->len; vi++)
            llm_busy_set(g_ptr_array_index(c->views, vi), FALSE);
        llm_request_free(req);
        return;
    }

    llm_process_bytes(req, req->scratch, n);
    g_input_stream_read_async(req->stream, req->scratch,
                              sizeof(req->scratch), G_PRIORITY_DEFAULT,
                              NULL, llm_stream_read, req);
}

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

void
cdb_poll_register(CdbPoll *pl)
{
    if (cdb_polls == NULL)
        cdb_polls = g_ptr_array_new();
    g_ptr_array_add(cdb_polls, pl);
}

void
cdd_poll_unregister(CdbPoll *pl)
{
    if (cdb_polls != NULL)
        g_ptr_array_remove_fast(cdb_polls, pl);
}



void
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
    if (pl->core->cdb_results == NULL)
        pl->core->cdb_results = g_queue_new();
    g_queue_push_tail(pl->core->cdb_results, r);

    llm_cdb_next(pl->core);

    g_free(pl->prev_tail);
    g_free(pl);
}

/* Attend que le shell d'un onglet fraîchement créé finisse son spawn
 * (PTY attaché), puis injecte la commande approuvée. Le poll normal ne
 * démarre qu'APRÈS l'injection : le prompt initial du shell ne peut plus
 * être confondu avec une fin de commande. */
gboolean
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

gboolean
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
void
llm_send_done(GObject *source, GAsyncResult *res, gpointer data)
{
    LlmRequest   *req = data;
    LlmCore      *c = req->core;
    guint       vi;
    GError       *error = NULL;
    GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source),
                                                    res, &error);

    if (error != NULL) {
        gboolean cancelled = g_error_matches(error, G_IO_ERROR,
                                             G_IO_ERROR_CANCELLED);

        if (!cancelled) {
            char *note = g_strdup_printf("\n[erreur : %s]\n",
                                         error->message);

            core_cdb_announce(c, note);
            g_free(note);
        }
        g_error_free(error);
        for (vi = 0; vi < c->views->len; vi++)
            llm_busy_set(g_ptr_array_index(c->views, vi), FALSE);
        llm_request_free(req);
        return;
    }
    {
        guint status = soup_message_get_status(req->msg);

        if (status == 429 ||
            (status >= 500 && status <= 504)) {
            gboolean    is_429 = status == 429;
            gboolean    infinite;
            int         max_retries, delay_ms;
            gboolean    retry_on;

            if (stream != NULL) {
                g_object_unref(stream);
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
                g_string_truncate(req->pending, 0);
                if (req->attempt == 1) {
                    char *note = g_strdup_printf(
                        "\n[CDB] HTTP %u — nouvelles tentatives en "
                        "cours…\n", status);

                    core_cdb_announce(c, note);
                    g_free(note);
                }
                g_timeout_add((guint)delay_ms, llm_retry_tick, req);
                return;
            }
        }

        if (status != 200) {
            char msg[128];
            char err_body[1024] = "";
            gsize nerr = 0;

            if (stream != NULL)
                g_input_stream_read_all(stream, err_body,
                                        sizeof(err_body) - 1, &nerr, NULL,
                                        NULL);
            g_snprintf(msg, sizeof(msg), "\n[HTTP %u] %.*s\n", status,
                       (int)nerr, err_body);
            core_cdb_announce(c, msg);
            if (stream != NULL)
                g_object_unref(stream);
            for (vi = 0; vi < c->views->len; vi++)
                llm_busy_set(g_ptr_array_index(c->views, vi), FALSE);
            llm_request_free(req);
            return;
        }
    }
    req->stream = stream;
    g_input_stream_read_async(req->stream, req->scratch,
                              sizeof(req->scratch), G_PRIORITY_DEFAULT,
                              req->core->cancel, llm_stream_read, req);
}
/* Un essai d'envoi : reconstruit un SoupMessage neuf depuis la
 * requête stockée (la session possède le message après send_async,
 * donc chaque tentative repart d'une instance fraîche). */
void
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

    /* Icône pause = annuler : miroir sur chaque vue attachée. */
    for (guint vi = 0; vi < req->core->views->len; vi++) {
        LlmTile *tv = g_ptr_array_index(req->core->views, vi);

        llm_busy_set(tv, TRUE); /* icône pause = annuler */
    }
    req->msg = msg;
    soup_session_send_async(req->core->soup, msg, G_PRIORITY_DEFAULT,
                            req->core->cancel, llm_send_done, req);
}

gboolean
llm_retry_tick(gpointer data)
{
    LlmRequest *req = data;

    /* Relance après délai : le core vit indépendamment de la vue. */
    llm_send_attempt(req);
    return G_SOURCE_REMOVE;
}
/* Livraison immédiate (décisions d'Éric, malformations) puis avance. */
void
core_cdb_announce(LlmCore *c, const char *text)
{
    core_history_push(c, LLMACTOR_CDB, TRUE, text);
    for (guint vi = 0; vi < c->views->len; vi++)
        llm_cdb_say_display(g_ptr_array_index(c->views, vi), text);
}


void
core_cdb_deliver(LlmCore *c, const char *text)
{
    core_history_push(c, LLMACTOR_CDB, FALSE, text);
    for (guint vi = 0; vi < c->views->len; vi++)
        llm_cdb_say_display(g_ptr_array_index(c->views, vi), text);
    llm_cdb_next(c);
}

/* Re-interrogation du modèle après livraison des résultats. L'ouverture
 * du tour neuf est faite par llm_send elle-même : chaque départ de
 * requête réinitialise t->core->reply — c'est LE correctif du re-comptage
 * infini des commandes /CDB::. */
void
llm_cdb_requery(LlmTile *t)
{
    llm_send(t, NULL);
}

/* Loi d'Éric (anti-spam) : quand la file de commandes se vide, un
 * résultat antérieur contenu à 100 % dans un résultat plus récent du
 * même bash est jeté — seule la version la plus longue est livrée.
 * Aucune perte : la capture lit tout le buffer, donc le plus récent
 * inclut fatalement les précédents du même terminal. */
void
llm_cdb_results_flush(LlmCore *c)
{
    GQueue   *q = c->cdb_results;
    guint     n;
    gboolean *drop;
    guint     i = 0;

    /* Boucle annulée par l'utilisateur : plus de re-requête. Les
     * résultats tardifs d'un poll bash encore actif sont jetés. */
    if (c->stop_requested) {
        if (q != NULL) {
            for (GList *l = q->head; l != NULL; l = l->next) {
                CdbResult *r = l->data;

                g_free(r->label);
                g_free(r->text);
                g_free(r);
            }
            g_queue_free(q);
        c->cdb_results = NULL;
        }
        return;
    }

    c->cdb_results = NULL;
    if (q == NULL) {
        if (c->views->len > 0)
            llm_cdb_requery(g_ptr_array_index(c->views, 0));
        /* C4 : sans vue, différer la re-interrogation */
        return;
    }
    if (g_queue_is_empty(q)) {
        g_queue_free(q);
        if (c->views->len > 0)
            llm_cdb_requery(g_ptr_array_index(c->views, 0));
        /* C4 : sans vue, différer la re-interrogation */
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

        core_history_push(c, LLMACTOR_CDB, FALSE, r->text);
        for (guint vi = 0; vi < c->views->len; vi++) {
            LlmTile *v = g_ptr_array_index(c->views, vi);

            if (!drop[i])
                llm_cdb_say_display(v, r->text);
        }
    }
    i = 0;
    for (GList *l = q->head; l != NULL; l = l->next, i++) {
        CdbResult *r = l->data;

        g_free(r->label);
        g_free(r->text);
        g_free(r);
    }
    g_free(drop);
    g_queue_free(q);

    if (c->views->len > 0)
            llm_cdb_requery(g_ptr_array_index(c->views, 0));
        /* C4 : sans vue, différer la re-interrogation */
}

/* Avance la file : commande suivante → approbation ; vide →
 * livraison des résultats pendants (dédupliqués), puis
 * re-interrogation du modèle. */
static LlmCore *
cdb_core_from_button(GtkButton *btn)
{
    for (GtkWidget *w = GTK_WIDGET(btn); w != NULL;
         w = gtk_widget_get_parent(w)) {
        LlmCore *c = g_object_get_data(G_OBJECT(w), "cdb-llm-core");

        if (c != NULL)
            return c;
    }
    return NULL;
}

void
on_cdb_refuse_clicked(GtkButton *btn, gpointer data)
{
    CdbDecision *d = data;
    LlmCore     *c = cdb_core_from_button(btn);
    const char  *note =
        "Éric a REFUSÉ cette commande. Ce n'est pas un bug : "
        "c'est une décision. Adapte-toi et propose autre chose.";

    if (c == NULL || c->decision != d || d->state != CDB_A_PENDING)
        return;
    d->state = CDB_A_REFUSED;
    for (guint vi = 0; vi < c->views->len; vi++)
        llm_tile_decision_lock(g_ptr_array_index(c->views, vi));
    core_cdb_deliver(c, note);
    g_free(d->cmd);
    g_free(d);
    c->decision = NULL;
}

void
on_cdb_approve_clicked(GtkButton *btn, gpointer data)
{
    CdbDecision *d = data;
    LlmCore     *c = cdb_core_from_button(btn);
    CdbPoll     *pl;

    if (c == NULL || c->decision != d || d->state != CDB_A_PENDING)
        return;
    d->state = CDB_A_APPROVED;
    for (guint vi = 0; vi < c->views->len; vi++)
        llm_tile_decision_lock(g_ptr_array_index(c->views, vi));

    pl = g_new0(CdbPoll, 1);
    pl->core = c;
    pl->tab = d->tab;
    pl->tab_label = g_strdup_printf("bash-%d", d->tab);

    bash_panel_ensure_tabs((guint)(d->tab + 1));

    if (!bash_panel_term_ready((guint)d->tab)) {
        if (!bash_panel_exec_tab_possible()) {
            char *note = g_strdup_printf(
                "terminal %s indisponible (panneau bash absent ?)",
                pl->tab_label);

            core_history_push(c, LLMACTOR_CDB, TRUE, note);
            for (guint vi = 0; vi < c->views->len; vi++)
                llm_cdb_say_display(g_ptr_array_index(c->views, vi), note);
            g_free(note);
            g_free(pl->tab_label);
            g_free(pl);
            return;
        }
        pl->pending_cmd = g_strdup(d->cmd);
        cdb_poll_register(pl);
        g_timeout_add(CDB_POLL_MS, cdb_spawn_wait_tick, pl);
        return;
    }

    if (!bash_panel_exec_tab((guint)d->tab, d->cmd)) {
        char *note = g_strdup_printf(
            "terminal %s indisponible (panneau bash absent ?)",
            pl->tab_label);

        core_history_push(c, LLMACTOR_CDB, TRUE, note);
        for (guint vi = 0; vi < c->views->len; vi++)
            llm_cdb_say_display(g_ptr_array_index(c->views, vi), note);
        g_free(note);
        g_free(pl->tab_label);
        g_free(pl);
        return;
    }
    bash_panel_set_busy((guint)d->tab, TRUE);

    cdb_poll_register(pl);
    g_timeout_add(CDB_POLL_MS, cdb_poll_tick, pl);
}

void
llm_cdb_next(LlmCore *c)
{
    CdbCmdSpec *s;

    if (c->cmd_queue == NULL || g_queue_is_empty(c->cmd_queue)) {
        llm_cdb_results_flush(c);
        return;
    }
    s = g_queue_pop_head(c->cmd_queue);

    c->decision = g_new0(CdbDecision, 1);
    c->decision->tab = s->tab;
    c->decision->cmd = g_strdup(s->cmd);
    c->decision->state = CDB_A_PENDING;
    for (guint vi = 0; vi < c->views->len; vi++)
        llm_tile_decision_render(g_ptr_array_index(c->views, vi));
    g_free(s->cmd);
    g_free(s);
}

/* Texte reply sans les blocs thinking : le modèle y rédige souvent des
 * brouillons de commandes qu'il ne faut ni exécuter ni condamner. Les
 * deux scans du protocole (détection + malformation) partent de là. */
char *
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
gboolean
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
gboolean
llm_agent_detect(LlmCore *c, const char *reply)
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
            if (c->cmd_queue != NULL)
                for (GList *l = c->cmd_queue->head; l != NULL; l = l->next) {
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
                if (c->cmd_queue == NULL)
                    c->cmd_queue = g_queue_new();
                g_queue_push_tail(c->cmd_queue, s);
            } else
                g_free(cmd);
        } while (g_match_info_next(mi, NULL));
    }
    g_match_info_free(mi);
    g_free(scan);

    if (found)
        llm_cdb_next(c);

    return found;
}

/* Construit le body chat/completions TEL qu'il serait envoyé à
 * l'instant T : persona re-résolu avec le projet courant + tout le
 * fil non-local. String g_strdup (à g_free). Utilisé par llm_send()
 * ET par le menu slots (voir / sauvegarder) — garantie d'identité
 * octet pour octet avec la requête réseau. */
char *
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
    json_builder_set_member_name(builder, "stream_options");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "include_usage");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);
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
    for (guint i = 0; i < t->core->history->len; i++) {
        LlmMsg     *m = &g_array_index(t->core->history, LlmMsg, i);
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
        } else if (m->actor == LLMACTOR_USER &&
                   m->images != NULL && m->images->len > 0) {
            json_builder_set_member_name(builder, "content");
            json_builder_begin_array(builder);

            if (m->content != NULL && m->content[0] != '\0') {
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "type");
                json_builder_add_string_value(builder, "text");
                json_builder_set_member_name(builder, "text");
                json_builder_add_string_value(builder, m->content);
                json_builder_end_object(builder);
            }

            for (guint j = 0; j < m->images->len; j++) {
                const char *url = g_ptr_array_index(m->images, j);

                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "type");
                json_builder_add_string_value(builder, "image_url");
                json_builder_set_member_name(builder, "image_url");
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "url");
                json_builder_add_string_value(builder, url);
                json_builder_end_object(builder);
                json_builder_end_object(builder);
            }

            json_builder_end_array(builder);
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
 * Ouvre un NOUVEAU tour de réponse : en-tête acteur, t->core->reply remis à
 * zéro, marque de streaming déplacée en fin de fil. Sans cette remise
 * à zéro, la boucle agentique ré-accumulait les réponses précédentes
 * dans t->core->reply et llm_agent_detect redétectait indéfiniment les MÊMES
 * commandes /CDB:: (comptage infini, exécutions multiples). */
/* Ouverture d'un tour : état au core, reset d'affichage par vue. */
void
llm_core_turn_new(LlmCore *c)
{
    g_string_truncate(c->reply, 0);
    c->in_reasoning = FALSE;
    c->stop_requested = FALSE;

    for (guint vi = 0; vi < c->views->len; vi++)
        llm_tile_turn_reset(g_ptr_array_index(c->views, vi));
}

void
llm_send(LlmTile *t, const char G_GNUC_UNUSED *prompt)
{
    LlmCore    *c = t->core;
    LlmRequest *req = g_new0(LlmRequest, 1);

    req->pending = g_string_new(NULL);
    llm_core_turn_new(c);
    if (c->cancel != NULL)
        g_object_unref(c->cancel);
    c->cancel = g_cancellable_new();
    c->cur_req = req;
    req->core = c;
    req->attempt = 0;
    req->url = g_strdup_printf("%s/chat/completions", c->cfg->api_url);

    if (c->cfg->api_key != NULL && c->cfg->api_key[0] != '\0')
        req->auth = g_strdup_printf("Bearer %s", c->cfg->api_key);

    req->body = llm_body_build(t);
    llm_slots_last_save(req->body);

    /* Bilan estimé : propre à chaque vue (bandeau). */
    for (guint vi = 0; vi < c->views->len; vi++) {
        LlmTile *v = g_ptr_array_index(c->views, vi);

        v->tokens_sent = (long)((strlen(req->body) + 3) / 4);
        v->tokens_received = (long)((c->reply->len + 3) / 4);
        v->tokens_context = v->tokens_sent + v->tokens_received;
        v->tokens_estimated = TRUE;
        llm_status_update(v);
        v->turns_since_ref++;
        llm_slots_title_update(v);
    }

    llm_send_attempt(req);
}

/* Vide t->core->history (contenus et images) sans toucher au GArray. */
void
llm_history_wipe(LlmTile *t)
{
    for (guint i = 0; i < t->core->history->len; i++) {
        LlmMsg *m = &g_array_index(t->core->history, LlmMsg, i);

        g_free(m->content);
        if (m->images != NULL)
            g_ptr_array_unref(m->images);
    }
    g_array_set_size(t->core->history, 0);
    llm_live_wipe();
}

/* Purge les files /CDB:: (elles référençaient l'ancien fil). */
void
llm_queues_purge(LlmTile *t)
{
    if (t->core->cmd_queue != NULL) {
        for (GList *l = t->core->cmd_queue->head; l != NULL; l = l->next) {
            CdbCmdSpec *s = l->data;

            g_free(s->cmd);
            g_free(s);
        }
        g_queue_free(t->core->cmd_queue);
        t->core->cmd_queue = NULL;
    }
    if (t->core->cdb_results != NULL) {
        for (GList *l = t->core->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            g_free(r->label);
            g_free(r->text);
            g_free(r);
        }
        g_queue_free(t->core->cdb_results);
        t->core->cdb_results = NULL;
    }
}

/* ===== Core conversationnel (Phase 1) ===== */

LlmCore *
llm_core_new(LlmConfig *cfg, GListStore *roots,
             GHashTable *multi_paths)
{
    LlmCore *c = g_new0(LlmCore, 1);

    c->cfg = cfg;
    c->roots = roots;
    c->multi_paths = multi_paths;
    c->soup = soup_session_new();
    c->cancel = g_cancellable_new();
    c->reply = g_string_new(NULL);
    c->history = g_array_new(FALSE, FALSE, sizeof(LlmMsg));
    c->views = g_ptr_array_new();
    return c;
}

void
llm_core_free(LlmCore *c)
{
    guint i;

    if (c == NULL)
        return;
    llm_live_save(c);
    if (c->cur_req != NULL)
        llm_request_free(c->cur_req);
    if (c->cancel != NULL)
        g_object_unref(c->cancel);
    if (c->soup != NULL)
        g_object_unref(c->soup);
    if (c->reply != NULL)
        g_string_free(c->reply, TRUE);
    if (c->history != NULL) {
        for (i = 0; i < c->history->len; i++) {
            LlmMsg *m = &g_array_index(c->history, LlmMsg, i);

            g_free(m->content);
            if (m->images != NULL)
                g_ptr_array_unref(m->images);
        }
        g_array_free(c->history, TRUE);
    }
    if (c->cmd_queue != NULL) {
        for (GList *l = c->cmd_queue->head; l != NULL; l = l->next) {
            CdbCmdSpec *s = l->data;

            g_free(s->cmd);
            g_free(s);
        }
        g_queue_free(c->cmd_queue);
    }
    if (c->cdb_results != NULL) {
        for (GList *l = c->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            g_free(r->label);
            g_free(r->text);
            g_free(r);
        }
        g_queue_free(c->cdb_results);
    }
    if (c->views != NULL)
        g_ptr_array_unref(c->views);
    g_free(c);
}
