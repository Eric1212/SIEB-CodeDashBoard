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

    /* Config incomplète = pas de chat. */
    if (cfg->api_url == NULL || cfg->api_key == NULL || cfg->model == NULL) {
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

    /* URL par défaut si absente : OpenRouter. */
    if (g_strcmp0(json_object_get_string_member(prov, "api_url"), "") == 0
        && g_strcmp0(provider, "OpenRouter") == 0)
        json_object_set_string_member(prov, "api_url",
                                      "https://openrouter.ai/api/v1");

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
} LlmTile;

static void on_llm_send_clicked(GtkButton *btn, gpointer data);
static void llm_stream_read(GObject *source, GAsyncResult *res, gpointer data);
static void llm_scroll_to_end(LlmTile *t);
static void on_llm_scroll(GtkAdjustment *adj, gpointer data);

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
    g_free(t);
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

/* Remplace le contenu après reply_mark par la réponse accumulée
 * (le streaming réécrit la fin du buffer au fil des chunks). */
static void
hist_update_reply(LlmTile *t)
{
    GtkTextIter start, end;

    gtk_text_buffer_get_iter_at_mark(t->hist, &start, t->reply_mark);
    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_delete(t->hist, &start, &end);
    gtk_text_buffer_get_end_iter(t->hist, &end);
    gtk_text_buffer_insert(t->hist, &end, t->reply->str, -1);
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
    GString     *auth;
    LlmRequest  *req = g_new0(LlmRequest, 1);

    url = g_strdup_printf("%s/chat/completions", t->cfg->api_url);
    msg = soup_message_new("POST", url);
    g_free(url);

    auth = g_string_new("Bearer ");
    g_string_append(auth, t->cfg->api_key);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Authorization", auth->str);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Accept", "text/event-stream");
    g_string_free(auth, TRUE);

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

        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 6);
        gtk_widget_set_hexpand(t->entry, TRUE);
        gtk_box_append(GTK_BOX(row), t->entry);
        gtk_box_append(GTK_BOX(row), t->send_btn);

        box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_append(GTK_BOX(box), scroll);
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_box_append(GTK_BOX(box), row);
    }

    t->soup = soup_session_new();
    /* Anti-hang : pas de données pendant 120 s = abandon. */
    g_object_set(t->soup, "timeout", 120, "idle-timeout", 180, NULL);
    t->reply = g_string_new("");
    t->history = g_array_new(FALSE, FALSE, sizeof(LlmMsg));
    g_object_set_data_full(G_OBJECT(box), "cdb-llm-tile", t, llm_tile_free);
    return box;
}
