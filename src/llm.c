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

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

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
    return NULL;
}

/* ------------------------------------------------ */
/* Liste des modèles (GET {api_url}/models)          */
/* ------------------------------------------------ */

typedef struct {
    LlmModelsCallback cb;
    gpointer          user_data;
    SoupSession      *soup;
} ModelsFetch;

static void
models_fetch_done(GObject *source, GAsyncResult *res, gpointer data)
{
    ModelsFetch *f = data;
    GBytes      *bytes;
    GError      *err = NULL;
    char       **ids = NULL;

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

                ids = g_new0(char *, n + 1);
                for (guint i = 0; i < n; i++) {
                    JsonObject *m = json_array_get_object_element(arr, i);

                    ids[i] = g_strdup(
                        json_object_get_string_member(m, "id"));
                }
            }
        }
        g_object_unref(parser);
        g_bytes_unref(bytes);
    } else {
        g_printerr("SIEB: /models échoué : %s\n", err->message);
        g_error_free(err);
    }

    f->cb(ids, f->user_data);
    g_strfreev(ids);
    g_object_unref(f->soup);
    g_free(f);
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

    /* Modèle actif (fallback : default_model du provider, puis
     * stealth/ox-alpha — le modèle de référence CDB). */
    cfg->model = json_object_has_member(active, "model")
                 ? g_strdup(json_object_get_string_member(active, "model"))
                 : NULL;

    /* Provider actif : api_url + api_key (+ default_model en repli). */
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
        if (cfg->model == NULL && json_object_has_member(prov_obj,
                                                         "default_model"))
            cfg->model = g_strdup(
                json_object_get_string_member(prov_obj, "default_model"));
    }

    /* Repli final : modèle de référence CDB si rien de défini. */
    if (cfg->model == NULL || cfg->model[0] == '\0') {
        g_free(cfg->model);
        cfg->model = g_strdup("stealth/ox-alpha");
    }

    /* Config incomplète = pas de chat. La clé est OPTIONNELLE
     * (providers gratuits type OpenCode Zen). */
    if (cfg->api_url == NULL || cfg->model == NULL) {
        llm_config_free(cfg);
        cfg = NULL;
    }
out:
    g_object_unref(parser);
    g_free(path);
    return cfg;
}

/* Sauvegarde (création/màj) d'un provider dans llm.json. Le fichier
 * existant est rechargé en arbre, modifié, réécrit — les autres
 * providers sont préservés. */
void
llm_config_save_provider(const char *provider, const char *api_key,
                         const char *default_model)
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
    json_object_set_string_member(prov, "default_model", default_model);

/* URL par défaut si absente, selon le provider. */
    if (g_strcmp0(json_object_get_string_member(prov, "api_url"), "") == 0) {
        const char *def = llm_provider_default_url(provider);

        if (def != NULL)
            json_object_set_string_member(prov, "api_url", def);
    }

    /* Provider actif + modèle actif. */
    {
        JsonObject *active = json_object_new();

        json_object_set_string_member(active, "provider", provider);
        json_object_set_string_member(active, "model", default_model);
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

/* Un échange de l'historique de conversation. */
typedef struct {
    char *role;    /* "user" / "assistant" */
    char *content;
} LlmMsg;

typedef struct {
    GtkWidget   *view;      /* historique (GtkTextView, non éditable) */
    GtkTextBuffer *hist;    /* buffer de l'historique */
    GtkWidget   *entry;     /* saisie */
    GtkWidget   *send_btn;
    LlmConfig   *cfg;
    SoupSession *soup;
    gboolean     busy;      /* requête en cours */
    gboolean     in_reasoning; /* delta courant = thinking */
    gboolean     follow;     /* scroll auto actif (user en bas) */
    GString     *reply;     /* réponse en cours d'accumulation */
    GtkTextMark *reply_mark;/* marque de fin de la réponse en streaming */
    GArray      *history;   /* LlmMsg[] : fil de conversation envoyé */
    GtkWidget   *model_btn; /* sélecteur de modèle (menu, label = actif) */
    GtkWidget   *model_pop; /* popover listant /models du provider */
    GtkWidget   *model_list;/* listbox des modèles */
    char       **models;    /* ids récupérés (copie possédée) */
} LlmTile;

static void on_llm_send_clicked(GtkButton *btn, gpointer data);
static void llm_stream_read(GObject *source, GAsyncResult *res, gpointer data);
static void llm_scroll_to_end(LlmTile *t);
static void on_llm_scroll(GtkAdjustment *adj, gpointer data);
static void llm_models_refresh(LlmTile *t);

static void
llm_tile_free(gpointer data)
{
    LlmTile *t = data;

    if (t->soup != NULL)
        g_object_unref(t->soup);
    if (t->reply != NULL)
        g_string_free(t->reply, TRUE);
    if (t->history != NULL) {
        for (guint i = 0; i < t->history->len; i++) {
            LlmMsg *m = &g_array_index(t->history, LlmMsg, i);

            g_free(m->role);
            g_free(m->content);
        }
        g_array_free(t->history, TRUE);
    }
    /* cfg EMPRUNTÉE à App (app->llm_cfg) : PAS libérée ici — main()
     * la libère une seule fois en fin de programme. */
    g_strfreev(t->models);
    g_free(t);
}

/* Choix d'un modèle dans le popover de la tuile : bascule immédiate
 * (mémoire) + persistance en « active » de llm.json. */
static void
on_llm_model_picked(GtkListBox G_GNUC_UNUSED *lb, GtkListBoxRow *row,
                    gpointer data)
{
    LlmTile    *t = data;
    const char *id = g_object_get_data(G_OBJECT(row), "model-id");

    if (id == NULL || t->cfg == NULL)
        return;
    g_free(t->cfg->model);
    t->cfg->model = g_strdup(id);
    gtk_menu_button_set_label(GTK_MENU_BUTTON(t->model_btn), id);
    llm_config_save_provider(t->cfg->provider,
                             t->cfg->api_key != NULL ? t->cfg->api_key : "",
                             id);
    gtk_popover_popdown(GTK_POPOVER(t->model_pop));
}

/* Reconstruit la liste des modèles du popover (filtre autorisés). */
static void
llm_models_refresh(LlmTile *t)
{
    char *filter = llm_config_get_allowed_models(t->cfg->provider);

    for (GtkWidget *child = gtk_widget_get_first_child(t->model_list);
         child != NULL; ) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);

        gtk_list_box_remove(GTK_LIST_BOX(t->model_list), child);
        child = next;
    }
    if (t->models != NULL) {
        for (int i = 0; t->models[i] != NULL; i++) {
            GtkWidget *lbl, *row;

            if (!llm_model_allowed(filter, t->models[i]))
                continue;
            lbl = gtk_label_new(t->models[i]);
            row = gtk_list_box_row_new();
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_widget_set_margin_start(lbl, 8);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
            g_object_set_data_full(G_OBJECT(row), "model-id",
                                   g_strdup(t->models[i]), g_free);
            gtk_list_box_append(GTK_LIST_BOX(t->model_list), row);
        }
    }
    g_free(filter);
}

/* Réception de /models pour la tuile. La tuile peut avoir été détruite
 * pendant le vol de la requête (re-rendu du layout) : contexte à
 * pointeur faible sur la tuile, NULLé par GTK à sa mort. */
typedef struct {
    LlmTile   *t;      /* valide garanti : l'ancre est référencée */
    GtkWidget *anchor; /* la box, ref possédée pendant le vol */
} LlmModelsFetchCtx;

static void
on_llm_models_fetched(char **ids, gpointer data)
{
    LlmModelsFetchCtx *ctx = data;
    LlmTile           *t = ctx->t;

    /* La ref sur l'ancre garantit que t est vivant ici, même si le
     * layout a été re-rendu entre-temps (la tuile attendait en mémoire). */
    if (t != NULL) {
        g_strfreev(t->models);
        t->models = ids != NULL ? g_strdupv(ids) : NULL;
        llm_models_refresh(t);
    }
    g_object_unref(ctx->anchor); /* lâche l'ancre : teardown normal */
    g_free(ctx);
}

/* Ajoute un échange à l'historique (role: "user"/"assistant"). */
static void
history_push(LlmTile *t, const char *role, const char *content)
{
    LlmMsg m = { g_strdup(role), g_strdup(content) };

    g_array_append_vals(t->history, &m, 1);
}

/* Ajoute du texte à l'historique. */
static void
hist_append(LlmTile *t, const char *text)
{
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_insert(t->hist, &end, text, -1);
}

/* Remplace le contenu après reply_mark par la réponse accumulée, rendue
 * en Markdown (le streaming réécrit la fin du buffer au fil des chunks ;
 * le parseur tolère le markdown incomplet). */
static void
hist_update_reply(LlmTile *t)
{
    GtkTextIter start, end;

    gtk_text_buffer_get_iter_at_mark(t->hist, &start, t->reply_mark);
    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_delete(t->hist, &start, &end);
    gtk_text_buffer_get_end_iter(t->hist, &end);
    md_insert(t->hist, &end, t->reply->str);
    /* Suit le texte qui défile. */
    llm_scroll_to_end(t);
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
        if (piece == NULL && json_object_has_member(delta, "reasoning")) {
            piece = json_object_get_string_member(delta, "reasoning");
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

typedef struct {
    LlmTile      *tile;
    SoupMessage  *msg;
    GInputStream *stream;
    char          scratch[4096]; /* buffer du read en cours */
    char          pending[8192]; /* lignes SSE partielles */
    gsize         pending_len;
    int           done;         /* garde anti double-libération */
} LlmRequest;

/* Libère la requête une seule fois (les callbacks de complétion
 * peuvent arriver en double selon l'état du flux). */
static void
llm_request_free(LlmRequest *req)
{
    if (req->done)
        return;
    req->done = 1;
    if (req->stream != NULL)
        g_object_unref(req->stream);
    g_free(req);
}

/* Traite les bytes reçus : découpe en lignes SSE.
 * Ligne trop longue pour le buffer : traitée par morceaux (le parser
 * JSON échouera sur le fragment, mais pending ne sature jamais —
 * les fragments sont jetés, pas accumulés à l'infini). */
static void
llm_process_bytes(LlmRequest *req, const char *bytes, gssize n)
{
    LlmTile *t = req->tile;

    if ((gsize)n > sizeof(req->pending) - 1 - req->pending_len) {
        /* Buffer plein sans \n : ligne aberrante, on la jette. */
        req->pending_len = 0;
        req->pending[0] = '\0';
    }
    memcpy(req->pending + req->pending_len, bytes, (size_t)n);
    req->pending_len += (gsize)n;
    req->pending[req->pending_len] = '\0';
    {
        char *nl;

        while ((nl = strchr(req->pending, '\n')) != NULL) {
            *nl = '\0';
            if (req->pending[0] != '\0')
                llm_handle_sse_line(t, req->pending);
            memmove(req->pending, nl + 1, strlen(nl + 1) + 1);
            req->pending_len -= (gsize)(nl - req->pending) + 1;
        }
    }
}

/* Lecture incrémentale du flux de réponse. */
static void
llm_stream_read(GObject G_GNUC_UNUSED *source, GAsyncResult *res,
                gpointer data)
{
    LlmRequest *req = data;
    LlmTile    *t = req->tile;
    gssize      n;
    GError     *error = NULL;

    n = g_input_stream_read_finish(req->stream, res, &error);
    if (error != NULL) {
        hist_append(t, error->message);
        g_error_free(error);
        llm_request_free(req);
        return;
    }
    if (n <= 0) {
        /* Fin du flux : la réponse complète rejoint l'historique
         * (sans les tags thinking, qui ne sont que de l'affichage). */
        history_push(t, "assistant", t->reply->str);
        hist_append(t, "\n");
        t->busy = FALSE;
        gtk_widget_set_sensitive(t->send_btn, TRUE);
        llm_request_free(req);
        return;
    }
    llm_process_bytes(req, req->scratch, n);
    g_input_stream_read_async(req->stream, req->scratch,
                              sizeof(req->scratch), G_PRIORITY_DEFAULT,
                              NULL, llm_stream_read, req);
}

/* Réponse initiale reçue : démarre la lecture du flux SSE. */
static void
llm_send_done(GObject *source, GAsyncResult *res, gpointer data)
{
    LlmRequest   *req = data;
    LlmTile      *t = req->tile;
    GError       *error = NULL;
    GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source),
                                                    res, &error);

    if (error != NULL) {
        hist_append(t, "\n[erreur : ");
        hist_append(t, error->message);
        hist_append(t, "]\n");
        g_error_free(error);
        t->busy = FALSE;
        gtk_widget_set_sensitive(t->send_btn, TRUE);
        llm_request_free(req);
        return;
    }
    {
        guint status = soup_message_get_status(req->msg);

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
            t->busy = FALSE;
            gtk_widget_set_sensitive(t->send_btn, TRUE);
            llm_request_free(req);
            return;
        }
    }
    req->stream = stream; /* transfert : libéré par llm_request_free */
    g_input_stream_read_async(req->stream, req->scratch,
                              sizeof(req->scratch), G_PRIORITY_DEFAULT,
                              NULL, llm_stream_read, req);
}

/* Construit et envoie la requête chat/completions (stream=true). */
static void
llm_send(LlmTile *t, const char G_GNUC_UNUSED *prompt)
{
    SoupMessage *msg;
    char        *url;
    JsonBuilder *builder;
    JsonNode    *root_node;
    char        *body;
    LlmRequest  *req = g_new0(LlmRequest, 1);

    url = g_strdup_printf("%s/chat/completions", t->cfg->api_url);
    msg = soup_message_new("POST", url);
    g_free(url);

    /* Clé optionnelle (OpenCode Zen fonctionne sans) : on n'envoie
     * l'Authorization que si elle existe. */
    if (t->cfg->api_key != NULL && t->cfg->api_key[0] != '\0') {
        char *auth = g_strdup_printf("Bearer %s", t->cfg->api_key);

        soup_message_headers_append(soup_message_get_request_headers(msg),
                                    "Authorization", auth);
        g_free(auth);
    }
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Accept", "text/event-stream");

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, t->cfg->model);
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);
    /* Historique COMPLET — le message user courant y est déjà
     * (history_push avant llm_send). Ne PAS le rajouter ici : c'était
     * la source des messages en double. */
    for (guint i = 0; i < t->history->len; i++) {
        LlmMsg *m = &g_array_index(t->history, LlmMsg, i);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "role");
        json_builder_add_string_value(builder, m->role);
        json_builder_set_member_name(builder, "content");
        json_builder_add_string_value(builder, m->content);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    root_node = json_builder_get_root(builder);
    body = json_to_string(root_node, FALSE);
    json_node_unref(root_node);
    g_object_unref(builder);

    soup_message_set_request_body_from_bytes(
        msg, "application/json",
        g_bytes_new_take((guint8 *)body, strlen(body)));

    gtk_widget_set_sensitive(t->send_btn, FALSE);
    req->tile = t;
    req->msg = msg; /* possédé par la session après send_async */
    soup_session_send_async(t->soup, msg, G_PRIORITY_DEFAULT, NULL,
                            llm_send_done, req);
}

static void
on_llm_send_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    LlmTile    *t = data;
    char       *prompt;

    /* Garde AVANT tout : activate + clicked peuvent arriver au même
     * tick (double émission du signal) — un seul envoi doit passer. */
    if (t->busy)
        return;
    t->busy = TRUE;
    prompt = g_strdup(gtk_editable_get_text(GTK_EDITABLE(t->entry)));
    if (prompt[0] == '\0') {
        g_free(prompt);
        t->busy = FALSE;
        return;
    }

    hist_append(t, "\n— vous —\n");
    hist_append(t, prompt);
    hist_append(t, "\n\n— ");
    hist_append(t, t->cfg->model);
    hist_append(t, " —\n");
    g_string_truncate(t->reply, 0);
    t->in_reasoning = FALSE;
    {
        GtkTextIter end;

        gtk_text_buffer_get_end_iter(t->hist, &end);
        if (t->reply_mark == NULL)
            t->reply_mark = gtk_text_buffer_create_mark(t->hist, NULL,
                                                        &end, TRUE);
        else
            gtk_text_buffer_move_mark(t->hist, t->reply_mark, &end);
    }
    gtk_editable_set_text(GTK_EDITABLE(t->entry), "");
    history_push(t, "user", prompt);
    llm_send(t, prompt);
    llm_scroll_to_end(t);
    g_free(prompt); /* copie : l'entry a été vidée */
}

/* Garde la vue collée en bas pendant/après le streaming — sauf si
 * l'utilisateur a remonté (il relit une zone : on ne le vole pas).
 * Le suivi reprend dès qu'il revient en bas. */
static void
llm_scroll_to_end(LlmTile *t)
{
    GtkTextIter end;

    if (!t->follow)
        return;
    gtk_text_buffer_get_end_iter(t->hist, &end);
    /* Place le curseur invisible à la fin : la vue suit le curseur. */
    gtk_text_buffer_place_cursor(t->hist, &end);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(t->view),
                                 gtk_text_buffer_get_insert(t->hist),
                                 0.0, TRUE, 0.0, 1.0);
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

static void
on_llm_entry_activate(GtkEntry G_GNUC_UNUSED *entry, gpointer data)
{
    on_llm_send_clicked(NULL, data);
}

GtkWidget *
llm_tile_new(const LlmConfig *cfg)
{
    GtkWidget *box;
    GtkWidget *scroll;
    LlmTile   *t = g_new0(LlmTile, 1);

    t->cfg = (LlmConfig *)cfg;

    if (cfg == NULL) {
        /* Pas de config : aide au lieu du chat. */
        GtkWidget *lbl = gtk_label_new(
            "LLM non configuré.\n\n"
            "Créez ~/.config/cdb/<session>/llm.json :\n"
            "{\n"
            "  \"providers\": {\n"
            "    \"OpenRouter\": {\n"
            "      \"api_url\": \"https://openrouter.ai/api/v1\",\n"
            "      \"api_key\": \"sk-or-…\",\n"
            "      \"default_model\": \"stealth/ox-alpha\"\n"
            "    }\n"
            "  },\n"
            "  \"active\": { \"provider\": \"OpenRouter\",\n"
            "               \"model\": \"stealth/ox-alpha\" }\n"
            "}");

        gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
        return lbl;
    }

    t->hist = gtk_text_buffer_new(NULL);
    t->view = gtk_text_view_new_with_buffer(t->hist);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(t->view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(t->view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(t->view), FALSE);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), t->view);
    /* Suivi auto tant que le user reste en bas de la vue. */
    t->follow = TRUE;
    g_signal_connect(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)),
        "value-changed", G_CALLBACK(on_llm_scroll), t);

    t->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(t->entry),
                                   "Message… (Entrée pour envoyer)");
    t->send_btn = gtk_button_new_with_label("Envoyer");
    g_signal_connect(t->send_btn, "clicked",
                     G_CALLBACK(on_llm_send_clicked), t);
    g_signal_connect(t->entry, "activate",
                     G_CALLBACK(on_llm_entry_activate), t);

    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *model_pop;
        GtkWidget *model_scroll;

        /* Sélecteur de modèle : label = modèle actif, popover = /models
         * du provider actif (chargé async). */
        t->model_list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(t->model_list),
                                        GTK_SELECTION_NONE);
        gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(t->model_list),
                                                  TRUE);
        g_signal_connect(t->model_list, "row-activated",
                         G_CALLBACK(on_llm_model_picked), t);
        model_scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(model_scroll),
                                       GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_min_content_height(
            GTK_SCROLLED_WINDOW(model_scroll), 24);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(model_scroll), 320);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(model_scroll), TRUE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(model_scroll),
                                      t->model_list);
        model_pop = gtk_popover_new();
        gtk_popover_set_child(GTK_POPOVER(model_pop), model_scroll);

        t->model_btn = gtk_menu_button_new();
        gtk_widget_add_css_class(t->model_btn, "flat");
        gtk_menu_button_set_label(GTK_MENU_BUTTON(t->model_btn),
                                  cfg->model != NULL ? cfg->model : "?");
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(t->model_btn),
                                    model_pop);
        t->model_pop = model_pop;

        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 6);
        gtk_box_append(GTK_BOX(row), t->model_btn);
        gtk_widget_set_hexpand(t->entry, TRUE);
        gtk_box_append(GTK_BOX(row), t->entry);
        gtk_box_append(GTK_BOX(row), t->send_btn);

        box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_append(GTK_BOX(box), scroll);
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_box_append(GTK_BOX(box), row);
    }

    {
        LlmModelsFetchCtx *ctx = g_new0(LlmModelsFetchCtx, 1);

        ctx->t = t;
        ctx->anchor = box;
        /* Ref sur l'ancre pendant le vol : pas de fenêtre de course —
         * la tuile ne peut pas être finalisée avant le callback. */
        g_object_ref(box);
        llm_models_fetch(cfg->provider, on_llm_models_fetched, ctx);
    }

    t->soup = soup_session_new();
    /* Anti-hang : pas de données pendant 120 s = abandon. */
    g_object_set(t->soup, "timeout", 120, "idle-timeout", 180, NULL);
    t->reply = g_string_new("");
    t->history = g_array_new(FALSE, FALSE, sizeof(LlmMsg));
    g_object_set_data_full(G_OBJECT(box), "cdb-llm-tile", t, llm_tile_free);
    return box;
}
