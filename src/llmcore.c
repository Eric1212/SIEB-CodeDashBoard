/*
 * llmcore.c : etat conversationnel LLM (LlmCore) — reseau SSE,
 * historique, boucle agentique (tool_calls), retries 429/5xx, annonces.
 *
 * Requete : POST {api_url}/chat/completions, stream=true.
 * Reponse : SSE data: {...} ; fin par data: [DONE].
 * Le core vit sans vue : les tuiles (llmtile.c) miroitent la
 * meme conversation (buffers par vue + diffusion).
 */

#define _POSIX_C_SOURCE 200809L
#include "llm.h"
/* Systeme : open/write/lstat/unlink (le _POSIX_C_SOURCE du dessus les
 * rend disponibles en -std=c23). */
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "session.h"
#include "mdview.h"
#include "i18n.h"
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

/* Attribution applicative (convention OpenRouter, doc app-attribution) :
 * headers HTTP poses sur CHAQUE requete chat/completions, tous providers
 * confondus — HTTP impose aux destinataires d'ignorer les headers
 * inconnus (RFC 9110 §6.3), donc HyperCharm/OpenCode les droppent
 * silencieusement (verifie par curl le 2026-06-25). OpenRouter, lui,
 * cree la page « App » dans ses rankings/analytics a partir de ca :
 *   HTTP-Referer             = URL, identifiant unique de l'app ;
 *   X-OpenRouter-Title       = nom affiche dans les rankings ;
 *   X-OpenRouter-Categories  = marketplace (comme Zed/Cursor). */
#define LLM_APP_REFERER    "https://github.com/SIEB/SIEB-CodeDashBoard"
#define LLM_APP_TITLE      "CodeDashBoard"
#define LLM_APP_CATEGORIES "programming-app"

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
        g_printerr(_("CDB: models.dev failed: %s\n"), err->message);
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
        g_printerr(_("CDB: /models failed: %s\n"), err->message);
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

/* ------------------------------------------------ */
/* Solde du provider (GET {api_url}/credits)         */
/*                                                    */
/* Liste VOLONTAIREMENT curated : deux entrées, les   */
/* seules dont la route ET la forme de réponse ont    */
/* été vérifiées. Les autres OpenAI-compatible        */
/* exposent bien un solde pour la plupart, mais       */
/* chacun chez soi : SiliconFlow sous /user/info avec */
/* « balance » en CHAÎNE, Kimi sous /users/me/balance */
/* (pluriel + /me/), DeepSeek sous /user/balance avec */
/* un TABLEAU multi-devises de valeurs en chaîne,     */
/* Fireworks sur une base d'admin distincte. Les      */
/* deviner produirait un « 0.00 » affirmé là où un    */
/* « — » est la seule réponse honnête. Ajouter un     */
/* provider = ajouter une ligne à CREDITS_LIST, avec  */
/* son champ, son unité et son taux.                  */
/* ------------------------------------------------ */

static const CreditsProvider CREDITS_LIST[] = {
    /* OpenRouter rend déjà des dollars, nidés sous « data ». */
    { "OpenRouter", "total_credits", TRUE,  "USD", 1.00 },
    /* HyperCharm rend des hypercredits, à la racine. Le taux est
     * confirmé par les totaux du compte : 1 830 hc consommés pour
     * 91,51 $, soit 0.050004 $/hc. */
    { "HyperCharm", "balance",       FALSE, "hc",  0.05 },
};

const CreditsProvider *
llm_credits_entry(const char *provider)
{
    guint i;

    if (provider == NULL)
        return NULL;
    for (i = 0; i < G_N_ELEMENTS(CREDITS_LIST); i++)
        if (g_strcmp0(provider, CREDITS_LIST[i].provider) == 0)
            return &CREDITS_LIST[i];
    return NULL;
}

/* Nombre d'un membre, dans les TROIS types qui se présentent vraiment :
 *
 *   G_TYPE_INT64  — json-glib range 173 et 0 en entier, PAS en double.
 *                   C'est le cas des deux providers de la liste : un
 *                   extracteur « double seulement » échouerait sur les
 *                   deux et afficherait « — » partout.
 *   G_TYPE_DOUBLE — un montant décimal (8.65).
 *   G_TYPE_STRING — plusieurs fournisseurs voisins rendent leurs dollars
 *                   entre guillemets (« "110.00" », « "0.88" »).
 *
 * json_object_get_double_member(), sur un type inattendu, ne signale pas
 * une erreur : il crache un CRITICAL json-glib et rend 0,0 — soit un
 * solde de 110 $ affiché 0 $. Ici on regarde le type et on échoue
 * proprement, ce qui mène à « — » et non à un faux montant. */
static gboolean
credits_number(JsonObject *obj, const char *member, double *out)
{
    JsonNode *node;
    GType     vt;

    if (obj == NULL || member == NULL || !json_object_has_member(obj, member))
        return FALSE;
    node = json_object_get_member(obj, member);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return FALSE;

    vt = json_node_get_value_type(node);
    if (vt == G_TYPE_INT64) {
        *out = (double) json_node_get_int(node);
        return TRUE;
    }
    if (vt == G_TYPE_DOUBLE) {
        *out = json_node_get_double(node);
        return TRUE;
    }
    if (vt == G_TYPE_STRING) {
        const char *s   = json_node_get_string(node);
        char       *end = NULL;
        double      v;

        if (s == NULL || s[0] == '\0')
            return FALSE;
        v = g_ascii_strtod(s, &end);
        if (end == s)
            return FALSE;
        *out = v;
        return TRUE;
    }
    return FALSE;
}

/* Où lire le solde : à la racine ou sous « data ». C'est la table qui le
 * dit, ligne par ligne — pas un reniflage de forme. */
static gboolean
credits_value(const CreditsProvider *cp, JsonNode *root, double *raw)
{
    JsonObject *obj;

    if (cp == NULL || root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        return FALSE;
    obj = json_node_get_object(root);

    if (cp->under_data) {
        JsonNode *data;

        if (!json_object_has_member(obj, "data"))
            return FALSE;
        data = json_object_get_member(obj, "data");
        if (data == NULL || !JSON_NODE_HOLDS_OBJECT(data))
            return FALSE;
        obj = json_node_get_object(data);
    }
    return credits_number(obj, cp->member, raw);
}

/* Base du provider : celle configurée dans llm.json d'abord — c'est elle
 * qui porte le vrai trafic du chat (cfg->api_url) —, la table en dur en
 * repli. Un « / » de trop collerait un « //credits » au bout de l'URL. */
static char *
credits_base_url(const char *provider)
{
    JsonObject *prov;
    JsonNode   *root_node = NULL;
    char       *url = NULL;

    prov = llm_config_provider_object(provider, &root_node);
    if (prov != NULL && json_object_has_member(prov, "api_url")) {
        JsonNode *n = json_object_get_member(prov, "api_url");

        if (n != NULL && JSON_NODE_HOLDS_VALUE(n)
            && json_node_get_value_type(n) == G_TYPE_STRING)
            url = g_strdup(json_node_get_string(n));
    }
    if (root_node != NULL)
        json_node_unref(root_node);

    if (url == NULL || url[0] == '\0') {
        g_free(url);
        url = g_strdup(llm_provider_default_url(provider));
    }
    if (url != NULL) {
        gsize n = strlen(url);

        while (n > 0 && url[n - 1] == '/')
            url[--n] = '\0';
    }
    return url;
}

static void
credits_fetch_done(GObject *source, GAsyncResult *res, gpointer data)
{
    CreditsFetch *f = data;
    GBytes       *bytes;
    GError       *err = NULL;
    gboolean      ok = FALSE;
    double        raw = 0.0;

    bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res,
                                              &err);
    if (bytes != NULL) {
        JsonParser *parser = json_parser_new();
        gsize       len    = g_bytes_get_size(bytes);

        if (json_parser_load_from_data(parser, g_bytes_get_data(bytes, NULL),
                                       (gssize)len, NULL))
            ok = credits_value(f->cp, json_parser_get_root(parser), &raw);
        g_object_unref(parser);
        g_bytes_unref(bytes);
    } else {
        /* 401, 404, 429 et les timeouts passent tous par ici : libsoup
         * ne rend pas le corps sur une erreur HTTP. Un échec réseau
         * n'est DONC jamais un solde à zéro, c'est un solde inconnu —
         * toute la nuance tient dans ce FALSE. */
        g_printerr(_("CDB: /credits %s failed: %s\n"), f->provider,
                   err->message);
        g_error_free(err);
    }

    if (f->cb != NULL)
        f->cb(ok, ok ? raw * f->cp->usd_per_unit : 0.0, ok ? raw : 0.0,
              f->user_data);

    g_free(f->provider);
    g_object_unref(f->soup);
    g_free(f);
}

/* Vrai si la requête est partie ; faux = rien sur le réseau, et callback
 * jamais appelé — c'est à l'appelant de poser « — » et d'en dire la
 * raison (provider hors liste, base vide ou pas de clé). */
gboolean
llm_credits_fetch(const char *provider, LlmCreditsCallback cb,
                  gpointer user_data)
{
    const CreditsProvider *cp = llm_credits_entry(provider);
    char                  *base;
    char                  *key;
    char                  *auth;
    char                  *url;
    CreditsFetch          *f;
    SoupMessage           *msg;

    if (cp == NULL || cb == NULL)
        return FALSE;              /* hors liste : on ne sonde rien du tout */

    base = credits_base_url(provider);
    if (base == NULL || base[0] == '\0') {
        g_free(base);
        return FALSE;
    }

    key = llm_config_get_api_key(provider);
    if (key == NULL || key[0] == '\0') {
        g_free(key);
        g_free(base);
        return FALSE;              /* pas de clé : rien à demander */
    }

    f = g_new0(CreditsFetch, 1);
    f->cb        = cb;
    f->user_data = user_data;
    f->provider  = g_strdup(provider);
    f->cp        = cp;             /* table statique : rien à libérer */
    f->soup      = soup_session_new();
    /* Un badge n'attend pas : 8 s, et un solde qui traîne n'intéresse
     * personne. La session du chat, elle, est à 120 s (anti-hang). */
    g_object_set(f->soup, "timeout", 8, NULL);

    url = g_strdup_printf("%s/credits", base);
    g_free(base);
    msg = soup_message_new("GET", url);
    g_free(url);
    if (msg == NULL) {
        g_free(f->provider);
        g_object_unref(f->soup);
        g_free(f);
        g_free(key);
        return FALSE;
    }

    /* Bearer pour les deux : HyperCharm refuse x-api-key (401 « missing
     * authorization ») et Bearer est leur convention partagée. */
    auth = g_strdup_printf("Bearer %s", key);
    g_free(key);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Authorization", auth);
    g_free(auth);

    /* La session possède msg après l'appel : NE PAS unref ici. */
    soup_session_send_and_read_async(f->soup, msg, G_PRIORITY_DEFAULT,
                                     NULL, credits_fetch_done, f);
    return TRUE;
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
        g_printerr(_("CDB: failed to write allowed_models: %s\n"), error->message);
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
            g_printerr(_("CDB: failed to write retry429: %s\n"), error->message);
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
            g_printerr(_("CDB: failed to write retry5xx: %s\n"), error->message);
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

    /* active.{provider,model} — les providers ne sont pas touchés. On mute
     * l'objet « active » déjà présent au lieu de le remplacer : le profil
     * actif ne vit QUE dans ce fichier (LlmConfig ne le porte pas), donc une
     * reconstruction à zéro effacerait « profile » en silence et rendrait
     * DEFAULT au prochain redémarrage. Même patron que
     * llm_config_set_active_profile(), qui préserve le reste du bloc. */
    if (!json_object_has_member(root, "active") ||
        json_object_get_object_member(root, "active") == NULL)
        json_object_set_object_member(root, "active", json_object_new());
    active = json_object_get_object_member(root, "active");
    json_object_set_string_member(active, "provider", provider);
    json_object_set_string_member(active, "model", model);

    /* COPIE immédiate : la chaîne vit dans l'arbre JSON qui sera libéré
     * plus bas (json_node_unref) — garder le pointeur serait un UAF. */
    {
        char *url_copy = (new_url != NULL) ? g_strdup(new_url) : NULL;

        gen = json_generator_new();
        text = json_to_string(work, TRUE);
        if (!g_file_set_contents(path, text, -1, &error)) {
            g_printerr(_("CDB: failed to save active provider: %s\n"),
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
        g_printerr(_("CDB: failed to write llm.json: %s\n"), error->message);
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
    "# Contrôle à distance des terminaux\n"                               \
    "Tu disposes de l'outil natif cdb_bash. Utilise-le chaque fois que "  \
    "tu dois inspecter, mesurer, compiler ou exécuter une action locale.\n" \
    "- terminal : numéro d'un terminal CDB (0 à 9 ; il est créé si besoin).\n" \
    "- command : commande shell complète, écrite telle quelle.\n"          \
    "- Le résultat est restitué dans une fenêtre de 100000 lignes. Pour "  \
    "paginer, utilise head/tail/sed DANS la commande.\n"                   \
    "- Chaque appel est soumis à l'approbation d'Éric avant exécution.\n"  \
    "- Si un résultat a content:null, cela signifie qu'il n'y a aucun "    \
    "contenu nouveau par rapport aux résultats précédents du même "        \
    "terminal : ce n'est pas un échec.\n"                                  \
    "- N'invente jamais une sortie de commande.\n"

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
        g_printerr(_("CDB: failed to write prompts/default.txt: %s\n"),
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

    memset(&m, 0, sizeof(m));
    m.actor = actor;
    m.local = local;
    m.kind = LLM_MSG_TEXT;
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

static void llm_process_tool_delta(LlmCore *c, JsonObject *obj,
                                   guint choice_index);

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

        llm_process_tool_delta(c, obj, 0);

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

static GPtrArray *cdb_polls = NULL;

#define CDB_TOOL_NAME "cdb_bash"
#define CDB_TOOL_NAME_READ "cdb_read"
#define CDB_TOOL_NAME_INSERT "cdb_insert"
#define CDB_TOOL_NAME_REPLACE "cdb_replace"
#define CDB_TOOL_NAME_CREATE "cdb_create"
#define CDB_TOOL_NAME_DELETE "cdb_delete"

void
llm_tool_call_free(gpointer data)
{
    LlmToolCall *tc = data;

    if (tc == NULL)
        return;
    g_free(tc->id);
    g_free(tc->name);
    g_free(tc->arguments_json);
    g_free(tc);
}

GPtrArray *
llm_tool_calls_new(void)
{
    return g_ptr_array_new_with_free_func(llm_tool_call_free);
}

void
llm_msg_clear(LlmMsg *m)
{
    if (m == NULL)
        return;
    g_free(m->content);
    if (m->images != NULL)
        g_ptr_array_unref(m->images);
    if (m->tool_calls != NULL)
        g_ptr_array_unref(m->tool_calls);
    g_free(m->tool_call_id);
    memset(m, 0, sizeof(*m));
}

static void
core_history_push_full(LlmCore *c, LlmActor actor, gboolean local,
                       LlmMsgKind kind, const char *content,
                       GPtrArray *tool_calls, const char *tool_call_id)
{
    LlmMsg m;

    memset(&m, 0, sizeof(m));
    m.actor = actor;
    m.local = local;
    m.kind = kind;
    m.content = g_strdup(content);
    m.images = NULL;
    /* Transfert de propriété : l'appelant ne doit plus libérer. */
    m.tool_calls = tool_calls;
    m.tool_call_id = g_strdup(tool_call_id);
    g_array_append_vals(c->history, &m, 1);
    llm_live_save(c);
}

static void
core_tool_result_commit(LlmCore *c, const char *tool_call_id,
                        const char *content, gboolean display)
{
    const char *shown;

    if (c == NULL || tool_call_id == NULL || tool_call_id[0] == '\0')
        return;
    if (c->answered_tools != NULL &&
        g_hash_table_contains(c->answered_tools, tool_call_id))
        return;

    core_history_push_full(c, LLMACTOR_CDB, FALSE,
                           LLM_MSG_TOOL_RESULT, content, NULL,
                           tool_call_id);
    if (c->answered_tools != NULL)
        g_hash_table_add(c->answered_tools, g_strdup(tool_call_id));

    if (!display)
        return;
    shown = content != NULL ? content : "〔tool〕 aucun contenu nouveau";
    for (guint vi = 0; vi < c->views->len; vi++) {
        LlmTile *v = g_ptr_array_index(c->views, vi);

        /* Vue par vue, pas globalement : une boîte qui attend CET
         * appel-là l'avale (sinon le même texte paraitrait deux fois —
         * une fois dans la boîte, une fois au fil). Les vues qui n'ont
         * pas la boîte — attachées après la décision, ou résultat d'un
         * outil ALLOW qui n'en a jamais eu — reçoivent le texte normal. */
        if (!llm_tile_box_result(v, tool_call_id, shown))
            llm_cdb_say_display(v, shown);
    }
}

static void
cdb_queue_text_result(LlmCore *c, const char *tool_call_id,
                      const char *label, const char *text,
                      const char *raw_text, gboolean shown)
{
    CdbResult *r;

    if (c == NULL || tool_call_id == NULL || tool_call_id[0] == '\0')
        return;
    r = g_new0(CdbResult, 1);
    r->tool_call_id = g_strdup(tool_call_id);
    r->label = g_strdup(label);
    r->raw_text = g_strdup(raw_text);
    r->text = g_strdup(text);
    r->shown = shown;

    if (c->cdb_results == NULL)
        c->cdb_results = g_queue_new();
    g_queue_push_tail(c->cdb_results, r);
}

/* Accumulateur des fragments tool_calls émis par le flux SSE. */
static void
pending_tool_free(gpointer data)
{
    LlmPendingToolCall *p = data;

    if (p == NULL)
        return;
    g_free(p->id);
    g_free(p->name);
    if (p->arguments != NULL)
        g_string_free(p->arguments, TRUE);
    g_free(p);
}

static LlmPendingToolCall *
pending_tool_get(LlmCore *c, long index, long fallback_index)
{
    LlmPendingToolCall *p;

    if (c->pending_tool_calls == NULL)
        c->pending_tool_calls = g_ptr_array_new_with_free_func(
            pending_tool_free);
    for (guint i = 0; i < c->pending_tool_calls->len; i++) {
        p = g_ptr_array_index(c->pending_tool_calls, i);

        if (p->index == index)
            return p;
    }
    p = g_new0(LlmPendingToolCall, 1);
    p->index = index >= 0 ? index : fallback_index;
    p->arguments = g_string_new(NULL);
    g_ptr_array_add(c->pending_tool_calls, p);
    return p;
}

static void
llm_core_clear_pending_tools(LlmCore *c)
{
    if (c->pending_tool_calls != NULL) {
        g_ptr_array_unref(c->pending_tool_calls);
        c->pending_tool_calls = NULL;
    }
}

static gint
pending_tool_cmp(gconstpointer a, gconstpointer b)
{
    const LlmPendingToolCall *pa = *((LlmPendingToolCall *const *)a);
    const LlmPendingToolCall *pb = *((LlmPendingToolCall *const *)b);

    return pa->index < pb->index ? -1 : pa->index > pb->index ? 1 : 0;
}

static void
llm_process_tool_delta(LlmCore *c, JsonObject *obj, guint choice_index)
{
    JsonArray *choices, *calls;
    JsonObject *choice, *container;

    if (obj == NULL || !json_object_has_member(obj, "choices"))
        return;
    choices = json_object_get_array_member(obj, "choices");
    if (choices == NULL || choice_index >= json_array_get_length(choices))
        return;
    choice = json_array_get_object_element(choices, choice_index);
    container = json_object_has_member(choice, "delta")
                    ? json_object_get_object_member(choice, "delta")
                    : json_object_has_member(choice, "message")
                          ? json_object_get_object_member(choice, "message")
                          : NULL;
    if (container == NULL || !json_object_has_member(container, "tool_calls"))
        return;
    calls = json_object_get_array_member(container, "tool_calls");
    if (calls == NULL)
        return;

    for (guint i = 0; i < json_array_get_length(calls); i++) {
        JsonNode          *tn = json_array_get_element(calls, i);
        JsonObject        *tc, *fn;
        LlmPendingToolCall *p;
        long               index;

        if (tn == NULL || !JSON_NODE_HOLDS_OBJECT(tn))
            continue;
        tc = json_node_get_object(tn);
        index = llm_json_int(tc, "index", (long)i);
        p = pending_tool_get(c, index, (long)i);

        if (json_object_has_member(tc, "id")) {
            JsonNode *n = json_object_get_member(tc, "id");

            if (JSON_NODE_HOLDS_VALUE(n) &&
                json_node_get_value_type(n) == G_TYPE_STRING) {
                const char *v = json_node_get_string(n);

                if (v != NULL && v[0] != '\0' && p->id == NULL)
                    p->id = g_strdup(v);
            }
        }

        if (!json_object_has_member(tc, "function") ||
            !JSON_NODE_HOLDS_OBJECT(json_object_get_member(tc, "function")))
            continue;
        fn = json_object_get_object_member(tc, "function");

        if (json_object_has_member(fn, "name")) {
            JsonNode *n = json_object_get_member(fn, "name");

            if (JSON_NODE_HOLDS_VALUE(n) &&
                json_node_get_value_type(n) == G_TYPE_STRING) {
                const char *v = json_node_get_string(n);

                if (v != NULL && v[0] != '\0') {
                    gchar *old_name = p->name;

                    /* Certains serveurs fragmentent aussi function.name. */
                    p->name = old_name ? g_strconcat(old_name, v, NULL)
                                       : g_strdup(v);
                }
            }
        }

        if (json_object_has_member(fn, "arguments")) {
            JsonNode *n = json_object_get_member(fn, "arguments");

            if (JSON_NODE_HOLDS_VALUE(n) &&
                json_node_get_value_type(n) == G_TYPE_STRING)
                g_string_append(p->arguments, json_node_get_string(n));
        }
    }
}

/* ---- outils fichiers : hash court (CRC32 -> 4 chars base36) ---- */

static guint32 cdb_crc_table[256];
static gboolean cdb_crc_ready = FALSE;

static void
cdb_crc32_init(void)
{
    for (guint32 i = 0; i < 256; i++) {
        guint32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        cdb_crc_table[i] = c;
    }
    cdb_crc_ready = TRUE;
}

static guint32
cdb_crc32(const void *buf, gsize len)
{
    const guint8 *b = buf;
    guint32 c = 0xFFFFFFFFu;

    if (!cdb_crc_ready)
        cdb_crc32_init();
    for (gsize i = 0; i < len; i++)
        c = cdb_crc_table[(c ^ b[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* Hash court d'une plage : 4 caracteres base36 (36^4 = 1 679 616).
 * Garde-fou de synchronisation, pas une signature. */
static char *
cdb_hash4(const void *buf, gsize len)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    guint32 v = cdb_crc32(buf, len) % 1679616u; /* 36^4 */
    char *s = g_new0(char, 5);

    s[4] = '\0';
    for (int i = 3; i >= 0; i--) {
        s[i] = digits[v % 36];
        v /= 36;
    }
    return s;
}

/* off[k] = offset du debut de la (k+1)-ieme ligne ; off[line_count] =
 * fin de la derniere ligne. Une ligne inclut son \n de terminaison,
 * sauf la derniere si le fichier ne finit pas par \n. vide => 0 ligne. */
static void
cdb_line_offsets(const char *content, gsize len, GArray *off,
                 guint *line_count)
{
    gsize z = 0;
    guint n_nl = 0;
    gboolean ends_nl;

    g_array_set_size(off, 0);
    g_array_append_val(off, z);
    for (gsize i = 0; i < len; i++) {
        if (content[i] == '\n') {
            gsize p = i + 1;
            g_array_append_val(off, p);
            n_nl++;
        }
    }
    ends_nl = (len > 0 && content[len - 1] == '\n');
    *line_count = n_nl + ((ends_nl || len == 0) ? 0 : 1);
    if (!ends_nl && len > 0) {
        gsize last = len;
        g_array_append_val(off, last);
    }
}

static const char *
cdb_kind_label(CdbSpecKind k)
{
    switch (k) {
    case CDB_SPEC_READ:    return "read";
    case CDB_SPEC_INSERT:  return "insert";
    case CDB_SPEC_REPLACE: return "replace";
    case CDB_SPEC_CREATE:  return "create";
    case CDB_SPEC_DELETE:  return "delete";
    default:               return "bash";
    }
}

/* Chaîne empruntée à l'arbre JSON : valable tant que le parser vit, NULL
 * si absente ou de mauvais type. (Rappel : ne jamais libérer le parser
 * avant la dernière utilisation d'une de ces chaînes.) */
static const char *
cdb_json_str(JsonObject *root, const char *member)
{
    JsonNode *n;

    if (root == NULL || !json_object_has_member(root, member))
        return NULL;
    n = json_object_get_member(root, member);
    if (!JSON_NODE_HOLDS_VALUE(n) ||
        json_node_get_value_type(n) != G_TYPE_STRING)
        return NULL;
    return json_object_get_string_member(root, member);
}

/* Nombre de lignes logiques d'un bloc, règle des offsets : une ligne
 * inclut son \n de terminaison. */
static guint
cdb_logical_lines(const char *t, gsize len)
{
    guint nl = 0;

    if (len == 0)
        return 0;
    for (gsize i = 0; i < len; i++)
        if (t[i] == '\n')
            nl++;
    return nl + ((t[len - 1] == '\n') ? 0 : 1);
}

/* Numéro (1-based) de la ligne contenant un offset. */
static guint
cdb_line_at(GArray *off, guint line_count, gsize pos)
{
    for (guint k = 1; k <= line_count; k++)
        if (pos < g_array_index(off, gsize, k))
            return k;
    return line_count;
}

/* Un seul libérateur pour la file agentique : chaque champ ajouté à
 * CdbCmdSpec doit passer par ici, sinon les 4 points de purge fuient
 * silencieusement. */
static void
cdb_cmd_spec_free(CdbCmdSpec *sp)
{
    if (sp == NULL)
        return;
    g_free(sp->tool_call_id);
    g_free(sp->cmd);
    g_free(sp->args_json);
    g_free(sp->summary);
    g_free(sp);
}

static void
cdb_decision_free(CdbDecision *d)
{
    if (d == NULL)
        return;
    cdb_cmd_spec_free(d->spec);
    g_free(d);
}

/* cdb_read : plage exacte depuis le DISQUE (jamais le dirty). */
static void
cdb_tool_file_read(LlmCore *c, const char *tool_call_id,
                   const char *args_json)
{
    JsonParser *parser;
    JsonObject *root;
    GError *gerr = NULL;
    const char *path = NULL;
    long from, to;
    guint f, t, line_count = 0;
    char *content = NULL;
    gsize len = 0;
    GArray *off = NULL;
    gsize rstart, rend;
    char *hash = NULL;
    GString *out = NULL;
    char *result = NULL;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser,
            args_json != NULL ? args_json : "",
            -1, NULL) ||
        json_parser_get_root(parser) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        cdb_queue_text_result(c, tool_call_id, "read",
            "arguments JSON invalides pour cdb_read.", NULL, FALSE);
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "path") &&
        JSON_NODE_HOLDS_VALUE(json_object_get_member(root, "path")) &&
        json_node_get_value_type(json_object_get_member(root, "path"))
            == G_TYPE_STRING)
        path = json_object_get_string_member(root, "path");
    from = llm_json_int(root, "from_line", -1);
    to = llm_json_int(root, "to_line", -1);

    if (path == NULL || path[0] == '\0') {
        cdb_queue_text_result(c, tool_call_id, "read",
            "path manquant.", NULL, FALSE);
        goto done;
    }
    if (path[0] != '/') {
        cdb_queue_text_result(c, tool_call_id, "read",
            "chemin absolu requis.", NULL, FALSE);
        goto done;
    }
    if (from < 1 || to < from) {
        cdb_queue_text_result(c, tool_call_id, "read",
            "from_line/to_line invalides (1-based, to >= from >= 1).",
            NULL, FALSE);
        goto done;
    }

    if (!g_file_get_contents(path, &content, &len, &gerr)) {
        char *m = g_strdup_printf("lecture impossible : %s",
            gerr != NULL ? gerr->message : "?");
        if (gerr != NULL)
            g_error_free(gerr);
        cdb_queue_text_result(c, tool_call_id, "read", m, NULL, FALSE);
        g_free(m);
        goto done;
    }
    if (!g_utf8_validate(content, (gssize)len, NULL)) {
        cdb_queue_text_result(c, tool_call_id, "read",
            "fichier binaire ou non UTF-8 : refusé.", NULL, FALSE);
        goto done;
    }

    off = g_array_new(FALSE, FALSE, sizeof(gsize));
    cdb_line_offsets(content, len, off, &line_count);
    f = (guint)from;
    t = (guint)to;
    if (f > line_count || t > line_count) {
        char *m = g_strdup_printf(
            "plage hors fichier : line_count=%u (demandé %u-%u).",
            line_count, f, t);
        cdb_queue_text_result(c, tool_call_id, "read", m, NULL, FALSE);
        g_free(m);
        goto done;
    }

    rstart = g_array_index(off, gsize, f - 1);
    rend = g_array_index(off, gsize, t);
    hash = cdb_hash4(content + rstart, rend - rstart);

    out = g_string_new(NULL);
    g_string_append_printf(out,
        "file: %s\nline_count: %u\nrange: %u-%u\nhash: %s\n\n",
        path, line_count, f, t, hash);
    for (guint ln = f; ln <= t; ln++) {
        gsize ls = g_array_index(off, gsize, ln - 1);
        gsize le = g_array_index(off, gsize, ln);
        gsize d = le - ls;
        if (d > 0 && content[ls + d - 1] == '\n')
            d--;
        g_string_append_printf(out, "%4u|%.*s\n", ln, (int)d,
            content + ls);
    }

    result = g_string_free(out, FALSE);
    out = NULL;
    cdb_queue_text_result(c, tool_call_id, "read", result, NULL, FALSE);

done:
    g_free(result);
    g_free(hash);
    if (out != NULL)
        g_string_free(out, TRUE);
    if (off != NULL)
        g_array_free(off, TRUE);
    g_free(content);
    g_object_unref(parser);
}


/* cdb_insert : insertion verbatim entre deux bornes adjacentes. DISQUE
 * seulement : le dirty de l'editeur reste l'affaire d'Eric (et git
 * protege). Les hashes sont revifies ICI et non au dispatch : c'est ce
 * qui permet a une approbation ASK de trainer sans rendre l'edition
 * aveugle. */
static void
cdb_tool_file_insert(LlmCore *c, const char *tool_call_id,
                     const char *args_json)
{
    JsonParser *parser = NULL;
    JsonObject *root = NULL;
    GError     *gerr = NULL;
    const char *path = NULL;
    const char *text = NULL;
    const char *before_hash = NULL;
    const char *after_hash = NULL;
    long         before = -1, after = -1;
    char        *content = NULL;
    char        *fresh = NULL;
    gsize        len = 0, ins_off = 0, tlen = 0, newlen = 0;
    GArray      *off = NULL;
    GArray      *noff = NULL;
    guint        line_count = 0, new_line_count = 0, a = 0, b = 0;
    guint        lo = 0, hi = 0;
    char        *hash = NULL;
    char        *hblock = NULL, *hfirst = NULL, *hlast = NULL;
    GString     *out = NULL;
    char        *result = NULL;
    char        *m = NULL;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser,
            args_json != NULL ? args_json : "", -1, NULL) ||
        json_parser_get_root(parser) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        m = g_strdup("arguments JSON invalides pour cdb_insert.");
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    path = cdb_json_str(root, "path");
    text = cdb_json_str(root, "text");
    before_hash = cdb_json_str(root, "before_hash");
    after_hash = cdb_json_str(root, "after_hash");
    before = llm_json_int(root, "before_line", -1);
    after = llm_json_int(root, "after_line", -1);

    if (path == NULL || path[0] == '\0') {
        m = g_strdup("path manquant.");
        goto done;
    }
    if (path[0] != '/') {
        m = g_strdup("chemin absolu requis.");
        goto done;
    }
    if (text == NULL || text[0] == '\0') {
        m = g_strdup("text manquant ou vide (pour supprimer, cdb_replace).");
        goto done;
    }
    if (before < 0 || after < 0) {
        m = g_strdup("before_line/after_line requis (0 = borne du fichier).");
        goto done;
    }
    if (before > 0 && after > 0 && after != before + 1) {
        m = g_strdup_printf(
            "bornes non adjacentes : after_line doit valoir "
            "before_line + 1 (recu %ld/%ld).", before, after);
        goto done;
    }
    tlen = strlen(text);

    if (!g_file_get_contents(path, &content, &len, &gerr)) {
        if (gerr != NULL && gerr->domain == G_FILE_ERROR &&
            gerr->code == G_FILE_ERROR_NOENT)
            m = g_strdup("fichier absent : utilise cdb_create.");
        else
            m = g_strdup_printf("lecture impossible : %s",
                                gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }
    if (!g_utf8_validate(content, (gssize)len, NULL)) {
        m = g_strdup("fichier binaire ou non UTF-8 : refusé.");
        goto done;
    }

    off = g_array_new(FALSE, FALSE, sizeof(gsize));
    cdb_line_offsets(content, len, off, &line_count);

    if (line_count == 0) {
        if (before != 0 || after != 0) {
            m = g_strdup_printf(
                "fichier vide (line_count=0) : before_line et after_line "
                "doivent valoir 0, recu %ld/%ld.", before, after);
            goto done;
        }
        ins_off = 0;
    } else if (before == 0) {
        if (after != 1) {
            m = g_strdup_printf(
                "insertion en tete : after_line doit valoir 1 (recu %ld).",
                after);
            goto done;
        }
        ins_off = g_array_index(off, gsize, 0);
    } else if (after == 0) {
        if (before != (long)line_count) {
            m = g_strdup_printf(
                "insertion en fin : before_line doit valoir line_count=%u "
                "(recu %ld).", line_count, before);
            goto done;
        }
        ins_off = len;
    } else {
        if (after > (long)line_count) {
            m = g_strdup_printf(
                "after_line hors fichier : line_count=%u (recu %ld). Pour "
                "inserer en fin de fichier, mets after_line=0 avec "
                "before_line=%u.", line_count, after, line_count);
            goto done;
        }
        ins_off = g_array_index(off, gsize, (guint)after - 1);
    }

    /* Garde-fou : les lignes qui bordent le point d'insertion sont-elles
     * bien celles que le modele a lues ? */
    if (before > 0) {
        gsize ls = g_array_index(off, gsize, (guint)before - 1);
        gsize le = g_array_index(off, gsize, (guint)before);

        hash = cdb_hash4(content + ls, le - ls);
        if (before_hash == NULL || g_strcmp0(hash, before_hash) != 0) {
            /* JAMAIS le hash courant ici : un refus qui le divulgue
             * dispense le modele de lire, et detruit la preuve de Focus. */
            m = g_strdup_printf(
                "before_hash obsolete ou absent : la ligne %ld ne porte "
                "plus le contenu que tu affirmes avoir lu. Fais "
                "cdb_read(%ld, %ld) et rejoue le hash recu.",
                before, before, before);
            goto done;
        }
        g_free(hash);
        hash = NULL;
    }
    if (after > 0) {
        gsize ls = g_array_index(off, gsize, (guint)after - 1);
        gsize le = g_array_index(off, gsize, (guint)after);

        hash = cdb_hash4(content + ls, le - ls);
        if (after_hash == NULL || g_strcmp0(hash, after_hash) != 0) {
            m = g_strdup_printf(
                "after_hash obsolete ou absent : la ligne %ld ne porte "
                "plus le contenu que tu affirmes avoir lu. Fais "
                "cdb_read(%ld, %ld) et rejoue le hash recu.",
                after, after, after);
            goto done;
        }
        g_free(hash);
        hash = NULL;
    }

    {
        GString *nb = g_string_sized_new(len + tlen);

        g_string_append_len(nb, content, ins_off);
        g_string_append_len(nb, text, tlen);
        g_string_append_len(nb, content + ins_off, len - ins_off);
        fresh = g_string_free(nb, FALSE);
    }
    newlen = len + tlen;

    if (!g_file_set_contents(path, fresh, (gssize)newlen, &gerr)) {
        m = g_strdup_printf("ecriture impossible : %s",
                            gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }

    /* On rend l'etat REEL apres coupure, pas l'intention declaree. */
    noff = g_array_new(FALSE, FALSE, sizeof(gsize));
    cdb_line_offsets(fresh, newlen, noff, &new_line_count);
    a = cdb_line_at(noff, new_line_count, ins_off);
    b = cdb_line_at(noff, new_line_count, ins_off + tlen - 1);

    /* Un token ne se frappe que sur une ligne ENTIEREMENT fournie par le
     * modele. Publier le hash d'une ligne de bordure qui melange son texte
     * a du contenu deja present reviendrait a lui delivrer le droit d'ecrire
     * cette ligne sans jamais l'avoir lue : c'est la faille du refus qui
     * rend le hash, mais par la porte d'entree. */
    lo = (g_array_index(noff, gsize, a - 1) == ins_off) ? a : a + 1;
    hi = (g_array_index(noff, gsize, b) <= ins_off + tlen) ? b : b - 1;

    out = g_string_new(NULL);
    g_string_append_printf(out,
        "insert: ok\npath: %s\ninserted_range: %u-%u\nline_count: %u\n",
        path, a, b, new_line_count);
    if (lo <= hi) {
        hblock = cdb_hash4(fresh + g_array_index(noff, gsize, lo - 1),
                           g_array_index(noff, gsize, hi) -
                           g_array_index(noff, gsize, lo - 1));
        hfirst = cdb_hash4(fresh + g_array_index(noff, gsize, lo - 1),
                           g_array_index(noff, gsize, lo) -
                           g_array_index(noff, gsize, lo - 1));
        hlast  = cdb_hash4(fresh + g_array_index(noff, gsize, hi - 1),
                           g_array_index(noff, gsize, hi) -
                           g_array_index(noff, gsize, hi - 1));
        g_string_append_printf(out,
            "authored_range: %u-%u\nhash_block: %s\nhash_first: %s\n"
            "hash_last: %s\n", lo, hi, hblock, hfirst, hlast);
        if (lo != a || hi != b)
            g_string_append(out,
                "note: les lignes hors de authored_range melangent ton texte "
                "et du contenu existant; aucun hash n'y est attache.\n");
    } else {
        g_string_append(out,
            "authored_range: aucune ligne entiere\n"
            "note: aucune ligne n'est entierement a toi (text sans saut de "
            "ligne final, ou insere au milieu d'une ligne). Lis avec "
            "cdb_read(N,N) avant toute autre ecriture.\n");
    }
    result = g_string_free(out, FALSE);
    out = NULL;
    cdb_queue_text_result(c, tool_call_id, "insert", result, NULL, FALSE);

done:
    if (m != NULL) {
        cdb_queue_text_result(c, tool_call_id, "insert", m, NULL, FALSE);
        g_free(m);
    }
    g_free(result);
    g_free(hblock);
    g_free(hfirst);
    g_free(hlast);
    g_free(hash);
    if (out != NULL)
        g_string_free(out, TRUE);
    if (noff != NULL)
        g_array_free(noff, TRUE);
    if (off != NULL)
        g_array_free(off, TRUE);
    g_free(fresh);
    g_free(content);
    if (gerr != NULL)
        g_error_free(gerr);
    if (parser != NULL)
        g_object_unref(parser);
}

/* cdb_replace : remplace les lignes from..to EN ENTIER (avec leur \n de
 * terminaison, ce qui est exactement la zone couverte par block_hash) par
 * text verbatim. text vide = suppression. Aucun plafond : le hash est la
 * preuve que le modele a lu les lignes precises qu'il detruit. */
static void
cdb_tool_file_replace(LlmCore *c, const char *tool_call_id,
                      const char *args_json)
{
    JsonParser *parser = NULL;
    JsonObject *root = NULL;
    GError     *gerr = NULL;
    const char *path = NULL;
    const char *text = NULL;
    const char *block_hash = NULL;
    long        from = -1, to = -1;
    char       *content = NULL;
    char       *fresh = NULL;
    gsize       len = 0, cut_a = 0, cut_b = 0, tlen = 0, newlen = 0;
    GArray     *off = NULL;
    GArray     *noff = NULL;
    guint       line_count = 0, new_line_count = 0, a = 0, b = 0;
    guint       lo = 0, hi = 0;
    char       *hash = NULL;
    char       *hblock = NULL, *hfirst = NULL, *hlast = NULL;
    GString    *out = NULL;
    char       *result = NULL;
    char       *m = NULL;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser,
            args_json != NULL ? args_json : "", -1, NULL) ||
        json_parser_get_root(parser) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        m = g_strdup("arguments JSON invalides pour cdb_replace.");
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    path = cdb_json_str(root, "path");
    text = cdb_json_str(root, "text");
    block_hash = cdb_json_str(root, "block_hash");
    from = llm_json_int(root, "from_line", -1);
    to = llm_json_int(root, "to_line", -1);

    if (path == NULL || path[0] == '\0') {
        m = g_strdup("path manquant.");
        goto done;
    }
    if (path[0] != '/') {
        m = g_strdup("chemin absolu requis.");
        goto done;
    }
    if (text == NULL) {
        m = g_strdup("text manquant : mets text:\"\" pour supprimer la plage.");
        goto done;
    }
    if (block_hash == NULL || block_hash[0] == '\0') {
        m = g_strdup("block_hash requis : lis la zone avec "
                    "cdb_read(from_line, to_line).");
        goto done;
    }
    if (from < 1 || to < from) {
        m = g_strdup("from_line/to_line invalides (1-based, to >= from >= 1).");
        goto done;
    }
    tlen = strlen(text);

    if (!g_file_get_contents(path, &content, &len, &gerr)) {
        if (gerr != NULL && gerr->domain == G_FILE_ERROR &&
            gerr->code == G_FILE_ERROR_NOENT)
            m = g_strdup("fichier absent : utilise cdb_create.");
        else
            m = g_strdup_printf("lecture impossible : %s",
                                gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }
    if (!g_utf8_validate(content, (gssize)len, NULL)) {
        m = g_strdup("fichier binaire ou non UTF-8 : refusé.");
        goto done;
    }

    off = g_array_new(FALSE, FALSE, sizeof(gsize));
    cdb_line_offsets(content, len, off, &line_count);
    if (line_count == 0) {
        m = g_strdup("fichier vide : rien a remplacer (utilise cdb_insert).");
        goto done;
    }
    if (to > (long)line_count) {
        m = g_strdup_printf(
            "to_line hors fichier : line_count=%u (recu %ld). Donne les "
            "lignes exactes.", line_count, to);
        goto done;
    }

    cut_a = g_array_index(off, gsize, (guint)from - 1);
    cut_b = g_array_index(off, gsize, (guint)to);

    /* SEULE preuve acceptee : le hash de LA plage, a l'instant present. */
    hash = cdb_hash4(content + cut_a, cut_b - cut_a);
    if (g_strcmp0(hash, block_hash) != 0) {
        m = g_strdup_printf(
            "block_hash obsolete ou absent : la plage %ld-%ld ne porte plus "
            "le contenu que tu affirmes avoir lu. Fais cdb_read(%ld, %ld) et "
            "rejoue le hash recu.", from, to, from, to);
        goto done;
    }
    g_free(hash);
    hash = NULL;

    {
        GString *nb = g_string_sized_new(len - (cut_b - cut_a) + tlen);
        g_string_append_len(nb, content, cut_a);
        g_string_append_len(nb, text, tlen);
        g_string_append_len(nb, content + cut_b, len - cut_b);
        fresh = g_string_free(nb, FALSE);
    }
    newlen = len - (cut_b - cut_a) + tlen;

    if (!g_file_set_contents(path, fresh, (gssize)newlen, &gerr)) {
        m = g_strdup_printf("ecriture impossible : %s",
                            gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }

    noff = g_array_new(FALSE, FALSE, sizeof(gsize));
    cdb_line_offsets(fresh, newlen, noff, &new_line_count);

    out = g_string_new(NULL);
    g_string_append_printf(out,
        "replace: ok\npath: %s\nreplaced_range: %ld-%ld\nline_count: %u\n",
        path, from, to, new_line_count);
    /* cut_a tombe toujours sur un debut de ligne (on remplace des lignes
     * entieres) : fusion possible seulement vers l'avant. */
    if (tlen == 0) {
        g_string_append(out,
            "authored_range: aucune ligne entiere\n"
            "note: plage supprimee sans reecriture, donc aucun hash frappe. "
            "Relis la zone resultante avant toute nouvelle ecriture.\n");
    } else {
        a = cdb_line_at(noff, new_line_count, cut_a);
        b = cdb_line_at(noff, new_line_count, cut_a + tlen - 1);
        lo = (g_array_index(noff, gsize, a - 1) == cut_a) ? a : a + 1;
        hi = (g_array_index(noff, gsize, b) <= cut_a + tlen) ? b : b - 1;
        if (lo <= hi) {
            hblock = cdb_hash4(fresh + g_array_index(noff, gsize, lo - 1),
                               g_array_index(noff, gsize, hi) -
                               g_array_index(noff, gsize, lo - 1));
            hfirst = cdb_hash4(fresh + g_array_index(noff, gsize, lo - 1),
                               g_array_index(noff, gsize, lo) -
                               g_array_index(noff, gsize, lo - 1));
            hlast  = cdb_hash4(fresh + g_array_index(noff, gsize, hi - 1),
                               g_array_index(noff, gsize, hi) -
                               g_array_index(noff, gsize, hi - 1));
            g_string_append_printf(out,
                "authored_range: %u-%u\nhash_block: %s\nhash_first: %s\n"
                "hash_last: %s\n", lo, hi, hblock, hfirst, hlast);
            if (lo != a || hi != b)
                g_string_append(out,
                    "note: les lignes hors de authored_range melangent ton "
                    "texte et du contenu existant; aucun hash n'y est "
                    "attache.\n");
        } else {
            g_string_append(out,
                "authored_range: aucune ligne entiere\n"
                "note: aucune ligne n'est entierement a toi (text sans saut "
                "de ligne final). Lis avec cdb_read(N,N) avant toute autre "
                "ecriture.\n");
        }
    }
    result = g_string_free(out, FALSE);
    out = NULL;
    cdb_queue_text_result(c, tool_call_id, "replace", result, NULL, FALSE);

done:
    if (m != NULL) {
        cdb_queue_text_result(c, tool_call_id, "replace", m, NULL, FALSE);
        g_free(m);
    }
    g_free(result);
    g_free(hblock);
    g_free(hfirst);
    g_free(hlast);
    g_free(hash);
    if (out != NULL)
        g_string_free(out, TRUE);
    if (noff != NULL)
        g_array_free(noff, TRUE);
    if (off != NULL)
        g_array_free(off, TRUE);
    g_free(fresh);
    g_free(content);
    if (gerr != NULL)
        g_error_free(gerr);
    if (parser != NULL)
        g_object_unref(parser);
}

/* cdb_create : cree un fichier texte NEUF.
 *
 * O_CREAT|O_EXCL n'est pas un raffinement : g_file_set_contents ferait
 * "existe-t-il ?" puis "j'ecris", deux gestes entre lesquels un autre
 * processus peut creer le fichier. Ici le noyau tranche : aucune course
 * ne peut transformer un "n'existe pas" en ecrasement. */
static gboolean
cdb_file_create_exclusive(const char *path, const char *buf, gsize len,
                          GError **err)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    int e;

    if (fd < 0) {
        e = errno;
        g_set_error(err, G_IO_ERROR,
                    (e == EEXIST) ? G_IO_ERROR_EXISTS : G_IO_ERROR_FAILED,
                    "%s", g_strerror(e));
        return FALSE;
    }
    while (len > 0) {
        ssize_t w = write(fd, buf, len);

        if (w < 0) {
            e = errno;
            if (e == EINTR)
                continue;
            close(fd);
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                        g_strerror(e));
            return FALSE;
        }
        buf += w;
        len -= (gsize)w;
    }
    if (close(fd) != 0) {
        e = errno;
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                    g_strerror(e));
        return FALSE;
    }
    return TRUE;
}

static void
cdb_tool_file_create(LlmCore *c, const char *tool_call_id,
                     const char *args_json)
{
    JsonParser *parser = NULL;
    JsonObject *root = NULL;
    GError     *gerr = NULL;
    const char *path = NULL;
    const char *content = NULL;
    gsize       clen = 0;
    char       *dir = NULL;
    GArray     *off = NULL;
    guint       line_count = 0;
    char       *hblock = NULL, *hfirst = NULL, *hlast = NULL;
    GString    *out = NULL;
    char       *result = NULL;
    char       *m = NULL;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser,
            args_json != NULL ? args_json : "", -1, NULL) ||
        json_parser_get_root(parser) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        m = g_strdup("arguments JSON invalides pour cdb_create.");
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    path = cdb_json_str(root, "path");
    content = cdb_json_str(root, "content");
    clen = (content != NULL) ? strlen(content) : 0;
    if (path == NULL || path[0] == '\0') {
        m = g_strdup("path manquant.");
        goto done;
    }
    if (path[0] != '/') {
        m = g_strdup("chemin absolu requis.");
        goto done;
    }
    if (content == NULL) {
        m = g_strdup("content manquant : mets content vide pour creer un "
                     "fichier vide.");
        goto done;
    }
    if (!g_utf8_validate(content, (gssize)clen, NULL)) {
        m = g_strdup("contenu non UTF-8 : refuse.");
        goto done;
    }
    dir = g_path_get_dirname(path);
    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        m = g_strdup_printf("dossier parent absent : %s (cdb_create ne cree "
                            "pas de repertoire).", dir);
        goto done;
    }
    if (!cdb_file_create_exclusive(path, content, clen, &gerr)) {
        if (gerr != NULL && gerr->domain == G_IO_ERROR &&
            gerr->code == G_IO_ERROR_EXISTS)
            m = g_strdup("fichier existe deja : utilise cdb_replace avec son "
                         "block_hash, ou cdb_delete puis cdb_create.");
        else
            m = g_strdup_printf("creation impossible : %s",
                                gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }

    /* Ce qui est sur disque est exactement ce qu'on vient d'ecrire : tout
     * est fourni par le modele, donc authored_range couvre le fichier. */
    out = g_string_new(NULL);
    if (clen == 0) {
        g_string_append_printf(out,
            "create: ok\npath: %s\nline_count: 0\n"
            "note: fichier vide cree; aucun hash frappe.\n", path);
    } else {
        off = g_array_new(FALSE, FALSE, sizeof(gsize));
        cdb_line_offsets(content, clen, off, &line_count);
        hblock = cdb_hash4(content, clen);
        hfirst = cdb_hash4(content, g_array_index(off, gsize, 1) -
                                  g_array_index(off, gsize, 0));
        hlast  = cdb_hash4(content +
                           g_array_index(off, gsize, line_count - 1),
                           g_array_index(off, gsize, line_count) -
                           g_array_index(off, gsize, line_count - 1));
        g_string_append_printf(out,
            "create: ok\npath: %s\nline_count: %u\nauthored_range: 1-%u\n"
            "hash_block: %s\nhash_first: %s\nhash_last: %s\n",
            path, line_count, line_count, hblock, hfirst, hlast);
    }
    result = g_string_free(out, FALSE);
    out = NULL;
    cdb_queue_text_result(c, tool_call_id, "create", result, NULL, FALSE);

done:
    if (m != NULL) {
        cdb_queue_text_result(c, tool_call_id, "create", m, NULL, FALSE);
        g_free(m);
    }
    g_free(result);
    g_free(hblock);
    g_free(hfirst);
    g_free(hlast);
    if (out != NULL)
        g_string_free(out, TRUE);
    if (off != NULL)
        g_array_free(off, TRUE);
    g_free(dir);
    if (gerr != NULL)
        g_error_free(gerr);
    if (parser != NULL)
        g_object_unref(parser);
}

/* cdb_delete : destruction en DEUX PASSES, comme demande par Eric.
 *
 * Le file_hash rendu par la premiere passe n'est PAS une divulgation :
 * ici le modele n'a pas a prouver qu'il a lu le CONTENU, il doit etre sur
 * du FICHIER. Le hash est une empreinte d'etat anti-TOCTOU entre les deux
 * appels. Ce qui reste interdit, c'est qu'une SECONDE passe refusee rende
 * l'empreinte courante : la, on retomberait dans l'oracle. */
static void
cdb_tool_file_delete(LlmCore *c, const char *tool_call_id,
                     const char *args_json)
{
    JsonParser *parser = NULL;
    JsonObject *root = NULL;
    GError     *gerr = NULL;
    const char *path = NULL;
    const char *file_hash = NULL;
    struct stat st;
    char       *content = NULL;
    gsize       len = 0;
    GArray     *off = NULL;
    guint       line_count = 0;
    char       *fh = NULL;
    GString    *out = NULL;
    char       *result = NULL;
    char       *m = NULL;
    gboolean    binary = FALSE;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser,
            args_json != NULL ? args_json : "", -1, NULL) ||
        json_parser_get_root(parser) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        m = g_strdup("arguments JSON invalides pour cdb_delete.");
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    path = cdb_json_str(root, "path");
    file_hash = cdb_json_str(root, "file_hash");

    if (path == NULL || path[0] == '\0') {
        m = g_strdup("path manquant.");
        goto done;
    }
    if (path[0] != '/') {
        m = g_strdup("chemin absolu requis.");
        goto done;
    }
    if (lstat(path, &st) != 0) {
        m = g_strdup("fichier absent : rien a supprimer.");
        goto done;
    }
    /* lstat, pas stat : un lien se detruit lui-meme alors que son contenu
     * se lit a travers la cible. Le hash rendu n'aurait rien a voir avec
     * ce qui serait efface. */
    if (S_ISLNK(st.st_mode)) {
        m = g_strdup("lien symbolique refuse : son contenu se lit a travers "
                     "la cible, mais la suppression detruirait le lien. Agis "
                     "sur la cible directement.");
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        m = g_strdup("pas un fichier regulier : non supprime (repertoire, "
                     "pipe, peripherique).");
        goto done;
    }
    if (!g_file_get_contents(path, &content, &len, &gerr)) {
        m = g_strdup_printf("lecture impossible : %s",
                            gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }
    /* Un binaire peut se supprimer : le hash prouve l'identite du fichier,
     * pas sa lisibilite. On le signale seulement. */
    if (!g_utf8_validate(content, (gssize)len, NULL))
        binary = TRUE;

    off = g_array_new(FALSE, FALSE, sizeof(gsize));
    cdb_line_offsets(content, len, off, &line_count);
    fh = cdb_hash4(content, len);
    out = g_string_new(NULL);
    if (file_hash == NULL || file_hash[0] == '\0') {
        g_string_append_printf(out,
            "delete: confirmation requise\npath: %s\nline_count: %u\n"
            "bytes: %lu\nbinary: %s\nfile_hash: %s\n"
            "note: relance cdb_delete(path, file_hash) pour detruire. Si le "
            "fichier change entre les deux appels, la seconde passe sera "
            "refusee.\n",
            path, line_count, (gulong)len, binary ? "yes" : "no", fh);
    } else if (g_strcmp0(fh, file_hash) != 0) {
        g_free(fh);
        fh = NULL;
        g_string_free(out, TRUE);
        out = NULL;
        m = g_strdup("file_hash obsolete : le fichier n'est plus celui qui a "
                     "ete confirme. Relance cdb_delete sans hash pour "
                     "l'empreinte courante.");
        goto done;
    } else {
        if (unlink(path) != 0) {
            g_free(fh);
            fh = NULL;
            g_string_free(out, TRUE);
            out = NULL;
            m = g_strdup_printf("suppression impossible : %s",
                                g_strerror(errno));
            goto done;
        }
        g_string_append_printf(out,
            "delete: ok\npath: %s\nremoved_lines: %u\nfile_hash: %s\n",
            path, line_count, fh);
    }
    result = g_string_free(out, FALSE);
    out = NULL;
    cdb_queue_text_result(c, tool_call_id, "delete", result, NULL, FALSE);

done:
    if (m != NULL) {
        cdb_queue_text_result(c, tool_call_id, "delete", m, NULL, FALSE);
        g_free(m);
    }
    g_free(result);
    g_free(fh);
    if (out != NULL)
        g_string_free(out, TRUE);
    if (off != NULL)
        g_array_free(off, TRUE);
    g_free(content);
    if (gerr != NULL)
        g_error_free(gerr);
    if (parser != NULL)
        g_object_unref(parser);
}

/* Outils fichiers : on ne valide ici que la FORME, pour ne pas rendre une
 * barre d'approbation inepte. L'etat du disque est verifie a l'execution. */
static void
cdb_dispatch_file_tool(LlmCore *c, const LlmToolCall *tc,
                       CdbSpecKind kind, LlmToolMode mode)
{
    JsonParser *parser;
    JsonObject *root;
    const char *path;
    char       *error = NULL;
    char       *summary = NULL;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser,
            tc->arguments_json != NULL ? tc->arguments_json : "",
            -1, NULL) ||
        json_parser_get_root(parser) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        error = g_strdup_printf("arguments JSON invalides pour %s.",
                                tc->name != NULL ? tc->name : "(sans nom)");
        goto done;
    }
    /* Un nul intercalaire dans text/content serait tronque silencieusement
     * par strlen : json-glib n'expose pas la longueur reelle de ses chaines.
     * On le refuse a la porte, en detectant la sequence d'chappement JSON qui la produit. */
    if (tc->arguments_json != NULL &&
        strstr(tc->arguments_json, "\\u0000") != NULL) {
        error = g_strdup("sequence NUL (\\u0000) refusee dans les arguments.");
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    path = cdb_json_str(root, "path");
    if (path == NULL || path[0] == '\0') {
        error = g_strdup("path manquant.");
        goto done;
    }
    if (path[0] != '/') {
        error = g_strdup("chemin absolu requis.");
        goto done;
    }

    if (kind == CDB_SPEC_READ) {
        long from = llm_json_int(root, "from_line", -1);
        long to   = llm_json_int(root, "to_line", -1);

        if (from < 1 || to < from) {
            error = g_strdup(
                "from_line/to_line invalides (1-based, to >= from >= 1).");
            goto done;
        }
        summary = g_strdup_printf("cdb_read  %s  %ld-%ld", path, from, to);
    } else if (kind == CDB_SPEC_REPLACE) {
        long        from = llm_json_int(root, "from_line", -1);
        long        to   = llm_json_int(root, "to_line", -1);
        const char *text = cdb_json_str(root, "text");
        const char *bh   = cdb_json_str(root, "block_hash");
        guint       removed, added;

        if (from < 1 || to < from) {
            error = g_strdup(
                "from_line/to_line invalides (1-based, to >= from >= 1).");
            goto done;
        }
        if (text == NULL) {
            error = g_strdup(
                "text manquant : mets text vide pour supprimer la plage.");
            goto done;
        }
        /* Pas de plafond, mais hash obligatoire : c'est lui qui prouve la
         * lecture des lignes precises que le modele va detruire. */
        if (bh == NULL || bh[0] == '\0') {
            error = g_strdup(
                "block_hash requis : lis la zone avec "
                "cdb_read(from_line, to_line).");
            goto done;
        }
        removed = (guint)(to - from + 1);
        added   = cdb_logical_lines(text, strlen(text));
        summary = g_strdup_printf(
            "cdb_replace  %s  %ld-%ld  -%u lignes / +%u lignes%s",
            path, from, to, removed, added,
            added == 0 ? "   [SUPPRESSION SANS REECRITURE]" : "");
    } else if (kind == CDB_SPEC_CREATE) {
        const char *content = cdb_json_str(root, "content");
        guint       added;

        if (content == NULL) {
            error = g_strdup(
                "content manquant : mets content vide pour creer un fichier "
                "vide.");
            goto done;
        }
        added = cdb_logical_lines(content, strlen(content));
        summary = g_strdup_printf(
            "cdb_create  %s  +%u ligne%s  (nouveau fichier)",
            path, added, added == 1 ? "" : "s");
    } else if (kind == CDB_SPEC_DELETE) {
        const char *fh = cdb_json_str(root, "file_hash");
        char       *cnt = NULL;
        gsize       cl = 0;
        guint       removed = 0;
        gboolean    absent = TRUE;

        /* Compter les lignes detruites ici, pour qu'Eric voie la taille du "
         * degat dans la barre d'approbation. Lecture seule. */
        if (g_file_get_contents(path, &cnt, &cl, NULL) && cnt != NULL) {
            GArray *o = g_array_new(FALSE, FALSE, sizeof(gsize));
            guint   lc = 0;

            absent = FALSE;
            cdb_line_offsets(cnt, cl, o, &lc);
            removed = lc;
            g_array_free(o, TRUE);
            g_free(cnt);
        }
        summary = g_strdup_printf(
            "cdb_delete  %s  -%u ligne%s%s%s", path, removed,
            removed == 1 ? "" : "s",
            absent ? "   [ABSENT OU ILLISIBLE]" : "",
            (fh != NULL && fh[0] != '\0') ? "   [DESTRUCTION CONFIRMEE]" : "");
    } else {
        long        before = llm_json_int(root, "before_line", -1);
        long        after  = llm_json_int(root, "after_line", -1);
        const char *text   = cdb_json_str(root, "text");
        const char *bh     = cdb_json_str(root, "before_hash");
        const char *ah     = cdb_json_str(root, "after_hash");
        guint       nlines;

        if (before < 0 || after < 0) {
            error = g_strdup(
                "before_line/after_line requis (0 = borne du fichier).");
            goto done;
        }
        if (text == NULL || text[0] == '\0') {
            error = g_strdup("text manquant ou vide.");
            goto done;
        }
        if (before > 0 && (bh == NULL || bh[0] == '\0')) {
            error = g_strdup(
                "before_hash requis : lis la ligne avec "
                "cdb_read(before_line, before_line).");
            goto done;
        }
        if (after > 0 && (ah == NULL || ah[0] == '\0')) {
            error = g_strdup(
                "after_hash requis : lis la ligne avec "
                "cdb_read(after_line, after_line).");
            goto done;
        }
        if (before > 0 && after > 0 && after != before + 1) {
            error = g_strdup(
                "before_line et after_line doivent etre adjacentes "
                "(after == before + 1).");
            goto done;
        }
        nlines = cdb_logical_lines(text, strlen(text));
        summary = g_strdup_printf(
            "cdb_insert  %s  apres %ld / avant %ld  (+%u ligne%s)",
            path, before, after, nlines, nlines > 1 ? "s" : "");
    }

    {
        CdbCmdSpec *spec = g_new0(CdbCmdSpec, 1);

        spec->tool_call_id = g_strdup(tc->id);
        spec->kind = kind;
        spec->mode = mode;
        spec->args_json = g_strdup(tc->arguments_json != NULL
                                       ? tc->arguments_json : "");
        spec->summary = summary;      /* la spec en devient proprietaire */
        summary = NULL;
        if (c->cmd_queue == NULL)
            c->cmd_queue = g_queue_new();
        g_queue_push_tail(c->cmd_queue, spec);
    }

done:
    if (error != NULL) {
        cdb_queue_text_result(c, tc->id, cdb_kind_label(kind),
                              error, NULL, FALSE);
        g_free(error);
    }
    g_free(summary);
    g_object_unref(parser);
}

/* Valide un appel natif et le place soit dans la file bash, soit dans une
 * réponse d'erreur formelle. Aucun appel ne reste sans réponse. */
static void
cdb_dispatch_native_call(LlmCore *c, const LlmToolCall *tc)
{
    JsonParser *parser = NULL;
    JsonObject *root = NULL;
    long        terminal = -1;
    const char *command = NULL;
    char       *error = NULL;
    CdbSpecKind kind = CDB_SPEC_BASH;
    LlmToolMode mode = LLM_TOOL_ASK;

    if (tc->id == NULL || tc->id[0] == '\0') {
        core_cdb_announce(c,
            "tool call ignoré : identifiant API manquant.");
        return;
    }
    if (c->answered_tools != NULL &&
        g_hash_table_contains(c->answered_tools, tc->id)) {
        core_cdb_announce(c, "tool call dupliqué ignoré par CDB.");
        return;
    }

    if (g_strcmp0(tc->name, CDB_TOOL_NAME) == 0)
        kind = CDB_SPEC_BASH;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_READ) == 0)
        kind = CDB_SPEC_READ;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_INSERT) == 0)
        kind = CDB_SPEC_INSERT;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_REPLACE) == 0)
        kind = CDB_SPEC_REPLACE;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_CREATE) == 0)
        kind = CDB_SPEC_CREATE;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_DELETE) == 0)
        kind = CDB_SPEC_DELETE;
    else {
        error = g_strdup_printf(
            "outil inconnu \"%s\" : cet outil n'existe pas dans CDB.",
            tc->name != NULL ? tc->name : "(sans nom)");
        goto done;
    }

    /* Mode effectif (profil actif). OFF = l'outil n'est pas annoncé au
     * modèle ; un appel ici est une hallucination : on répond quand même
     * (tout tool_call_id doit recevoir une réponse) sans exécuter. */
    mode = llm_tools_effective_mode(tc->name);
    if (mode == LLM_TOOL_OFF) {
        error = g_strdup_printf(
            "outil \"%s\" désactivé dans le profil courant.", tc->name);
        goto done;
    }

    /* Les outils fichiers passent par la MEME file que bash : l'ordre
     * demande -> approuve -> applique est identique pour tous, et un read
     * en ASK est vraiment demande a Eric desormais. */
    if (kind != CDB_SPEC_BASH) {
        cdb_dispatch_file_tool(c, tc, kind, mode);
        goto done;
    }

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser,
                                    tc->arguments_json != NULL
                                        ? tc->arguments_json : "",
                                    -1, NULL) ||
        json_parser_get_root(parser) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        error = g_strdup("arguments JSON invalides pour cdb_bash.");
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    terminal = llm_json_int(root, "terminal", -1);
    if (json_object_has_member(root, "command") &&
        JSON_NODE_HOLDS_VALUE(json_object_get_member(root, "command")) &&
        json_node_get_value_type(json_object_get_member(root, "command"))
            == G_TYPE_STRING)
        command = json_object_get_string_member(root, "command");

    if (terminal < 0 || terminal > 9) {
        error = g_strdup("terminal invalide : attendu entre 0 et 9.");
        goto done;
    }
    if (command == NULL || command[0] == '\0') {
        error = g_strdup("command manquante ou vide.");
        goto done;
    }

    {
        CdbCmdSpec *spec = g_new0(CdbCmdSpec, 1);

        spec->tool_call_id = g_strdup(tc->id);
        spec->kind = CDB_SPEC_BASH;
        spec->tab = (int)terminal;
        spec->cmd = g_strdup(command);
        spec->summary = g_strdup_printf("bash-%d $ %s",
                                        (int)terminal, command);
        spec->mode = mode;
        if (c->cmd_queue == NULL)
            c->cmd_queue = g_queue_new();
        g_queue_push_tail(c->cmd_queue, spec);
    }

done:
    if (error != NULL)
        cdb_queue_text_result(c, tc->id, "bash-?", error, NULL, FALSE);
    if (parser != NULL)
        g_object_unref(parser);
    g_free(error);
}

static gboolean
llm_finalize_pending_tools(LlmCore *c)
{
    GPtrArray *calls;
    gboolean   has_valid_id = FALSE;

    if (c->pending_tool_calls == NULL || c->pending_tool_calls->len == 0)
        return FALSE;

    g_ptr_array_sort(c->pending_tool_calls, pending_tool_cmp);
    calls = llm_tool_calls_new();

    for (guint i = 0; i < c->pending_tool_calls->len; i++) {
        LlmPendingToolCall *p =
            g_ptr_array_index(c->pending_tool_calls, i);
        LlmToolCall *tc;

        if (p->id == NULL || p->id[0] == '\0' || p->name == NULL) {
            core_cdb_announce(c,
                "tool call incomplet ignoré : id ou function.name absent.");
            continue;
        }
        tc = g_new0(LlmToolCall, 1);
        tc->id = g_steal_pointer(&p->id);
        tc->name = g_steal_pointer(&p->name);
        tc->arguments_json = g_string_free(p->arguments, FALSE);
        p->arguments = NULL;
        g_ptr_array_add(calls, tc);
        has_valid_id = TRUE;
    }

    if (calls->len > 0) {
        core_history_push_full(c, LLMACTOR_LLM, FALSE,
                               LLM_MSG_ASSISTANT_TOOL_CALLS,
                               c->reply->str, calls, NULL);
        for (guint i = 0; i < calls->len; i++)
            cdb_dispatch_native_call(c, g_ptr_array_index(calls, i));
    } else {
        g_ptr_array_unref(calls);
    }

    llm_core_clear_pending_tools(c);
    return has_valid_id;
}

/* Le bouton média et le chrono du tour ne décrivent pas une requête mais
 * la BOUCLE agentique. Un seul verdict, deux effets : vivante = icone
 * pause (un clic annule tout, decision ASK en attente comprise) et
 * compteur du tour qui tourne ; morte = play et horloge arretee.
 * stop_requested domine tout : une boucle annulée est morte même si sa
 * requête n'est pas encore libérée (le callback de lecture la jettera).
 * Les polls sont globaux et partagés par tous les cores : filtrer sur
 * pl->core et ignorer ceux que l'annulation a déjà répondus. */
gboolean
core_agent_loop_alive(LlmCore *c)
{
    if (c == NULL || c->stop_requested)
        return FALSE;
    if (c->cur_req != NULL || c->decision != NULL)
        return TRUE;
    if (c->cmd_queue != NULL && !g_queue_is_empty(c->cmd_queue))
        return TRUE;
    if (cdb_polls != NULL) {
        for (guint i = 0; i < cdb_polls->len; i++) {
            CdbPoll *pl = g_ptr_array_index(cdb_polls, i);

            if (pl->core == c && !pl->cancelled)
                return TRUE;
        }
    }
    return FALSE;
}

void
llm_cancel_current(LlmTile *t)
{
    LlmCore *c = t->core;
    guint    vi;

    /* 1. Flux réseau : close TCP + jet de ce qui arrive encore. */
    c->stop_requested = TRUE;
    if (c->cancel != NULL)
        g_cancellable_cancel(c->cancel);
    if (c->cur_req != NULL && c->cur_req->stream != NULL)
        g_input_stream_close_async(c->cur_req->stream, G_PRIORITY_DEFAULT,
                                   NULL, NULL, NULL);

    /* 2. Résultats déjà capturés mais pas encore livrés : ils répondent
     * réellement à leur appel, même si l'utilisateur coupe la boucle. */
    if (c->cdb_results != NULL) {
        for (GList *l = c->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            core_tool_result_commit(c, r->tool_call_id, r->text, TRUE);
        }
        for (GList *l = c->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            g_free(r->tool_call_id);
            g_free(r->label);
            g_free(r->raw_text);
            g_free(r->text);
            g_free(r);
        }
        g_queue_free(c->cdb_results);
        c->cdb_results = NULL;
    }

    /* 3. Décision en attente : réponse formelle, puis boîte verrouillée.
     * Ordre important : on LIVRE d'abord (la boîte encaisse le texte dans
     * sa zone output), on colore ensuite — resolve() n'a donc pas encore
     * perdu sa cible. Une annulation se rend en rouge : elle non plus
     * n'a pas été exécutée. */
    if (c->decision != NULL) {
        core_tool_result_commit(c, c->decision->spec->tool_call_id,
                                "Annulé par l'utilisateur.", TRUE);
        /* Livrer d'abord, colorer ensuite : la boîte a donc déjà reçu son
         * output quand resolve() la cherche. Elle doit par suite rester au
         * registre de la vue après sa réponse (voir llm.h) — sinon cette
         * annulation arriverait sur une boîte restée grise, sans
         * explication. Une annulation se rend en rouge : elle non plus
         * n'a été exécutée. */
        for (vi = 0; vi < c->views->len; vi++)
            llm_tile_decision_resolve(g_ptr_array_index(c->views, vi),
                                      c->decision->spec->tool_call_id,
                                      CDB_A_REFUSED);
        cdb_decision_free(c->decision);
        c->decision = NULL;
    }

    /* 4. File d'attente : chaque ID restant est répondu formellement. */
    if (c->cmd_queue != NULL) {
        for (GList *l = c->cmd_queue->head; l != NULL; l = l->next) {
            CdbCmdSpec *sp = l->data;

            core_tool_result_commit(c, sp->tool_call_id,
                                    "Annulé par l'utilisateur.", TRUE);
            cdb_cmd_spec_free(sp);
        }
        g_queue_free(c->cmd_queue);
        c->cmd_queue = NULL;
    }

    /* 5. Polls actifs : leurs IDs sont déjà répondus ; leur résultat
     * futur sera ignoré par answered_tools. Les timers se terminent seuls. */
    if (cdb_polls != NULL) {
        for (guint i = 0; i < cdb_polls->len; i++) {
            CdbPoll *pl = g_ptr_array_index(cdb_polls, i);

            if (pl->core == c) {
                pl->cancelled = TRUE;
                core_tool_result_commit(c, pl->tool_call_id,
                                        "Annulé par l'utilisateur.", TRUE);
            }
        }
    }

    llm_core_clear_pending_tools(c);
    for (vi = 0; vi < c->views->len; vi++)
        llm_busy_set(g_ptr_array_index(c->views, vi), FALSE);
}

/* Libère la requête une seule fois (les callbacks de complétion
 * peuvent arriver en double selon l'état du flux). */
void
llm_request_free(LlmRequest *req)
{
    /* La requête courante de la tuile meurt : plus rien à annuler. */
    if (req->core != NULL && req->core->cur_req == req) {
        req->core->cur_req = NULL;
        /* Point de passage UNIQUE du bouton : la derniere requete du fil
         * meurt ici, quel que soit le chemin — annulation, erreur reseau,
         * erreur HTTP, fin de flux sans tools. Si rien d'autre ne tient la
         * boucle (decision en attente, file non vide, poll vivant, requete
         * deja relancee par-dessous), l'icone retombe a play et le chrono
         * du tour s'arrete. Sans ca, un flux mort laissait pause affiche
         * pour toujours, avec un compteur qui tourne dans le vide. */
        if (!core_agent_loop_alive(req->core))
            for (guint vi = 0; vi < req->core->views->len; vi++)
                llm_busy_set(g_ptr_array_index(req->core->views, vi),
                             FALSE);
    }
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
 * alors le début de ligne sans log, et un fragment perdu rendait le
 * tool_call indétectable ET non condamnable (silence total de la boucle
 * agentique, bug constaté). */
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

    memset(&m, 0, sizeof(m));
    m.actor = actor;
    m.local = local;
    m.kind = LLM_MSG_TEXT;
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
        gboolean has_tools = llm_finalize_pending_tools(c);

        if (!has_tools)
            core_history_push(c, LLMACTOR_LLM, FALSE, c->reply->str);
        for (vi = 0; vi < c->views->len; vi++) {
            LlmTile *v = g_ptr_array_index(c->views, vi);

            hist_flush_reply(v);
            llm_slots_title_update(v);
            hist_append(v, "\n");
        }
        llm_request_free(req);
        if (has_tools)
            llm_cdb_next(c);
        /* Un tour de tools n'est PAS la fin de l'echange : llm_cdb_next()
         * vient peut-etre de relancer llm_send() par-dessous nous, et la
         * requete suivante est deja en vol. Poser FALSE ici a l'aveugle
         * peignait play pendant ce tour-la et coupait le chrono — d'ou un
         * second envoi possible, donc DEUX requetes concurrentlyes sur le
         * meme core (cur_req ecrase, deux flux SSE dans reply). Le bouton
         * suit desormais la boucle, pas la requete qui vient de mourir. */
        {
            gboolean alive = core_agent_loop_alive(c);

            for (vi = 0; vi < c->views->len; vi++)
                llm_busy_set(g_ptr_array_index(c->views, vi), alive);
        }
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

static void
cdb_poll_teardown(CdbPoll *pl)
{
    if (pl == NULL)
        return;
    cdd_poll_unregister(pl);
    bash_panel_set_busy((guint)pl->tab, FALSE);
    g_free(pl->tool_call_id);
    g_free(pl->tab_label);
    g_free(pl->prev_tail);
    g_free(pl->pending_cmd);
    g_free(pl);
}



void
cdb_poll_finish(CdbPoll *pl, const char *text, gboolean is_output)
{
    CdbResult *r;
    gboolean    blank;

    cdd_poll_unregister(pl);
    bash_panel_set_busy((guint)pl->tab, FALSE);

    r = g_new0(CdbResult, 1);
    r->tool_call_id = g_steal_pointer(&pl->tool_call_id);
    r->label = g_steal_pointer(&pl->tab_label);
    blank = text == NULL || text[strspn(text, " \t\r\n")] == '\0';
    if (is_output) {
        r->raw_text = g_strdup(text);
        r->text = blank ? NULL : g_strdup(text);
    } else {
        r->raw_text = NULL;
        r->text = g_strdup(text != NULL ? text : "(erreur sans message)");
    }

    if (pl->core->cdb_results == NULL)
        pl->core->cdb_results = g_queue_new();
    g_queue_push_tail(pl->core->cdb_results, r);

    /* AllowPlus (bash) : la capture est faite, on remplace l'onglet par
     * un shell FRAIS — la prochaine commande repart d'un environnement
     * propre, comme si Éric avait cliqué « x » puis r ouvert. */
    if (pl->allowplus)
        bash_panel_reset_tab((guint)pl->tab);

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

    if (pl->cancelled || pl->core->stop_requested) {
        cdb_poll_teardown(pl);
        return G_SOURCE_REMOVE;
    }

    /* Poll purgé (tuile détruite) : se retire silencieusement. */
    if (cdb_polls == NULL || !g_ptr_array_find(cdb_polls, pl, NULL)) {
        cdb_poll_teardown(pl);
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

        cdb_poll_finish(pl, note, FALSE);
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

    if (pl->cancelled || pl->core->stop_requested) {
        cdb_poll_teardown(pl);
        return G_SOURCE_REMOVE;
    }

    /* Poll purgé (tuile détruite) : se retire silencieusement. */
    if (cdb_polls == NULL ||
        !g_ptr_array_find(cdb_polls, pl, NULL)) {
        cdb_poll_teardown(pl);
        return G_SOURCE_REMOVE;
    }

    if (prompt_re == NULL)
        prompt_re = g_regex_new(CDB_PROMPT_RE, 0, 0, NULL);

    if (!bash_panel_term_alive((guint)pl->tab)) {
        note = g_strdup_printf(
            "le terminal %s a été fermé pendant l'exécution.",
            pl->tab_label);
        cdb_poll_finish(pl, note, FALSE);
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

    cdb_poll_finish(pl, bounded, TRUE);

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
    /* Attribution applicative : tous providers (voir #defines en tete). */
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "HTTP-Referer", LLM_APP_REFERER);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "X-OpenRouter-Title", LLM_APP_TITLE);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "X-OpenRouter-Categories",
                                LLM_APP_CATEGORIES);

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
 * infini des appels d'outils. */
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
    GQueue *q = c->cdb_results;

    /* Boucle annulée : les IDs ouverts ont déjà reçu une réponse formelle.
     * Les résultats tardifs sont jetés et surtout, pas de re-requête. */
    if (c->stop_requested) {
        c->cdb_results = NULL;
        if (q != NULL) {
            for (GList *l = q->head; l != NULL; l = l->next) {
                CdbResult *r = l->data;

                g_free(r->tool_call_id);
                g_free(r->label);
                g_free(r->raw_text);
                g_free(r->text);
                g_free(r);
            }
            g_queue_free(q);
        }
        return;
    }

    c->cdb_results = NULL;
    if (q == NULL) {
        if (c->views->len > 0)
            llm_cdb_requery(g_ptr_array_index(c->views, 0));
        return;
    }
    if (g_queue_is_empty(q)) {
        g_queue_free(q);
        if (c->views->len > 0)
            llm_cdb_requery(g_ptr_array_index(c->views, 0));
        return;
    }


    /* Déduplication chronologique, par terminal :
     * si A est contenu exactement dans B, retire A de B ;
     * si le reste est blanc, B devient NULL. */
    for (GList *li = q->head; li != NULL; li = li->next) {
        CdbResult *ri = li->data;
        gchar     *wire;

        if (ri->raw_text == NULL)
            continue;
        wire = g_strdup(ri->raw_text);
        for (GList *lj = q->head; lj != li; lj = lj->next) {
            CdbResult *rj = lj->data;
            gchar     *pos;
            gsize      old_len;

            gchar *needle;

            if (rj->raw_text == NULL || rj->raw_text[0] == '\0' ||
                g_strcmp0(rj->label, ri->label) != 0)
                continue;
            /* Needle strippé : les captures d'un même bloc peuvent
             * différer d'un \n de bordure (début d'écran). Sans ça,
             * le cas "tout est déjà vu -> NULL" ne déclenche jamais. */
            needle = g_strstrip(g_strdup(rj->raw_text));
            old_len = strlen(needle);
            if (old_len == 0) {
                g_free(needle);
                continue;
            }
            while ((pos = strstr(wire, needle)) != NULL)
                memmove(pos, pos + old_len,
                        strlen(pos + old_len) + 1);
            g_free(needle);
        }

        g_free(ri->text);
        ri->text = wire[strspn(wire, " \t\r\n")] == '\0'
                       ? NULL : g_steal_pointer(&wire);
        g_free(wire);
    }

    for (GList *l = q->head; l != NULL; l = l->next) {
        CdbResult *r = l->data;

        core_tool_result_commit(c, r->tool_call_id, r->text, !r->shown);
    }
    for (GList *l = q->head; l != NULL; l = l->next) {
        CdbResult *r = l->data;

        g_free(r->tool_call_id);
        g_free(r->label);
        g_free(r->raw_text);
        g_free(r->text);
        g_free(r);
    }
    g_queue_free(q);

    if (c->views->len > 0)
        llm_cdb_requery(g_ptr_array_index(c->views, 0));
}

/* Issue refusée : la boîte devient rouge dans toutes les vues, et la
 * raison du refus EST la réponse livrée au modèle — elle va donc aussi se
 * lire dans la zone output de la boîte. Un refus n'est pas un silence. */
void
cdb_decision_refuse(LlmCore *c, CdbDecision *d)
{
    const char *note =
        "Éric a REFUSÉ cet appel d'outil. Ce n'est pas un bug : "
        "c'est une décision. Adapte-toi et propose autre chose.";

    if (c == NULL || c->decision != d || d->state != CDB_A_PENDING)
        return;
    d->state = CDB_A_REFUSED;
    for (guint vi = 0; vi < c->views->len; vi++)
        llm_tile_decision_resolve(g_ptr_array_index(c->views, vi),
                                  d->spec->tool_call_id, CDB_A_REFUSED);
    /* La note d'Éric entre dans la zone output de la boîte rouge : le
     * refus aussi est une réponse, et elle doit rester lisible dans le
     * fil, pas seulement reçue par le modèle. */
    core_tool_result_commit(c, d->spec->tool_call_id, note, TRUE);
    cdb_decision_free(d);
    c->decision = NULL;
    llm_cdb_next(c);
}

/* Lance l'exécution d'une commande déjà validée. Chemin unique pour
 * ASK (après clic « Exécuter ») et ALLOW/ALLOWPLUS (auto, sans UI).
 * Ne touche pas à la décision : l'appelant s'en charge. En cas d'échec
 * synchrone (terminal absent), répond au tool_call_id et rappelle
 * llm_cdb_next ; sinon le poll actif le fera à la fin. */
static void
cdb_execute(LlmCore *c, const char *tool_call_id, int tab,
            const char *cmd, gboolean allowplus)
{
    CdbPoll *pl = g_new0(CdbPoll, 1);
    char    *note;

    pl->core = c;
    pl->tool_call_id = g_strdup(tool_call_id);
    pl->tab = tab;
    pl->allowplus = allowplus;
    pl->tab_label = g_strdup_printf("bash-%d", tab);

    bash_panel_ensure_tabs((guint)(tab + 1));

    if (!bash_panel_exec_tab_possible()) {
        note = g_strdup_printf(
            "terminal %s indisponible (panneau bash absent ?)",
            pl->tab_label);
        core_tool_result_commit(c, pl->tool_call_id, note, TRUE);
        g_free(note);
        g_free(pl->tool_call_id);
        g_free(pl->tab_label);
        g_free(pl);
        llm_cdb_next(c);
        return;
    }

    if (!bash_panel_term_ready((guint)tab)) {
        pl->pending_cmd = g_strdup(cmd);
        cdb_poll_register(pl);
        g_timeout_add(CDB_POLL_MS, cdb_spawn_wait_tick, pl);
    } else if (bash_panel_exec_tab((guint)tab, cmd)) {
        bash_panel_set_busy((guint)tab, TRUE);
        cdb_poll_register(pl);
        g_timeout_add(CDB_POLL_MS, cdb_poll_tick, pl);
    } else {
        note = g_strdup_printf(
            "terminal %s indisponible (panneau bash absent ?)",
            pl->tab_label);
        core_tool_result_commit(c, pl->tool_call_id, note, TRUE);
        g_free(note);
        g_free(pl->tool_call_id);
        g_free(pl->tab_label);
        g_free(pl);
        llm_cdb_next(c);
    }
}

/* Exécuter une spec déjà validée. Chemin unique pour ASK (après clic) et
 * ALLOW/ALLOWPLUS (direct). Ne libère rien : l'appelant possède la spec.
 * Bash est asynchrone ; les outils fichiers sont synchrones et ont déjà
 * rendu leur résultat en revenant ici. */
static void
cdb_run_spec(LlmCore *c, CdbCmdSpec *sp, gboolean allowplus)
{
    switch (sp->kind) {
    case CDB_SPEC_BASH:
        cdb_execute(c, sp->tool_call_id, sp->tab, sp->cmd, allowplus);
        break;
    case CDB_SPEC_READ:
        cdb_tool_file_read(c, sp->tool_call_id, sp->args_json);
        break;
    case CDB_SPEC_INSERT:
        cdb_tool_file_insert(c, sp->tool_call_id, sp->args_json);
        break;
    case CDB_SPEC_REPLACE:
        cdb_tool_file_replace(c, sp->tool_call_id, sp->args_json);
        break;
    case CDB_SPEC_CREATE:
        cdb_tool_file_create(c, sp->tool_call_id, sp->args_json);
        break;
    case CDB_SPEC_DELETE:
        cdb_tool_file_delete(c, sp->tool_call_id, sp->args_json);
        break;
    }
}

/* Issue d'une décision approuvée. Plus rien ne passe par un GtkButton :
 * la boîte interactive d'une vue rapporte un choix, le core tranche l'état
 * de la décision et rediffuse la couleur à TOUTES les vues (loi du
 * miroir). La tuile, elle, ne connait toujours aucun outil. */
void
cdb_decision_approve(LlmCore *c, CdbDecision *d)
{
    gboolean is_bash;

    if (c == NULL || c->decision != d || d->state != CDB_A_PENDING)
        return;
    d->state = CDB_A_APPROVED;
    /* L'ID est passé : une vue peut avoir plusieurs boîtes ouvertes, il
     * faut dire laquelle verdit. Livrer AVANT de couleur serait possible
     * (la boîte reste au registre), mais l'ordre lire→exécuter rend le fil
     * plus honnête : la décision est marquée, puis l'action part. */
    for (guint vi = 0; vi < c->views->len; vi++)
        llm_tile_decision_resolve(g_ptr_array_index(c->views, vi),
                                  d->spec->tool_call_id, CDB_A_APPROVED);

    /* ASK approuvé = exécution sans effet « plus » (le reset n'a de sens
     * qu'en mode AllowPlus, décidé au dispatch, pas ici). */
    is_bash = (d->spec != NULL && d->spec->kind == CDB_SPEC_BASH);
    cdb_run_spec(c, d->spec, FALSE);
    cdb_decision_free(d);
    c->decision = NULL;
    /* Bash : asynchrone, cdb_execute (ou son poll) avancera la file.
     * Fichier : synchrone, c'est donc ici qu'on la reprend. */
    if (!is_bash)
        llm_cdb_next(c);
    else
        core_sync_buttons(c); /* le poll roule : le bouton doit le dire */
}

/* Avance la file : commande suivante → approbation ; vide →
 * livraison des résultats pendants (dédupliqués), puis
 * re-interrogation du modèle. */
static void
cdb_next_step(LlmCore *c)
{
    /* Une seule file pour tous les outils : l'ordre demandé -> approuvé
     * -> appliqué est préservé même en mélangeant bash et éditions de
     * fichiers. */
    for (;;) {
        CdbCmdSpec *sp;
        gboolean    is_bash;

        if (c->cmd_queue == NULL || g_queue_is_empty(c->cmd_queue)) {
            llm_cdb_results_flush(c);
            return;
        }
        sp = g_queue_pop_head(c->cmd_queue);
        is_bash = (sp->kind == CDB_SPEC_BASH);

        /* ALLOW / ALLOWPLUS : demande ACCEPTEE D'AVANCE. Elle s'exécute
         * sans attendre Éric, mais ce n'est pas une absence de demande :
         * c'est une demande accordée d'avance, et elle reste une demande.
         * Chaque vue la montre donc dans SA boîte — zone déjà verte,
         * libellée « autorisé » (personne n'a cliqué : « exécuté » serait
         * un mensonge) — et l'output y entrera par le même chemin que pour
         * un ASK, sous le même tool_call_id. */
        if (sp->mode != LLM_TOOL_ASK) {
            gboolean allowplus = (sp->mode == LLM_TOOL_ALLOWPLUS);

            for (guint vi = 0; vi < c->views->len; vi++)
                llm_tile_box_auto(g_ptr_array_index(c->views, vi),
                                  sp->summary, sp->tool_call_id, allowplus);
            cdb_run_spec(c, sp, allowplus);
            cdb_cmd_spec_free(sp);
            if (is_bash)
                return;   /* le poll rappellera llm_cdb_next */
            continue;     /* fichier : déjà fini, on enchaîne */
        }

        /* ASK : la décision POSSÈDE la spec. Rien n'est copié ici, donc
         * rien ne peut être oublié à la libération. */
        c->decision = g_new0(CdbDecision, 1);
        c->decision->spec = sp;
        c->decision->state = CDB_A_PENDING;
        for (guint vi = 0; vi < c->views->len; vi++)
            llm_tile_decision_render(g_ptr_array_index(c->views, vi));
        return;
    }
}

/* Diffuse l'état de la boucle sur TOUTES les vues : le bouton n'est jamais
 * écrit localement par une tuile, il est RE-LU du core (loi du miroir).
 * Seul chemin qui garde les vues d'accord quand la boucle avance hors
 * requête réseau — outil en cours d'exécution, décision posée. */
void
core_sync_buttons(LlmCore *c)
{
    gboolean alive = core_agent_loop_alive(c);

    for (guint vi = 0; vi < c->views->len; vi++)
        llm_busy_set(g_ptr_array_index(c->views, vi), alive);
}

/* Avancer la file change l'etat du bouton dans TOUS les cas : bash lance
 * (poll vivant = pause), decision ASK posee (pause : le clic annulera la
 * decision et repondra formellement son tool_call_id), file epuisee
 * (re-requete = pause, boucle finie = play). Le peindre a la sortie de
 * l'etape, pas dans chaque branche, ferme la fenetre du tour de tools. */
void
llm_cdb_next(LlmCore *c)
{
    cdb_next_step(c);
    core_sync_buttons(c);
}

/* Schéma de l'outil cdb_bash (canal natif). */
static void
tools_schema_cdb_bash(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "cdb_bash");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(
        builder,
        "Exécute une commande shell dans un terminal CDB. La commande est "
        "soumise à l'approbation d'Éric avant exécution.");
    json_builder_set_member_name(builder, "parameters");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "object");
    json_builder_set_member_name(builder, "properties");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "terminal");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "integer");
    json_builder_set_member_name(builder, "minimum");
    json_builder_add_int_value(builder, 0);
    json_builder_set_member_name(builder, "maximum");
    json_builder_add_int_value(builder, 9);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(
        builder, "Numéro du terminal CDB (0 à 9).");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "command");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(
        builder, "Commande shell complète à exécuter.");
    json_builder_end_object(builder);

    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "required");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "terminal");
    json_builder_add_string_value(builder, "command");
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    json_builder_end_object(builder);
    json_builder_end_object(builder);
}

/* Schéma de l'outil cdb_read. */
static void
tools_schema_cdb_read(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "cdb_read");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Lit une plage exacte de lignes d'un fichier texte depuis le "
        "disque (chemin absolu). from_line/to_line 1-based inclusifs. "
        "Retourne les lignes et un hash court (4 caracteres base36) des "
        "octets exacts de la plage, a rejouer dans cdb_insert/cdb_replace.");
    json_builder_set_member_name(builder, "parameters");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "object");
    json_builder_set_member_name(builder, "properties");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "path");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "from_line");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "integer");
    json_builder_set_member_name(builder, "minimum");
    json_builder_add_int_value(builder, 1);
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "to_line");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "integer");
    json_builder_set_member_name(builder, "minimum");
    json_builder_add_int_value(builder, 1);
    json_builder_end_object(builder);

    json_builder_end_object(builder); /* properties */
    json_builder_set_member_name(builder, "required");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "path");
    json_builder_add_string_value(builder, "from_line");
    json_builder_add_string_value(builder, "to_line");
    json_builder_end_array(builder);
    json_builder_end_object(builder); /* parameters */
    json_builder_end_object(builder); /* function */
    json_builder_end_object(builder); /* tool */
}

/* Schéma de l'outil cdb_insert. */
static void
tools_schema_cdb_insert(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "cdb_insert");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Inserer du texte verbatim entre deux lignes ADJACENTES d'un "
        "fichier texte du disque (chemin absolu). Le modele doit avoir lu "
        "la ligne avant et la ligne apres SEPAREMENT (cdb_read avec "
        "from_line==to_line) et rejouer leurs deux hashes. before_line=0 "
        "ou after_line=0 designe une borne du fichier : pas de ligne "
        "reelle, donc pas de hash. Les sauts de ligne de text "
        "appartiennent au modele. Ne cree pas le fichier et n'ecrit "
        "jamais dans le dirty de l'editeur.");
    json_builder_set_member_name(builder, "parameters");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "object");
    json_builder_set_member_name(builder, "properties");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "path");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "before_line");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "integer");
    json_builder_set_member_name(builder, "minimum");
    json_builder_add_int_value(builder, 0);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Ligne reelle apres laquelle inserer ; 0 = tete du fichier.");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "before_hash");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Hash rendu par cdb_read(before_line,before_line). A omettre "
        "seulement si before_line=0.");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "after_line");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "integer");
    json_builder_set_member_name(builder, "minimum");
    json_builder_add_int_value(builder, 0);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Ligne reelle avant laquelle inserer ; vaut before_line+1, ou 0 "
        "pour inserer en fin de fichier.");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "after_hash");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Hash rendu par cdb_read(after_line,after_line). A omettre "
        "seulement si after_line=0.");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "text");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Texte insere tel quel, sauts de ligne compris.");
    json_builder_end_object(builder);

    json_builder_end_object(builder); /* properties */
    json_builder_set_member_name(builder, "required");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "path");
    json_builder_add_string_value(builder, "before_line");
    json_builder_add_string_value(builder, "after_line");
    json_builder_add_string_value(builder, "text");
    json_builder_end_array(builder);
    json_builder_end_object(builder); /* parameters */
    json_builder_end_object(builder); /* function */
    json_builder_end_object(builder); /* tool */
}

/* Schéma de l'outil cdb_replace. */
static void
tools_schema_cdb_replace(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "cdb_replace");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Remplace une plage de lignes ENTIERES (from_line..to_line, inclus, "
        "1-based) d'un fichier texte du disque par un texte verbatim. Le "
        "block_hash doit etre le hash rendu par un cdb_read(portant exactement "
        "les memes from_line et to_line) : c'est la preuve que la zone a ete "
        "lue. text vide supprime la plage. Aucun plafond de taille, mais "
        "aucun hash, hash faux ou plage hors fichier = refus. N'ecrit jamais "
        "dans le dirty de l'editeur.");
    json_builder_set_member_name(builder, "parameters");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "object");
    json_builder_set_member_name(builder, "properties");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "path");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "from_line");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "integer");
    json_builder_set_member_name(builder, "minimum");
    json_builder_add_int_value(builder, 1);
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "to_line");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "integer");
    json_builder_set_member_name(builder, "minimum");
    json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Derniere ligne remplacee, inclusive. Doit exister.");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "block_hash");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Hash (4 caracteres base36) rendu par cdb_read de la MEME plage.");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "text");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Nouveau texte, insere tel quel. Chaine vide = suppression.");
    json_builder_end_object(builder);

    json_builder_end_object(builder); /* properties */
    json_builder_set_member_name(builder, "required");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "path");
    json_builder_add_string_value(builder, "from_line");
    json_builder_add_string_value(builder, "to_line");
    json_builder_add_string_value(builder, "block_hash");
    json_builder_add_string_value(builder, "text");
    json_builder_end_array(builder);
    json_builder_end_object(builder); /* parameters */
    json_builder_end_object(builder); /* function */
    json_builder_end_object(builder); /* tool */
}

/* Schéma de l'outil cdb_create. */
static void
tools_schema_cdb_create(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "cdb_create");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Cree un fichier texte NEUF sur le disque (chemin absolu) avec ce "
        "content verbatim. Refuse si le fichier existe deja (utilise "
        "cdb_replace) et si le dossier parent est absent (ne cree jamais de "
        "repertoire). L'ecriture est exclusive (O_EXCL) : aucune course ne "
        "peut ecraser un fichier apparu entre-temps. content vide cree un "
        "fichier vide.");
    json_builder_set_member_name(builder, "parameters");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "object");
    json_builder_set_member_name(builder, "properties");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "path");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "content");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Contenu ecrit tel quel, sauts de ligne compris. Chaine vide = "
        "fichier vide.");
    json_builder_end_object(builder);

    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "required");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "path");
    json_builder_add_string_value(builder, "content");
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    json_builder_end_object(builder);
    json_builder_end_object(builder);
}

/* Schéma de l'outil cdb_delete. */
static void
tools_schema_cdb_delete(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "cdb_delete");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Detruit un fichier du disque (chemin absolu) en DEUX passes. "
        "Sans file_hash : ne supprime RIEN et rend l'empreinte courante du "
        "fichier. Avec un file_hash qui correspond encore : supprime. Avec "
        "un hash perime : refus. Refuse les repertoires, les liens "
        "symboliques et tout ce qui n'est pas un fichier regulier.");
    json_builder_set_member_name(builder, "parameters");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "object");
    json_builder_set_member_name(builder, "properties");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "path");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "file_hash");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        "Empreinte rendue par le premier cdb_delete(path). Omettre pour la "
        "demande de confirmation.");
    json_builder_end_object(builder);

    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "required");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "path");
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    json_builder_end_object(builder);
    json_builder_end_object(builder);
}

char *
llm_body_build(LlmTile *t)
{
    JsonBuilder *builder;
    JsonNode    *root_node;
    char        *out;
    char        *base_persona;
    char        *persona;

    static const char tools_policy[] =
        "\n\n# Outils CDB natifs\n\n"
        "Utilise exclusivement les outils natifs pour agir.\n"
        "Un résultat tool avec content:null signifie qu'il n'y a aucun "
        "contenu nouveau par rapport aux résultats précédents du même "
        "terminal.\n\n"
        "## cdb_bash\n"
        "Exécute une commande shell dans un terminal CDB (0-9).\n\n"
        "## cdb_read\n"
        "Lit une plage exacte de lignes (chemin absolu, 1-based inclusif). "
        "Retourne les lignes + un hash court (4 caractères base36) couvrant "
        "les octets exacts de la plage lue. Ce hash prouve que tu as lu la "
        "zone et devra être rejoué par les outils d'écriture. Pour obtenir "
        "le hash d'UNE ligne, lis exactement cette ligne "
        "(from_line==to_line).\n\n"
        "## cdb_insert\n"
        "Insere du texte entre deux lignes adjacentes d'un fichier du "
        "disque. Tu DOIS avoir lu la ligne avant et la ligne apres, "
        "chacune par son propre cdb_read(N,N), et rejouer leurs deux "
        "hashes. before_line=0 ou after_line=0 designe une borne du "
        "fichier : la, aucun hash. Le texte est insere verbatim, comme "
        "cat : les sauts de ligne t'appartiennent, et un texte qui ne "
        "finit pas par un saut de ligne fusionne avec la ligne suivante. "
        "CDB te rend le range REEL et ne frappe un hash que sur les "
        "lignes entierement fournies par toi (authored_range) : "
        "une ligne melant ton texte a du contenu existant reste "
        "sans hash, donc sans droit d'ecriture immediate.\n\n"
        "## cdb_replace\n"
        "Remplace les lignes from_line..to_line (inclus, 1-based) par ton "
        "texte verbatim. block_hash est OBLIGATOIRE et doit venir d'un "
        "cdb_read de cette plage exacte : sans lui, ou s'il ne correspond "
        "plus, refus. La plage remplacee inclut le saut de ligne terminal "
        "de to_line. text vide supprime les lignes. Un refus ne te donnera "
                "jamais le hash courant : relis.\n\n"
        "## cdb_create\n"
        "Cree un fichier NEUF (chemin absolu) avec content verbatim. Refuse "
        "si le fichier existe ou si le parent manque ; ne cree jamais de "
        "repertoire. Tout le contenu venant de toi, authored_range couvre le "
        "fichier entier.\n\n"
        "## cdb_delete\n"
        "DEUX PASSES, obligatoires : cdb_delete(path) ne supprime rien et te "
        "rend file_hash ; cdb_delete(path, file_hash) supprime seulement si "
        "l'empreinte est encore la bonne. Ce hash-la n'est pas une preuve de "
        "lecture : il certifie que le fichier n'a pas change entre ta "
        "decouverte et ta destruction. repertoire et lien symbolique = refus.\n";

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

    /* Canal tools natif : piloté par les préfs du PROFIL ACTIF. Un outil
     * en OFF n'est pas annoncé (inexistant pour le modèle). Si aucun
     * outil n'est annoncé, on omet tools/tool_choice ET la policy persona,
     * pour rester compatible avec les providers qui refusent tools. */
    {
        LlmToolProfile     prof = llm_config_active_profile();
        GPtrArray         *prefs = llm_tools_prefs_load();
        const LlmToolPref *bash_pref = llm_tools_pref_find(prefs,
                                                           "cdb_bash");
        LlmToolMode        bash_mode = llm_tool_pref_mode(bash_pref, prof);
        gboolean           announce_bash = (bash_mode != LLM_TOOL_OFF);
        const LlmToolPref *read_pref = llm_tools_pref_find(prefs,
                                                           "cdb_read");
        LlmToolMode        read_mode = llm_tool_pref_mode(read_pref, prof);
        gboolean           announce_read = (read_mode != LLM_TOOL_OFF);
        const LlmToolPref *ins_pref = llm_tools_pref_find(prefs,
                                                         "cdb_insert");
        LlmToolMode        ins_mode = llm_tool_pref_mode(ins_pref, prof);
        gboolean           announce_insert = (ins_mode != LLM_TOOL_OFF);
        const LlmToolPref *rep_pref = llm_tools_pref_find(prefs,
                                                          "cdb_replace");
        LlmToolMode        rep_mode = llm_tool_pref_mode(rep_pref, prof);
        gboolean           announce_replace = (rep_mode != LLM_TOOL_OFF);
        const LlmToolPref *cre_pref = llm_tools_pref_find(prefs,
                                                          "cdb_create");
        LlmToolMode        cre_mode = llm_tool_pref_mode(cre_pref, prof);
        gboolean           announce_create = (cre_mode != LLM_TOOL_OFF);
        const LlmToolPref *del_pref = llm_tools_pref_find(prefs,
                                                          "cdb_delete");
        LlmToolMode        del_mode = llm_tool_pref_mode(del_pref, prof);
        gboolean           announce_delete = (del_mode != LLM_TOOL_OFF);
        guint              n_enabled = (announce_bash ? 1 : 0) +
                                       (announce_read ? 1 : 0) +
                                       (announce_insert ? 1 : 0) +
                                       (announce_replace ? 1 : 0) +
                                       (announce_create ? 1 : 0) +
                                       (announce_delete ? 1 : 0);

        if (n_enabled > 0) {
            json_builder_set_member_name(builder, "tools");
            json_builder_begin_array(builder);
            if (announce_bash)
                tools_schema_cdb_bash(builder);
            if (announce_read)
                tools_schema_cdb_read(builder);
            if (announce_insert)
                tools_schema_cdb_insert(builder);
            if (announce_replace)
                tools_schema_cdb_replace(builder);
            if (announce_create)
                tools_schema_cdb_create(builder);
            if (announce_delete)
                tools_schema_cdb_delete(builder);
            /* futurs outils : autres schémas + tests de mode ici */
            json_builder_end_array(builder);

            json_builder_set_member_name(builder, "tool_choice");
            json_builder_add_string_value(builder, "auto");

            json_builder_set_member_name(builder, "messages");
            json_builder_begin_array(builder);

            /* Persona utilisateur + politique du canal tools. */
            base_persona = llm_persona_load(t);            persona = g_strconcat(base_persona != NULL
                                      ? base_persona : "",
                                  tools_policy, NULL);
            g_free(base_persona);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "role");
            json_builder_add_string_value(builder, "system");
            json_builder_set_member_name(builder, "content");
            json_builder_add_string_value(builder, persona);
            json_builder_end_object(builder);
            g_free(persona);
        } else {
            json_builder_set_member_name(builder, "messages");
            json_builder_begin_array(builder);

            base_persona = llm_persona_load(t);
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "role");
            json_builder_add_string_value(builder, "system");
            json_builder_set_member_name(builder, "content");
            json_builder_add_string_value(builder, base_persona);
            json_builder_end_object(builder);
            g_free(base_persona);
        }
        llm_tools_prefs_free(prefs);
    }

    for (guint i = 0; i < t->core->history->len; i++) {
        LlmMsg *m = &g_array_index(t->core->history, LlmMsg, i);

        if (m->local)
            continue;

        json_builder_begin_object(builder);

        if (m->kind == LLM_MSG_TOOL_RESULT) {
            json_builder_set_member_name(builder, "role");
            json_builder_add_string_value(builder, "tool");
            json_builder_set_member_name(builder, "tool_call_id");
            json_builder_add_string_value(builder, m->tool_call_id);
            json_builder_set_member_name(builder, "content");
            if (m->content != NULL)
                json_builder_add_string_value(builder, m->content);
            else
                json_builder_add_null_value(builder);
        } else if (m->kind == LLM_MSG_ASSISTANT_TOOL_CALLS) {
            json_builder_set_member_name(builder, "role");
            json_builder_add_string_value(builder, "assistant");
            json_builder_set_member_name(builder, "content");
            if (m->content != NULL && m->content[0] != '\0')
                json_builder_add_string_value(builder, m->content);
            else
                json_builder_add_null_value(builder);

            json_builder_set_member_name(builder, "tool_calls");
            json_builder_begin_array(builder);
            for (guint k = 0; k < m->tool_calls->len; k++) {
                LlmToolCall *tc = g_ptr_array_index(m->tool_calls, k);

                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder, tc->id);
                json_builder_set_member_name(builder, "type");
                json_builder_add_string_value(builder, "function");
                json_builder_set_member_name(builder, "function");
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "name");
                json_builder_add_string_value(builder, tc->name);
                json_builder_set_member_name(builder, "arguments");
                json_builder_add_string_value(builder, tc->arguments_json);
                json_builder_end_object(builder);
                json_builder_end_object(builder);
            }
            json_builder_end_array(builder);
        } else {
            const char *wire = llm_msg_wire_role(m->actor);

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
 * dans t->core->reply et relançait indéfiniment les MÊMES appels
 * d'outils (comptage infini, exécutions multiples). */
/* Ouverture d'un tour : état au core, reset d'affichage par vue. */
void
llm_core_turn_new(LlmCore *c)
{
    llm_core_clear_pending_tools(c);
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

        llm_msg_clear(m);
    }
    g_array_set_size(t->core->history, 0);
    if (t->core->answered_tools != NULL)
        g_hash_table_remove_all(t->core->answered_tools);
    llm_live_wipe();
}

/* Purge les files d'outils (elles référençaient l'ancien fil). */
void
llm_queues_purge(LlmTile *t)
{
    if (t->core->cmd_queue != NULL) {
        for (GList *l = t->core->cmd_queue->head; l != NULL; l = l->next) {
            CdbCmdSpec *s = l->data;

            cdb_cmd_spec_free(s);
        }
        g_queue_free(t->core->cmd_queue);
        t->core->cmd_queue = NULL;
    }
    if (t->core->cdb_results != NULL) {
        for (GList *l = t->core->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            g_free(r->tool_call_id);
            g_free(r->label);
            g_free(r->raw_text);
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
    c->answered_tools = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    return c;
}

void
llm_core_free(LlmCore *c)
{
    guint i;

    if (c == NULL)
        return;
    llm_live_save(c);
    llm_core_clear_pending_tools(c);
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

            llm_msg_clear(m);
        }
        g_array_free(c->history, TRUE);
    }
    if (c->cmd_queue != NULL) {
        for (GList *l = c->cmd_queue->head; l != NULL; l = l->next) {
            CdbCmdSpec *s = l->data;

            cdb_cmd_spec_free(s);
        }
        g_queue_free(c->cmd_queue);
    }
    if (c->cdb_results != NULL) {
        for (GList *l = c->cdb_results->head; l != NULL; l = l->next) {
            CdbResult *r = l->data;

            g_free(r->tool_call_id);
            g_free(r->label);
            g_free(r->raw_text);
            g_free(r->text);
            g_free(r);
        }
        g_queue_free(c->cdb_results);
    }
    if (c->answered_tools != NULL)
        g_hash_table_unref(c->answered_tools);
    if (c->views != NULL)
        g_ptr_array_unref(c->views);
    g_free(c);
}
