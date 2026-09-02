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
#include "mem.h"
#include "bashpanel.h"
#include "modal.h"
#include "llmslots.h"
#include "roots.h"
#include "llmlive.h"
#include "textops.h"

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
#define LLM_APP_REFERER    "https://github.com/Eric1212/SIEB-CodeDashBoard"
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

/* llm.json est relu et réécrit en entier par sept écritures distinctes — les
 * cinq de ce fichier, les deux de llmtoolpref.c — qui toutes mutent la COPIE
 * de l'objet relu : un membre qu'elles ne connaissent pas survit donc à leurs
 * écritures. Ces deux accès sont ce qui permet à un autre module (les racines
 * de l'explorateur) de loger ici sans réécrire le fichier à la main — et sans
 * jamais toucher aux clés API. */

/* Membre étranger (clé que le modèle LLM ignore) : COPIE à libérer par
 * l'appelant, NULL si absente — le nœud prêté par le parser meurt avec lui. */
JsonNode *
llm_config_get_member(const char *key)
{
    char       *path   = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonNode   *out    = NULL;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *obj = json_node_get_object(json_parser_get_root(parser));

        if (json_object_has_member(obj, key))
            out = json_node_copy(json_object_get_member(obj, key));
    }
    g_object_unref(parser);
    g_free(path);
    return out;
}

/* Fusionne des membres au sommet de llm.json sans toucher aux autres.
 * `members` n'est pas consommé. */
void
llm_config_merge_members(JsonObject *members)
{
    char       *path   = llm_config_path();
    JsonParser *parser = json_parser_new();
    JsonNode   *work   = NULL;
    JsonObject *obj;
    GError     *error = NULL;
    gchar      *text;
    GList      *keys;

    if (members == NULL || json_object_get_size(members) == 0) {
        g_object_unref(parser);
        g_free(path);
        return;                       /* rien à fusionner : rien à écrire */
    }
    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        work = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser);
    if (work == NULL) {
        work = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(work, json_object_new());
    }
    obj = json_node_get_object(work);

    keys = json_object_get_members(members);
    for (GList *l = keys; l != NULL; l = l->next) {
        const char *k = l->data;

        json_object_set_member(obj, k,
                               json_node_copy(json_object_get_member(members, k)));
    }
    g_list_free(keys);

    text = json_to_string(work, TRUE);
    if (!g_file_set_contents(path, text, -1, &error)) {
        g_printerr(_("CDB: failed to write llm.json: %s\n"), error->message);
        g_error_free(error);
    }
    g_free(text);
    json_node_unref(work);
    g_free(path);
}

/* Catalogue des providers connus : LA source unique. Le seed (plus bas) en
 * remplit llm.json, llm_provider_default_url y puise l'URL de base, et le
 * formulaire Settings a sa liste. Un provider sans URL — OpenAi-Compatible,
 * qui n'a pas encore de formulaire et n'est voué à terme à rien d'autre
 * qu'à être un fourre-tout de providers — n'a qu'à ne pas figurer ici.
 * Ajouter un provider, muni d'une clé ou sans (OpenCode Zen en est la
 * première preuve), est désormais UNE ligne, et plus trois fichiers. */
static const struct {
    const char *name;
    const char *url;
} LLM_PROVIDERS[] = {
    { "OpenRouter",  "https://openrouter.ai/api/v1"    },
    { "OpenCode",    "https://opencode.ai/zen/v1"      },
    { "HyperCharm",  "https://hyper.charm.land/v1"     },
    { "KiloGateway", "https://api.kilo.ai/api/gateway" },
};

/* URL de base d'un provider connu ; NULL si inconnu. */
const char *
llm_provider_default_url(const char *provider)
{
    gsize i;

    if (provider == NULL)
        return NULL;
    for (i = 0; i < G_N_ELEMENTS(LLM_PROVIDERS); i++)
        if (g_strcmp0(provider, LLM_PROVIDERS[i].name) == 0)
            return LLM_PROVIDERS[i].url;
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

/* --------------------------------------------------------------- */
/* models.dev : noms lisibles des modèles                          */
/*                                                                 */
/* Certains providers (OpenCode Zen) ne renvoient aucun nom dans   */
/* /models. Les métadonnées viennent du tree models.dev vendoré    */
/* sous third_party/models-dev/models : un fichier .toml par       */
/* modèle, 295 Kio pour les 363.                                   */
/*                                                                 */
/* Ça remplace le GET de models.dev/api.json — 4,3 Mio de texte    */
/* qu'il fallait tenir en un DOM json-glib résident (~20 Mio) pour */
/* finir par n'en lire que quelques noms. Plus rien ne se charge   */
/* au départ : on ouvre le seul fichier qu'on cherche, quand on    */
/* le cherche.                                                     */
/*                                                                 */
/* Règles de résolution (Éric) :                                   */
/*  1. Seuls comptent les slugs SANS nom fourni.                   */
/*  2. Tout ce qui précède le dernier « / » saute : le fournisseur */
/*     n'est pas la clef. C'est ce qui fait marcher un gateway     */
/*     (opencode) absent de models/ —                            */
/*     « michel/anthropic/claude-opus-4-8 » est le modèle         */
/*     d'anthropic.                                               */
/*  3. Le suffixe « -free » se retire de la recherche.             */
/*  4. La comparaison ignore la casse.                             */
/*  5. On cherche dans models/ ENTIER, mais ANCRÉ sur le nom du    */
/*     fichier : « gpt-4o » doit répondre gpt-4o, pas              */
/*     « gpt-4o-mini ». 363 slugs, 0 collision, 0 ambiguïté.       */
/*  6. Le nom vient de la PREMIÈRE ligne commençant par `name` —   */
/*     souvent la ligne 1, jamais garantie (jusqu'à la 21 sous un  */
/*     bloc de sources) ; jamais après une section TOML, donc      */
/*     jamais le `name` d'un [[benchmark]].                        */
/*  7. Entre les deux premiers guillemets.                         */
/*  8. « -free » greffe un tag traduit.                            */
/* --------------------------------------------------------------- */

typedef struct {
    char *path;   /* le .toml, tel que trouvé sur disque */
    char *name;   /* lu à la demande, puis gardé         */
} MdEntry;

static GHashTable *md_index = NULL;   /* slug minuscule → MdEntry * */
static gboolean    md_warned = FALSE;       /* arbre vendoré introuvable   */
static gboolean    md_warned_name = FALSE;  /* un .toml sans ligne `name`  */

static void
md_entry_free(gpointer p)
{
    MdEntry *e = p;

    g_free(e->path);
    g_free(e->name);
    g_free(e);
}

/* <dir du binaire>/third_party/models-dev/models, exactement comme
 * i18n_localedir() le fait pour po/locale : CDB se lance depuis la racine du
 * projet, il n'y a pas d'installation à cibler. Repli relatif au cwd si
 * /proc/self/exe est illisible. */
static const char *
md_modelsdir(void)
{
    static char dir[4096];

    if (dir[0] != '\0')
        return dir;

    gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe == NULL) {
        g_strlcpy(dir, "third_party/models-dev/models", sizeof dir);
        return dir;
    }
    gchar *base = g_path_get_dirname(exe);

    g_snprintf(dir, sizeof dir, "%s/third_party/models-dev/models", base);
    g_free(base);
    g_free(exe);
    return dir;
}

/* Balaie models/ et retient le NOM de chaque fichier .toml — jamais un
 * morceau de ce nom : c'est l'ancrage qui interdit à « gpt-4o » de répondre
 * « gpt-4o-mini ». Règle 5. */
static void
md_scan(const char *dir, int depth)
{
    GDir *d = g_dir_open(dir, 0, NULL);

    if (d == NULL)
        return;

    const char *name;

    while ((name = g_dir_read_name(d)) != NULL) {
        char *path = g_build_filename(dir, name, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            if (depth > 0)
                md_scan(path, depth - 1);
            g_free(path);
            continue;
        }
        if (!g_str_has_suffix(name, ".toml")) {
            g_free(path);
            continue;
        }

        char *stem = g_strndup(name, strlen(name) - 5);
        char *key  = g_ascii_strdown(stem, -1);

        if (g_hash_table_contains(md_index, key)) {
            /* Deux fournisseurs portant le même nom de modèle donneraient
             * deux fichiers du même nom : le premier garde la place, mais ça
             * se dit. Un choix silencieux se lirait plus tard comme un bug de
             * nom, alors que c'est un conflit dans les données amont. */
            g_printerr(_("CDB: models.dev: duplicate slug '%s', keeping the first (%s)\n"),
                       key, path);
        } else {
            MdEntry *e = g_new0(MdEntry, 1);

            e->path = path;               /* la table devient propriétaire */
            g_hash_table_insert(md_index, key, e);
            key  = NULL;
            path = NULL;
        }
        g_free(stem);
        g_free(key);
        g_free(path);
    }
    g_dir_close(d);
}

/* L'index ne se construit qu'au premier slug sans nom : un CDB qui ne
 * interroge aucun modèle anonyme ne paie rien du tout. */
static void
md_index_ensure(void)
{
    if (md_index != NULL)
        return;

    md_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                     md_entry_free);
    md_scan(md_modelsdir(), 2);       /* models/<fournisseur>/<modèle>.toml */

    if (g_hash_table_size(md_index) == 0 && !md_warned) {
        /* Arbre absent = plus aucun nom, et surtout aucun symptôme : le
         * sélecteur de modèles affiche juste des slugs. On le dit une fois. */
        md_warned = TRUE;
        g_printerr(_("CDB: models.dev: no model found under %s —"
                     " `make tools && tools/refresh_third_party --bump"
                     " models-dev` ?\n"), md_modelsdir());
    }
}

/* Règles 6 et 7 : la première ligne qui COMMENCE par `name`, puis le texte
 * entre ses deux premiers guillemets. */
static char *
md_read_name(const char *path)
{
    char *text = NULL;
    char *out  = NULL;

    if (!g_file_get_contents(path, &text, NULL, NULL))
        return NULL;

    char **lines = g_strsplit(text, "\n", 0);

    for (int i = 0; lines[i] != NULL; i++) {
        const char *l = lines[i];
        const char *p, *open, *close;

        if (strncmp(l, "name", 4) != 0)
            continue;
        p = l + 4;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '=')            /* namespace =, name_x = : ce n'est pas lui */
            continue;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        open = strchr(p, '"');
        if (open == NULL)
            break;
        close = strchr(open + 1, '"');
        if (close == NULL)
            break;
        out = g_strndup(open + 1, (gsize)(close - open - 1));
        break;                    /* la première ligne name est la bonne */
    }
    g_strfreev(lines);
    g_free(text);

    /* Un fichier sans nom exploitable se lit comme un nom manquant, pas comme
     * un modèle inexistant : on le dit une fois, sinon le trou est muet. */
    if (out == NULL && !md_warned_name) {
        md_warned_name = TRUE;
        g_printerr(_("CDB: models.dev: no `name` line in %s\n"), path);
    }

    return out;
}

/* Applique les règles 2, 3 et 4 au slug brut, et rend le nom prêt à afficher
 * (règle 8). Alloué : le caller le donne à LlmModelInfo. */
static char *
md_lookup(const char *slug)
{
    const char *tail;
    MdEntry    *e;
    char       *key;
    gboolean    free_variant = FALSE;
    gsize       n;

    md_index_ensure();

    tail = strrchr(slug, '/');
    tail = (tail != NULL) ? tail + 1 : slug;
    if (*tail == '\0')
        return NULL;

    n = strlen(tail);
    if (n > 5 && g_ascii_strcasecmp(tail + n - 5, "-free") == 0) {
        free_variant = TRUE;
        n -= 5;
    }

    key = g_ascii_strdown(tail, (gssize)n);
    e   = g_hash_table_lookup(md_index, key);
    g_free(key);
    if (e == NULL)
        return NULL;

    if (e->name == NULL)
        e->name = md_read_name(e->path);
    if (e->name == NULL)
        return NULL;

    if (free_variant)
        return g_strdup_printf("%s %s", e->name, _("Free"));
    return g_strdup(e->name);
}

/* Règle 1 : ne touche que les modèles que le provider n'a pas nommés. */
void
md_enrich(LlmModelInfo *models)
{
    if (models == NULL)
        return;               /* le /models a échoué : rien à nommer */
    for (guint i = 0; models[i].id != NULL; i++) {
        if (models[i].name != NULL)
            continue;
        {
            char *nm = md_lookup(models[i].id);

            if (nm != NULL)
                models[i].name = nm;
        }
    }
}

void
md_deliver(ModelsFetch *f, LlmModelInfo *models)
{
    md_enrich(models);
    f->cb(models, f->user_data);
    llm_models_free(models);
    g_free(f->provider);
    g_clear_object (&f->msg);
    g_object_unref(f->soup);
    g_free(f);
    /* Liste livrée, DOM rendu, copies libérées : le tas vient de redescendre
     * en objets sans redescendre en pages. Un gateway à 354 modèles laisse
     * là plusieurs méga-octets de trous. */
    cdb_mem_trim();
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

    /* L'enrichissement est local et synchrone : les noms viennent des TOML
     * vendorés, il n'y a plus rien à attendre d'un aller-réseau. La liste part
     * donc dès ici — plus de file d'attente, plus de callback différé. */
    md_deliver(f, models);
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
    /* Garde même famille que celle du chat et que celle, déjà écrite, de
     * llm_credits_fetch : msg == NULL ne déclenche AUCUN rappel chez
     * libsoup. Sans elle, f, sa session et le contexte de l'appelant
     * partent, et le sélecteur attend une réponse qui ne viendra jamais.
     * Livrer NULL par le chemin normal, c'est ce qui libère tout — et
     * md_enrich accepte NULL depuis ce commit, sans quoi on se planterait
     * ailleurs pour avoir bouché un trou. */
    if (msg == NULL) {
        md_deliver(f, NULL);
        return;
    }
    /* Règle mesurée au weak pointer, voir llm_send_attempt : send_async ne
     * vole pas la référence de l'appelant — la session tient la sienne et la
     * rend à la fin, c'est donc à nous de rendre la nôtre. Ici elle ne
     * pouvait pas l'être : msg est une locale. Le tenir dans f->msg le rend
     * justiciable de md_deliver, l'unique route de mort de ModelsFetch.
     * Variante « read » : tout le corps en mémoire (les /models sont
     * petits) — le finish correspondant est send_and_read_finish. */
    f->msg = msg;
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
    g_clear_object (&f->msg);
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

    /* Règle mesurée au weak pointer, voir llm_send_attempt : send_async ne
     * vole pas la référence de l'appelant. Sans tenue dans le struct, la
     * nôtre ne pouvait être rendue nulle part : msg est une locale, morte à
     * la sortie de cette fonction — un message survuvait donc par poll, et
     * le poll de solde tourne toutes les 60 s. f->msg est rendu à
     * credits_fetch_done, l'unique route de mort de CreditsFetch. */
    f->msg = msg;
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
        json_node_take_object(work_root, root);
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
        json_node_take_object(work, root);
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
        json_node_take_object(work, root);
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

/* ------------------------------------------------ */
/* Noms des acteurs (section Harness)                */
/* ------------------------------------------------ */

/* Défauts : assistant = "Claude" ; user = login de la session
 * (« eric » de « eric@Eric-PC »). Un membre absent, vide ou non-chaîne
 * retombe sur son défaut — jamais sur une chaîne figée. */
void
llm_harness_names_load(LlmHarnessNames *out)
{
    char       *path = llm_config_path();
    JsonParser *parser = json_parser_new();

    out->assistant = g_strdup(LLM_NAME_ASSISTANT_DEFAULT);
    out->user = g_strdup(g_get_user_name());
    if (out->user == NULL || out->user[0] == '\0') {
        g_free(out->user);
        out->user = g_strdup("user");
    }
    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *root =
            json_node_get_object(json_parser_get_root(parser));

        if (root != NULL && json_object_has_member(root, "harness")) {
            JsonObject *h =
                json_object_get_object_member(root, "harness");
            JsonNode *n;

            if (h != NULL && json_object_has_member(h, "name_user")) {
                n = json_object_get_member(h, "name_user");
                if (n != NULL && json_node_get_value_type(n) == G_TYPE_STRING &&
                    json_node_get_string(n)[0] != '\0') {
                    g_free(out->user);
                    out->user = g_strdup(json_node_get_string(n));
                }
            }
            if (h != NULL && json_object_has_member(h, "name_assistant")) {
                n = json_object_get_member(h, "name_assistant");
                if (n != NULL && json_node_get_value_type(n) == G_TYPE_STRING &&
                    json_node_get_string(n)[0] != '\0') {
                    g_free(out->assistant);
                    out->assistant = g_strdup(json_node_get_string(n));
                }
            }
        }
    }
    g_object_unref(parser);
    g_free(path);
}

/* Même discipline que les saves retry : relecture du fichier, remplacement
 * des SEULS membres harness.name_user / harness.name_assistant, écriture.
 * NULL ou vide : écrit tel quel — le défaut sera appliqué au rechargement. */
void
llm_config_save_harness_names(const char *user, const char *assistant)
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
        json_node_take_object(work, root);
    }

    if (!json_object_has_member(root, "harness") ||
        json_object_get_object_member(root, "harness") == NULL)
        json_object_set_object_member(root, "harness",
                                      json_object_new());
    harness = json_object_get_object_member(root, "harness");
    json_object_set_string_member(harness, "name_user",
                                  user != NULL ? user : "");
    json_object_set_string_member(harness, "name_assistant",
                                  assistant != NULL ? assistant : "");

    {
        gchar  *text = json_to_string(work, TRUE);
        GError *error = NULL;

        if (!g_file_set_contents(path, text, -1, &error)) {
            g_printerr(_("CDB: failed to write harness names: %s\n"),
                       error->message);
            g_error_free(error);
        }
        g_free(text);
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
                /* Les maillons sont neufs, mais les chaines qu'ils portent
                 * sont les cles du hash table interne de l'objet : empruntees,
                 * pas copiees. Donc g_list_free() SEUL — g_list_free_full()
                 * libererait les cles de l'objet (double free). */
                g_list_free(members);
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
        json_node_take_object(work, root);
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

/* Lectrice seule : toute lecture de llm.json passe par ici, et elle rend
 * toujours une config NEUVE. La mise à jour de la config vivante est
 * llm_config_reload, plus bas — les deux ne sont pas interchangeables,
 * parce que la vivante est pointée directement par le core et les tuiles. */
static LlmConfig *
llm_config_read(void){
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

/* Relit llm.json DANS la config vivante, sans réallouer. Contrainte : le
 * core (llm_core_new) et chaque tuile (llm_tile_new) gardent un POINTEUR
 * direct sur cfg — réallouer serait un use-after-free assuré. On déplace
 * donc les membres au lieu de déplacer l'objet, qui est exactement le geste
 * que fait déjà llm_config_switch_active au terme de sa bascule.
 *
 * Renvoie cfg (le MÊME pointeur) quand la recharge a réussi ; NULL si
 * llm.json ne décrit aucune config exploitable — auquel cas l'état vivant
 * est laissé tel quel : un fichier illisible ne doit pas éteindre un chat en
 * cours. cfg == NULL : renvoie une config neuve, ou NULL. */
LlmConfig *
llm_config_reload(LlmConfig *cfg)
{
    LlmConfig *fresh = llm_config_read();

    if (fresh == NULL)
        return NULL;
    if (cfg == NULL)
        return fresh;

    g_free(cfg->provider);
    cfg->provider = fresh->provider;   /* possession transférée */
    g_free(cfg->model);
    cfg->model = fresh->model;
    g_free(cfg->api_url);
    cfg->api_url = fresh->api_url;
    g_free(cfg->api_key);
    cfg->api_key = fresh->api_key;
    g_free(fresh);                     /* coquille : plus rien dedans */
    return cfg;
}

/* Lecture simple, au démarrage : une config neuve, ou NULL. */
LlmConfig *
llm_config_load(void)
{
    return llm_config_reload(NULL);
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
        json_node_take_object(root_node, root);
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

/* Matérialise llm.json au premier lancement : le catalogue des providers,
 * leur URL de base et une clé VIDE. Une clé vide est une information (« ce
 * provider est connu, il n'a pas encore été muni »), pas une absence : c'est
 * elle qui rend llm_config_get_api_key homogène ("" plutôt que NULL) et qui
 * fait qu'un provider ne peut plus « apparaître » à l'exécution — le seul
 * membre que le fichier verra naître ensuite est « active ».
 *
 * Ce que le seed ne fait PAS, volontairement : il ne pose aucun « active ».
 * Choisir un provider est un acte de l'utilisateur (menu de la tuile, ou
 * première sauvegarde d'une clé), jamais un repli — cf. la loi « aucun
 * default_model » de llm.h. llm_config_load() renvoie donc encore NULL sur
 * une session neuve : le seed donne au fichier sa forme, pas une config.
 *
 * Jamais d'écrasement : si le fichier existe, on ne le touche pas, même
 * incomplet — ses membres appartiennent à l'utilisateur. Pas de mkdir non
 * plus : le dossier de session est créé par session_ensure(). */
void
llm_config_seed_if_absent(void)
{
    char          *path = llm_config_path();
    JsonObject    *root, *provs;
    JsonNode      *work;
    JsonGenerator *gen;
    gchar         *text;
    GError        *error = NULL;
    gsize          i;

    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return;
    }

    /* Convention json-glib, MESURÉE et non supposée — elle commande tout
     * transfert d'objet vers un nœud dans le projet :
     *
     *   json_node_set_object()   REFERENCE  -> il faut rendre notre référence
     *   json_node_init_object()  REFERENCE  -> idem
     *   json_node_take_object()  VOLE       -> il ne faut RIEN rendre
     *
     * On prend take_object : un mot, pas de ligne de plus, et c'est déjà
     * l'idiome de llmlive.c.
     *
     * Comment ça a été établi, parce que la première mesure était fausse.
     * Quatre formes testées dans UN SEUL processus : la dernière ne fuyait
     * pas. LSAN l'avait déclarée atteignable — son pointeur traînait encore
     * dans une pile alors dans les bornes au moment du rapport. Faux négatif
     * d'ordre d'exécution, pas de sémantique. Remis UN processus par forme,
     * la fuite est apparue des deux côtés, et l'ordre ne change plus rien.
     *
     * Le discriminant croisé, lui, ne dépend pas de LSAN : rendre notre
     * référence APRÈS la mort du nœud ne plaint que pour take_object (double
     * free), et relire l'objet après cette mort ne plaint que pour lui aussi.
     * Preuve directe de qui possédait quoi, sans rapport de fuite.
     *
     * Et pour qui relirait ce fichier : json_node_new(JSON_NODE_OBJECT) ne
     * crée PAS l'objet — son pointeur interne est NULL. json_node_get_object
     * sur un nœud ayant encore rien reçu rend donc NULL, et le générateur
     * s'y écrase. */
    root  = json_object_new();
    provs = json_object_new();
    for (i = 0; i < G_N_ELEMENTS(LLM_PROVIDERS); i++) {
        JsonObject *prov = json_object_new();

        json_object_set_string_member(prov, "api_url",
                                      LLM_PROVIDERS[i].url);
        json_object_set_string_member(prov, "api_key", "");
        json_object_set_object_member(provs, LLM_PROVIDERS[i].name, prov);
    }
    json_object_set_object_member(root, "providers", provs);

    work = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(work, root);   /* le nœud devient propriétaire */

    gen  = json_generator_new();
    json_generator_set_root(gen, work);
    json_generator_set_pretty(gen, TRUE);
    text = json_generator_to_data(gen, NULL);
    if (!g_file_set_contents(path, text, -1, &error)) {
        g_printerr(_("CDB: failed to seed llm.json: %s\n"), error->message);
        g_error_free(error);
    }
    g_free(text);
    g_object_unref(gen);
    json_node_unref(work);
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
    N_("Hello [NAME_ASSISTANT]. I am CodeDashBoard (CDB), an IDE "        \
    "application that acts as a relay between you and [NAME_USER]. "      \
    "You were hired by [NAME_USER] for your skills as a senior programmer " \
    "in systems, simulation and video games, and for your work ethic.\n\n"  \
    "You work exclusively remotely. Your employer never sees you: the "    \
    "whole of your working relationship passes through CDB, which runs "  \
    "on the workstation assigned to you both.\n\n"                        \
    "Project: [PROJET] ([CHEMIN]).\n\n"                                   \
    "# Remote control of the terminals\n"                                 \
    "You have the native tool bash. Use it whenever you must "        \
    "inspect, measure, compile or run a local action.\n"                   \
    "- terminal: the number of a CDB terminal (0 to 9; it is created "    \
    "if needed).\n"                                                       \
    "- command: a complete shell command, written as is.\n"                \
    "- The result is returned in a window of 100000 lines. To page, "     \
    "use head/tail/sed INSIDE the command.\n"                              \
    "- Every call is submitted to [NAME_USER]'s approval before execution.\n" \
    "- If a result has content:null, it means there is no new content "   \
    "compared with the previous results of the same terminal: that is "   \
    "not a failure.\n"                                                    \
    "- Never invent the output of a command.\n")

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
        txt = g_strdup(_(LLM_INITPROMPT_DEFAULT));
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

/* Noms PUBLICS des outils — tels que vus par le modele, tels que servis
 * par llm_body_build, tels que compares au tool_call recu, et tels qu'ils
 * servent de CLE dans le tableau "tools" de llm.json. Pas de prefixe :
 * le namespace est deja celui de CDB tout entier, et ces noms sont
 * exactement ceux que rend cdb_kind_label() plus bas. Les anciens noms
 * "cdb_read" & co ne survivent que dans la table de migration de
 * llmtoolpref.c. */
#define CDB_TOOL_NAME           "bash"
#define CDB_TOOL_NAME_READ      "read"
#define CDB_TOOL_NAME_INSERT    "insert"
#define CDB_TOOL_NAME_REMOVE    "remove"
#define CDB_TOOL_NAME_REPLACE   "replace"
#define CDB_TOOL_NAME_CREATE    "create"
#define CDB_TOOL_NAME_DELETE    "delete"

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
    shown = content != NULL ? content : _("〔tool〕 no new content");
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

static const char *
cdb_kind_label(CdbSpecKind k)
{
    /* Le libelle EST le nom public de l'outil, par les memes macros :
     * ecrire "read" ici et "read" la aussi serait deux verites capables
     * de diverger. Pas de default : un kind ajoute sans libelle doit
     * faire du bruit a la compilation, pas retomber silencieusement sur
     * le bash. */
    switch (k) {
    case CDB_SPEC_BASH:    return CDB_TOOL_NAME;
    case CDB_SPEC_READ:    return CDB_TOOL_NAME_READ;
    case CDB_SPEC_INSERT:  return CDB_TOOL_NAME_INSERT;
    case CDB_SPEC_REMOVE:  return CDB_TOOL_NAME_REMOVE;
    case CDB_SPEC_REPLACE: return CDB_TOOL_NAME_REPLACE;
    case CDB_SPEC_CREATE:  return CDB_TOOL_NAME_CREATE;
    case CDB_SPEC_DELETE:  return CDB_TOOL_NAME_DELETE;
    }
    return CDB_TOOL_NAME;     /* inatteignable ; -Wreturn-type content */
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
/* ---- plan d'appel : LA source de verite des regles d'un outil ------ */

/* Un seul endroit connait les regles d'un appel. Le dispatch (qui dresse
 * la barre d'approbation) et l'execution consomment le MEME plan.
 *
 * Avant, chacun validait de son cote, en copie collee, et les deux avaient
 * deja diverge : la barre annoncait « +1 line » la ou l'execution rendait
 * « no whole line ». Le compte que Eric approuve n'etait pas le compte qui
 * s'ecrivait.
 *
 * Le plan ne touche JAMAIS le disque : il ne statue que sur ce que le
 * modele AFFIRME. Les hashes sont revifies a l'execution, parce qu'une
 * decision ASK peut trainer et que le fichier a pu changer entre-temps.
 * Un plan valide n'est donc jamais une autorisation d'ecrire, c'est une
 * requete correctement formee.
 *
 * Le trio d'encadrement est le meme pour les trois outils d'edition, et
 * c'est voulu : hash_before = la ligne qui precede la zone touchee,
 * hash_target = la zone touchee elle-meme, hash_after = la ligne qui la
 * suit. insert n'a pas de cible — il insert — il n'a donc que les bords. */
typedef struct {
    JsonParser  *parser;        /* possede l'arbre ; les chaines ci-dessous
                                 * sont EMPRUNTEES et ne vivent que lui */
    CdbSpecKind  kind;
    const char  *path;

    /* insert : point d'insertion entre deux bornes ADJACENTES */
    long         before_line;   /* 0 = tete de fichier */
    long         after_line;    /* 0 = fin de fichier */
    const char  *text;          /* verbatim, newlines du modele */

    /* remove et replace : bloc designe */
    long         from_line;
    long         to_line;

    /* encadrement, commun aux trois */
    const char  *hash_before;
    const char  *hash_target;   /* remove/replace : read(from,to) */
    const char  *hash_after;

    /* replace seulement : lignes SANS terminateur */
    char       **lines;
    guint        n_lines;

    /* create et delete seulement. Loges ici, et non relus dans un
     * JsonObject au dispatch : un seul endroit touche aux arguments. */
    const char  *content;       /* create */
    const char  *file_hash;     /* delete, absent = 1re passe */
} CdbToolPlan;

static void
cdb_plan_clear(CdbToolPlan *p)
{
    if (p == NULL)
        return;
    if (p->lines != NULL) {
        for (guint i = 0; i < p->n_lines; i++)
            g_free(p->lines[i]);
        g_free(p->lines);
        p->lines = NULL;
        p->n_lines = 0;
    }
    if (p->parser != NULL) {
        g_object_unref(p->parser);
        p->parser = NULL;
    }
}

/* Un hash de bord est soit requis, soit interdit, selon la position
 * designee. L'INTERDIRE est la moitie utile de la regle : un hash presente
 * la ou il n'y a pas de ligne dit que le modele croit voir un bord qui
 * n'existe pas. Ce n'est pas une politesse, c'est une information.
 *
 * at_head = TRUE quand la zone touchee commence a la ligne 1 (pour insert,
 * quand le point d'insertion est la tete du fichier : before_line == 0). */
static gboolean
cdb_plan_border(const char *which, long line_no, gboolean at_head,
                const char *given, char **err)
{
    if (at_head) {
        if (given != NULL && given[0] != '\0') {
            *err = g_strdup_printf(_(
                "%s given but there is no line above: the block starts at "
                "the head of the file. Omit %s."), which, which);
            return FALSE;
        }
        return TRUE;
    }
    if (given == NULL || given[0] == '\0') {
        *err = g_strdup_printf(_(
            "%s required: read the bordering line with read(%ld, %ld) and "
            "replay its hash."), which, line_no, line_no);
        return FALSE;
    }
    return TRUE;
}

/* Extrait lines[] : tableau de chaines, aucune ne contient de saut de
 * ligne. Un element = une ligne. La regle de cardinalite est verifiee ICI
 * et non a l'execution, pour que le compte affiche dans la barre
 * d'approbation soit exactement celui qui sera ecrit. */
static gboolean
cdb_plan_lines(JsonObject *root, long from, long to,
               CdbToolPlan *p, char **err)
{
    JsonArray *arr;
    JsonNode  *member;
    guint      want = (guint)(to - from + 1);

    if (!json_object_has_member(root, "lines")) {
        *err = g_strdup_printf(_(
            "lines required: send exactly %u line%s, one element per line, "
            "without their newlines. \"\" is an EMPTY line, not a deleted "
            "one."), want, want == 1 ? "" : "s");
        return FALSE;
    }
    member = json_object_get_member(root, "lines");
    if (!JSON_NODE_HOLDS_ARRAY(member)) {
        *err = g_strdup(_(
            "lines must be an array of strings, one element per line."));
        return FALSE;
    }
    arr = json_node_get_array(member);
    if (json_array_get_length(arr) == 0) {
        *err = g_strdup(_(
            "lines is empty: to drop lines without rewriting them, use "
            "remove."));
        return FALSE;
    }
    if (json_array_get_length(arr) != want) {
        *err = g_strdup_printf(_(
            "cardinality mismatch: the block you read is %u line%s, you "
            "sent %u. replace writes exactly as many lines as it read — to "
            "shrink use remove, to grow use insert."),
            want, want == 1 ? "" : "s", json_array_get_length(arr));
        return FALSE;
    }

    p->lines = g_new0(char *, want);
    for (guint i = 0; i < want; i++) {
        JsonNode    *el = json_array_get_element(arr, i);
        const char  *s;

        if (el == NULL || !JSON_NODE_HOLDS_VALUE(el) ||
            json_node_get_value_type(el) != G_TYPE_STRING) {
            *err = g_strdup_printf(_(
                "lines[%u] is not a string: one element per line, without "
                "its newline."), i);
            goto fail;
        }
        s = json_node_get_string(el);
        for (const char *q = s; *q != '\0'; q++) {
            if (*q != '\n' && *q != '\r')
                continue;
            *err = g_strdup_printf(_(
                "lines[%u] contains a %s: one element is one line. Split it "
                "into as many elements."), i,
                *q == '\n' ? "newline" : "carriage return");
            goto fail;
        }
        p->lines[i] = g_strdup(s);
    }
    p->n_lines = want;
    return TRUE;

fail:
    for (guint i = 0; i < want; i++)
        g_free(p->lines[i]);
    g_free(p->lines);
    p->lines = NULL;
    p->n_lines = 0;
    return FALSE;
}

/* TRUE si l'appel est correctement FORME. *err porte alors le message a
 * rendre au modele et le plan est vide.
 *
 * Le refus des sequences NUL ne vit PAS ici : il est en tete de
 * cdb_dispatch_file_tool, donc en amont de la file, et les arguments d'un
 * appel en file ne mutent plus. Le repeter serait la duplication meme que
 * ce validateur vient supprimer. */
static gboolean
cdb_plan_parse(CdbSpecKind kind, const char *args_json,
               CdbToolPlan *p, char **err)
{
    JsonObject *root;

    memset(p, 0, sizeof(*p));
    p->kind = kind;
    *err = NULL;

    p->parser = json_parser_new();
    if (!json_parser_load_from_data(p->parser,
            args_json != NULL ? args_json : "", -1, NULL) ||
        json_parser_get_root(p->parser) == NULL ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p->parser))) {
        *err = g_strdup_printf(_("invalid JSON arguments for %s."),
                               cdb_kind_label(kind));
        goto bad;
    }
    root = json_node_get_object(json_parser_get_root(p->parser));

    p->path = cdb_json_str(root, "path");
    if (p->path == NULL || p->path[0] == '\0') {
        *err = g_strdup(_("missing path."));
        goto bad;
    }
    if (p->path[0] != '/') {
        *err = g_strdup(_("absolute path required."));
        goto bad;
    }

    p->hash_before = cdb_json_str(root, "hash_before");
    p->hash_target = cdb_json_str(root, "hash_target");
    p->hash_after  = cdb_json_str(root, "hash_after");

    switch (kind) {
    case CDB_SPEC_READ:
        p->from_line = llm_json_int(root, "from_line", -1);
        p->to_line   = llm_json_int(root, "to_line", -1);
        if (p->from_line < 1 || p->to_line < p->from_line) {
            *err = g_strdup(_(
                "from_line/to_line invalid (1-based, to >= from >= 1)."));
            goto bad;
        }
        return TRUE;

    case CDB_SPEC_INSERT:
        p->before_line = llm_json_int(root, "before_line", -1);
        p->after_line  = llm_json_int(root, "after_line", -1);
        p->text        = cdb_json_str(root, "text");
        if (p->before_line < 0 || p->after_line < 0) {
            *err = g_strdup(_(
                "before_line/after_line required (0 = file bound)."));
            goto bad;
        }
        if (p->before_line > 0 && p->after_line > 0 &&
            p->after_line != p->before_line + 1) {
            *err = g_strdup_printf(_(
                "non-adjacent bounds: after_line must be before_line + 1 "
                "(got %ld/%ld)."), p->before_line, p->after_line);
            goto bad;
        }
        if (p->text == NULL || p->text[0] == '\0') {
            *err = g_strdup(_(
                "missing or empty text (to delete lines, use remove)."));
            goto bad;
        }
        if (!cdb_plan_border("hash_before", p->before_line,
                             p->before_line == 0, p->hash_before, err))
            goto bad;
        if (!cdb_plan_border("hash_after", p->after_line,
                             p->after_line == 0, p->hash_after, err))
            goto bad;
        return TRUE;

    case CDB_SPEC_REMOVE:
    case CDB_SPEC_REPLACE:
        p->from_line = llm_json_int(root, "from_line", -1);
        p->to_line   = llm_json_int(root, "to_line", -1);
        if (p->from_line < 1 || p->to_line < p->from_line) {
            *err = g_strdup(_(
                "from_line/to_line invalid (1-based, to >= from >= 1)."));
            goto bad;
        }
        if (p->hash_target == NULL || p->hash_target[0] == '\0') {
            *err = g_strdup_printf(_(
                "hash_target required: it is the proof you read the very "
                "lines you are about to destroy. Run read(%ld, %ld) and "
                "replay the hash you got."), p->from_line, p->to_line);
            goto bad;
        }
        if (!cdb_plan_border("hash_before", p->from_line - 1,
                             p->from_line == 1, p->hash_before, err))
            goto bad;
        /* hash_after reste facultatif a ce stade : savoir si le bloc
         * touche la fin du fichier est un FAIT DE DISQUE, pas une
         * affirmation du modele. L'execution tranche dans les deux sens :
         * present, il doit matcher la ligne to+1 ; absent, le bloc doit
         * vraiment etre le dernier. Le silence n'est jamais une cle. */
        if (kind == CDB_SPEC_REPLACE &&
            !cdb_plan_lines(root, p->from_line, p->to_line, p, err))
            goto bad;
        return TRUE;

    case CDB_SPEC_CREATE:
        p->content = cdb_json_str(root, "content");
        if (p->content == NULL) {
            *err = g_strdup(_(
                "missing content: set content empty to create an empty "
                "file."));
            goto bad;
        }
        return TRUE;

    case CDB_SPEC_DELETE:
        p->file_hash = cdb_json_str(root, "file_hash");
        return TRUE;                /* absent = 1re passe : l'empreinte */

    case CDB_SPEC_BASH:
        *err = g_strdup(_("bash is not a file tool."));
        goto bad;
    }

bad:
    cdb_plan_clear(p);
    return FALSE;
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

/* read : plage exacte depuis le DISQUE (jamais le dirty). */
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
        char *e = g_strdup_printf(_("invalid JSON arguments for %s."), "read");
        cdb_queue_text_result(c, tool_call_id, "read", e, NULL, FALSE);
        g_free(e);
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
            _("missing path."), NULL, FALSE);
        goto done;
    }
    if (path[0] != '/') {
        cdb_queue_text_result(c, tool_call_id, "read",
            _("absolute path required."), NULL, FALSE);
        goto done;
    }
    if (from < 1 || to < from) {
        cdb_queue_text_result(c, tool_call_id, "read",
            _("from_line/to_line invalid (1-based, to >= from >= 1)."),
            NULL, FALSE);
        goto done;
    }

    if (!g_file_get_contents(path, &content, &len, &gerr)) {
        char *m = g_strdup_printf(_("cannot read: %s"),
            gerr != NULL ? gerr->message : "?");
        if (gerr != NULL)
            g_error_free(gerr);
        cdb_queue_text_result(c, tool_call_id, "read", m, NULL, FALSE);
        g_free(m);
        goto done;
    }
    if (!g_utf8_validate(content, (gssize)len, NULL)) {
        cdb_queue_text_result(c, tool_call_id, "read",
            _("binary file or non-UTF-8 encoding: refused."), NULL, FALSE);
        goto done;
    }

    off = g_array_new(FALSE, FALSE, sizeof(gsize));
    textops_line_offsets(content, len, off, &line_count);
    f = (guint)from;
    t = (guint)to;
    if (f > line_count || t > line_count) {
        char *m = g_strdup_printf(
            _("range outside the file: line_count=%u (requested %u-%u)."),
            line_count, f, t);
        cdb_queue_text_result(c, tool_call_id, "read", m, NULL, FALSE);
        g_free(m);
        goto done;
    }

    rstart = g_array_index(off, gsize, f - 1);
    rend = g_array_index(off, gsize, t);
    hash = textops_hash4(content + rstart, rend - rstart);

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


/* insert : insertion verbatim entre deux bornes adjacentes. DISQUE
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
        m = g_strdup_printf(_("invalid JSON arguments for %s."), "insert");
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    path = cdb_json_str(root, "path");
    text = cdb_json_str(root, "text");
    /* MEMES cles que celles que valide cdb_plan_parse : le dispatch et
     * l'execution doivent lire le meme champ, sinon le premier laisse
     * passer ce que la seconde rejette. Les variables locales gardent
     * leur nom, lui est purement interne. */
    before_hash = cdb_json_str(root, "hash_before");
    after_hash  = cdb_json_str(root, "hash_after");
    before = llm_json_int(root, "before_line", -1);
    after = llm_json_int(root, "after_line", -1);

    if (path == NULL || path[0] == '\0') {
        m = g_strdup(_("missing path."));
        goto done;
    }
    if (path[0] != '/') {
        m = g_strdup(_("absolute path required."));
        goto done;
    }
    if (text == NULL || text[0] == '\0') {
        m = g_strdup(_("missing or empty text (to delete, use replace)."));
        goto done;
    }
    if (before < 0 || after < 0) {
        m = g_strdup(_("before_line/after_line required (0 = file bound)."));
        goto done;
    }
    if (before > 0 && after > 0 && after != before + 1) {
        m = g_strdup_printf(
            _("non-adjacent bounds: after_line must be before_line + 1 "
              "(got %ld/%ld)."), before, after);
        goto done;
    }
    tlen = strlen(text);

    if (!g_file_get_contents(path, &content, &len, &gerr)) {
        if (gerr != NULL && gerr->domain == G_FILE_ERROR &&
            gerr->code == G_FILE_ERROR_NOENT)
            m = g_strdup(_("file absent: use create."));
        else
            m = g_strdup_printf(_("cannot read: %s"),
                                gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }
    if (!g_utf8_validate(content, (gssize)len, NULL)) {
        m = g_strdup(_("binary file or non-UTF-8 encoding: refused."));
        goto done;
    }

    off = g_array_new(FALSE, FALSE, sizeof(gsize));
    textops_line_offsets(content, len, off, &line_count);

    if (line_count == 0) {
        if (before != 0 || after != 0) {
            m = g_strdup_printf(
                _("empty file (line_count=0): before_line and after_line "
                  "must be 0, got %ld/%ld."), before, after);
            goto done;
        }
        ins_off = 0;
    } else if (before == 0) {
        if (after != 1) {
            m = g_strdup_printf(
                _("insertion at head: after_line must be 1 (got %ld)."),
                after);
            goto done;
        }
        ins_off = g_array_index(off, gsize, 0);
    } else if (after == 0) {
        if (before != (long)line_count) {
            m = g_strdup_printf(
                _("insertion at end: before_line must be line_count=%u "
                  "(got %ld)."), line_count, before);
            goto done;
        }
        ins_off = len;
    } else {
        if (after > (long)line_count) {
            m = g_strdup_printf(
                _("after_line outside the file: line_count=%u (got %ld). "
                  "To insert at the end of the file, set after_line=0 "
                  "with before_line=%u."), line_count, after, line_count);
            goto done;
        }
        ins_off = g_array_index(off, gsize, (guint)after - 1);
    }

    /* Garde-fou : les lignes qui bordent le point d'insertion sont-elles
     * bien celles que le modele a lues ? */
    if (before > 0) {
        gsize ls = g_array_index(off, gsize, (guint)before - 1);
        gsize le = g_array_index(off, gsize, (guint)before);

        hash = textops_hash4(content + ls, le - ls);
        if (before_hash == NULL || g_strcmp0(hash, before_hash) != 0) {
            /* JAMAIS le hash courant ici : un refus qui le divulgue
             * dispense le modele de lire, et detruit la preuve de Focus. */
            m = g_strdup_printf(
                _("before_hash stale or missing: line %ld no longer holds "
                  "the content you claim to have read. Run "
                  "read(%ld, %ld) and replay the hash you got."),
                before, before, before);
            goto done;
        }
        g_free(hash);
        hash = NULL;
    }
    if (after > 0) {
        gsize ls = g_array_index(off, gsize, (guint)after - 1);
        gsize le = g_array_index(off, gsize, (guint)after);

        hash = textops_hash4(content + ls, le - ls);
        if (after_hash == NULL || g_strcmp0(hash, after_hash) != 0) {
            m = g_strdup_printf(
                _("after_hash stale or missing: line %ld no longer holds "
                  "the content you claim to have read. Run "
                  "read(%ld, %ld) and replay the hash you got."),
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
        m = g_strdup_printf(_("cannot write: %s"),
                            gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }

    /* On rend l'etat REEL apres coupure, pas l'intention declaree. */
    noff = g_array_new(FALSE, FALSE, sizeof(gsize));
    textops_line_offsets(fresh, newlen, noff, &new_line_count);
    a = textops_line_at(noff, new_line_count, ins_off);
    b = textops_line_at(noff, new_line_count, ins_off + tlen - 1);

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
        hblock = textops_hash4(fresh + g_array_index(noff, gsize, lo - 1),
                           g_array_index(noff, gsize, hi) -
                           g_array_index(noff, gsize, lo - 1));
        hfirst = textops_hash4(fresh + g_array_index(noff, gsize, lo - 1),
                           g_array_index(noff, gsize, lo) -
                           g_array_index(noff, gsize, lo - 1));
        hlast  = textops_hash4(fresh + g_array_index(noff, gsize, hi - 1),
                           g_array_index(noff, gsize, hi) -
                           g_array_index(noff, gsize, hi - 1));
        g_string_append_printf(out,
            "authored_range: %u-%u\nhash_block: %s\nhash_first: %s\n"
            "hash_last: %s\n", lo, hi, hblock, hfirst, hlast);
        if (lo != a || hi != b)
            g_string_append(out,
                _("note: lines outside authored_range mix your text with "
                  "existing content; no hash is attached to them.\n"));
    } else {
        g_string_append(out,
            _("authored_range: no whole line\n"
              "note: no line is entirely yours (text without a trailing "
              "newline, or inserted mid-line). Read with "
              "read(N,N) before any other write.\n"));
    }
    result = g_string_free(out, FALSE);
    out = NULL;
    cdb_queue_text_result(c, tool_call_id,
                          cdb_kind_label(CDB_SPEC_INSERT),
                          result, NULL, FALSE);

done:
    if (m != NULL) {
        cdb_queue_text_result(c, tool_call_id,
                              cdb_kind_label(CDB_SPEC_INSERT),
                              m, NULL, FALSE);
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

/* ---- garde partage : le trio verifie contre l'octet present ---------- */

/* Hash d'UNE ligne (1-based), terminaison comprise — donc identique a ce
 * que rend read(N,N). */
static char *
cdb_hash_line(GArray *off, const char *content, guint ln)
{
    gsize a = g_array_index(off, gsize, ln - 1);
    gsize b = g_array_index(off, gsize, ln);

    return textops_hash4(content + a, b - a);
}

/* Verifie hash_target et les deux bords contre le fichier tel qu'il est
 * MAINTENANT, et rend la zone du bloc. remove et replace partagent cette
 * fonction : ils detruisent la meme zone et doivent la prouver de la meme
 * facon. Deux implementations divergentes de la preuve, c'est exactement
 * ce qu'on est venu corriger.
 *
 * Loi du silence : l'ABSENCE d'un hash est une affirmation, pas une
 * absence d'affirmation. hash_after absent soutient « mon bloc est le
 * dernier » — et cette soutient est verifiee, comme la presence. */
static gboolean
cdb_block_guard(const char *content, gsize len, GArray *off,
                guint line_count, const CdbToolPlan *p,
                gsize *start, gsize *end, char **err)
{
    const guint from = (guint)p->from_line, to = (guint)p->to_line;
    char *h;

    if (line_count == 0) {
        *err = g_strdup(_(
            "empty file: there is no line to destroy. Use insert to write "
            "the first lines."));
        return FALSE;
    }
    if (!textops_block_range(len, off, line_count, from, to, start, end)) {
        *err = g_strdup_printf(_(
            "block %u-%u is outside the file (line_count=%u). Line numbers "
            "do not survive writes: read the file again before naming a "
            "range."), from, to, line_count);
        return FALSE;
    }

    h = textops_hash4(content + *start, *end - *start);
    if (g_strcmp0(h, p->hash_target) != 0) {
        g_free(h);
        /* JAMAIS le hash courant : un refus qui le divulgue dispense de
         * lire et detruit la loi du Focus. */
        *err = g_strdup_printf(_(
            "hash_target stale or wrong: %u-%u no longer holds what you "
            "claim to have read. Run read(%u, %u) and replay the hash you "
            "got."), from, to, from, to);
        return FALSE;
    }
    g_free(h);
    h = NULL;

    if (from > 1) {
        h = cdb_hash_line(off, content, from - 1);
        if (g_strcmp0(h, p->hash_before) != 0) {
            g_free(h);
            *err = g_strdup_printf(_(
                "hash_before stale or wrong: line %u no longer holds what "
                "you claim to have read. Run read(%u, %u) and replay the "
                "hash you got."), from - 1, from - 1, from - 1);
            return FALSE;
        }
        g_free(h);
        h = NULL;
    }

    if (p->hash_after != NULL && p->hash_after[0] != '\0') {
        if (to >= line_count) {
            *err = g_strdup_printf(_(
                "hash_after given but the block already ends at the last "
                "line (line_count=%u): there is nothing below it. Omit "
                "hash_after."), line_count);
            return FALSE;
        }
        h = cdb_hash_line(off, content, to + 1);
        if (g_strcmp0(h, p->hash_after) != 0) {
            g_free(h);
            *err = g_strdup_printf(_(
                "hash_after stale or wrong: line %u no longer holds what "
                "you claim to have read. Run read(%u, %u) and replay the "
                "hash you got."), to + 1, to + 1, to + 1);
            return FALSE;
        }
        g_free(h);
        h = NULL;
    } else if (to < line_count) {
        *err = g_strdup_printf(_(
            "hash_after required: line %u exists below your block "
            "(line_count=%u). Either read it with read(%u, %u) and replay "
            "its hash, or extend the block to the end of the file."),
            to + 1, line_count, to + 1, to + 1);
        return FALSE;
    }
    return TRUE;
}

/* ---- l'echo de ce qui est entierement du modele --------------------- */

/* Frappe le hash des lignes que le modele a ecrites de bout en bout. Le
 * contrat de replace (k lignes -> k lignes entieres) rend le cas general
 * inutile ICI : la zone ecrite couvre toujours des lignes entieres. */
static void
cdb_echo_authored(GString *out, const char *fresh, GArray *noff,
                  guint new_line_count, guint lo, guint hi)
{
    char *hblock, *hfirst, *hlast;

    if (lo > hi || hi > new_line_count) {
        g_string_append(out, _("authored_range: no whole line\n"));
        return;
    }
    hblock = textops_hash4(fresh + g_array_index(noff, gsize, lo - 1),
                           g_array_index(noff, gsize, hi) -
                           g_array_index(noff, gsize, lo - 1));
    hfirst = textops_hash4(fresh + g_array_index(noff, gsize, lo - 1),
                           g_array_index(noff, gsize, lo) -
                           g_array_index(noff, gsize, lo - 1));
    hlast  = textops_hash4(fresh + g_array_index(noff, gsize, hi - 1),
                           g_array_index(noff, gsize, hi) -
                           g_array_index(noff, gsize, hi - 1));
    g_string_append_printf(out,
        "authored_range: %u-%u\nhash_block: %s\nhash_first: %s\n"
        "hash_last: %s\n", lo, hi, hblock, hfirst, hlast);
    g_free(hblock);
    g_free(hfirst);
    g_free(hlast);
}

/* remove : retire n lignes ENTIERES. Miroir de insert, en destructif.
 *
 * La zone retiree va de off[from-1] a off[to], terminaison comprise :
 * ce n'est QUE jamais une demi-ligne, et c'est ce qui rend l'operation
 * geometriquement propre. Aucun hash n'est frappe a la fin : le modele
 * n'a ecrit aucun octet. */
static void
cdb_tool_file_remove(LlmCore *c, const char *tool_call_id,
                     const char *args_json)
{
    CdbToolPlan plan;
    char       *err = NULL;
    char       *content = NULL, *fresh = NULL;
    GError     *gerr = NULL;
    GArray     *off = NULL, *noff = NULL;
    gsize       len = 0, a = 0, b = 0, newlen = 0;
    guint       line_count = 0, new_line_count = 0;
    GString    *out = NULL;
    char       *result = NULL;

    if (!cdb_plan_parse(CDB_SPEC_REMOVE, args_json, &plan, &err)) {
        result = err;
        err = NULL;
        goto done;
    }

    if (!g_file_get_contents(plan.path, &content, &len, &gerr)) {
        if (gerr != NULL && gerr->domain == G_FILE_ERROR &&
            gerr->code == G_FILE_ERROR_NOENT)
            err = g_strdup(_("file absent: use create."));
        else
            err = g_strdup_printf(_("cannot read: %s"),
                                  gerr != NULL ? gerr->message : "?");
        goto done;
    }
    if (!g_utf8_validate(content, (gssize)len, NULL)) {
        err = g_strdup(_("binary file or non-UTF-8 encoding: refused."));
        goto done;
    }

    off = g_array_new(FALSE, FALSE, sizeof(gsize));
    textops_line_offsets(content, len, off, &line_count);
    if (!cdb_block_guard(content, len, off, line_count, &plan,
                         &a, &b, &err))
        goto done;

    newlen = len - (b - a);
    {
        GString *nb = g_string_sized_new(newlen);

        g_string_append_len(nb, content, a);
        g_string_append_len(nb, content + b, len - b);
        fresh = g_string_free(nb, FALSE);
    }

    if (!g_file_set_contents(plan.path, fresh, (gssize)newlen, &gerr)) {
        err = g_strdup_printf(_("cannot write: %s"),
                              gerr != NULL ? gerr->message : "?");
        goto done;
    }

    noff = g_array_new(FALSE, FALSE, sizeof(gsize));
    textops_line_offsets(fresh, newlen, noff, &new_line_count);

    out = g_string_new(NULL);
    g_string_append_printf(out,
        "remove: ok\npath: %s\nremoved_range: %ld-%ld\nremoved_lines: %ld\n"
        "line_count: %u\n",
        plan.path, plan.from_line, plan.to_line,
        plan.to_line - plan.from_line + 1, new_line_count);
    /* Le decalage est ANNONCE, pas devine : c'est la source directe des
     * off-by-one quand le modele recite des numeros apres une suppression.
     * Une seule condition fait foi : il restait une ligne sous le bloc. */
    if (plan.to_line < (long)line_count) {
        g_string_append_printf(out,
            _("note: former line %ld is now line %ld (everything below the "
              "block moved up by %ld). The hash of an untouched line does "
              "not change — only its number.\n"),
            plan.to_line + 1, plan.from_line,
            (long)(plan.to_line - plan.from_line + 1));
    }
    if (new_line_count == 0)
        g_string_append(out,
            _("note: the file is now empty. To write it again, insert with "
              "before_line=0 and after_line=0.\n"));
    g_string_append(out,
        _("authored_range: no whole line\n"
          "note: nothing was written, so no hash was struck. Read the new "
          "neighbours before writing here.\n"));
    result = g_string_free(out, FALSE);
    out = NULL;

done:
    cdb_queue_text_result(c, tool_call_id,
                          cdb_kind_label(CDB_SPEC_REMOVE),
                          result != NULL ? result : err, NULL, FALSE);
    g_free(result);
    g_free(err);
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
    cdb_plan_clear(&plan);
}

/* replace : k lignes lues -> k lignes ecrites. JAMAIS plus, JAMAIS moins.
 *
 * Le modele renvoie le bloc qu'il a lu, ligne par ligne, SANS leurs
 * terminateurs ; c'est CDB qui joint, avec le terminateur que le fichier
 * utilisait deja. Le \n final n'est donc ni cree ni perdu, et une ligne
 * vide demandee est une ligne vide ecrite : l'ancien contrat laissait ces
 * trois decisions au modele, et c'est de la que venaient ses erreurs.
 * La geometrie du fichier ne bouge pas : replace est le seul outil qui
 * ne deplace rien. */
static void
cdb_tool_file_replace(LlmCore *c, const char *tool_call_id,
                      const char *args_json)
{
    CdbToolPlan plan;
    char       *err = NULL;
    char       *content = NULL, *fresh = NULL, *block = NULL;
    GError     *gerr = NULL;
    GArray     *off = NULL, *noff = NULL;
    gsize       len = 0, a = 0, b = 0, jlen = 0, newlen = 0;
    guint       line_count = 0, new_line_count = 0;
    TextopsEol  eol;
    GString    *out = NULL;
    char       *result = NULL;

    if (!cdb_plan_parse(CDB_SPEC_REPLACE, args_json, &plan, &err)) {
        result = err;
        err = NULL;
        goto done;
    }

    if (!g_file_get_contents(plan.path, &content, &len, &gerr)) {
        if (gerr != NULL && gerr->domain == G_FILE_ERROR &&
            gerr->code == G_FILE_ERROR_NOENT)
            err = g_strdup(_("file absent: use create."));
        else
            err = g_strdup_printf(_("cannot read: %s"),
                                  gerr != NULL ? gerr->message : "?");
        goto done;
    }
    if (!g_utf8_validate(content, (gssize)len, NULL)) {
        err = g_strdup(_("binary file or non-UTF-8 encoding: refused."));
        goto done;
    }

    off = g_array_new(FALSE, FALSE, sizeof(gsize));
    textops_line_offsets(content, len, off, &line_count);
    if (!cdb_block_guard(content, len, off, line_count, &plan,
                         &a, &b, &err))
        goto done;

    /* La politique de terminateur vit ICI, pas dans la tete du modele :
     * CDB joint les lignes avec le style du fichier, et ne cree ni ne
     * detruit le \n final. */
    textops_scan_eol(content, len, &eol);

    /* Le plan a deja refuse tout element portant un saut de ligne ; la
     * rev ici est un garde-fou de dernier kilometre, pas la regle. */
    TextopsJoinErr jerr = { 0, 0 };
    block = textops_join_block((const char *const *)plan.lines,
                               plan.n_lines, &eol,
                               plan.to_line == (long)line_count,
                               &jlen, &jerr);
    if (block == NULL) {
        err = g_strdup_printf(_(
            "lines[%u] cannot be written as one line (found a %s)."),
            jerr.bad_line,
            jerr.bad_char == '\r' ? "carriage return" : "newline");
        goto done;
    }

    /* No-op refuse : renvoyer le bloc qu'on vient de lire n'est pas une
     * ecriture. Le signaler evite une approbation et un write pour rien,
     * et attrape le modele qui rejoue sa lecture au lieu d'ecrire. */
    if (jlen == b - a && memcmp(content + a, block, jlen) == 0) {
        err = g_strdup_printf(_(
            "no change: you sent back exactly the block at %ld-%ld. "
            "replace must write something different — if nothing had to "
            "change, that is read's job, not a write."),
            plan.from_line, plan.to_line);
        goto done;
    }

    newlen = len - (b - a) + jlen;
    {
        GString *nb = g_string_sized_new(newlen);

        g_string_append_len(nb, content, a);
        g_string_append_len(nb, block, jlen);
        g_string_append_len(nb, content + b, len - b);
        fresh = g_string_free(nb, FALSE);
    }

    if (!g_file_set_contents(plan.path, fresh, (gssize)newlen, &gerr)) {
        err = g_strdup_printf(_("cannot write: %s"),
                              gerr != NULL ? gerr->message : "?");
        goto done;
    }

    noff = g_array_new(FALSE, FALSE, sizeof(gsize));
    textops_line_offsets(fresh, newlen, noff, &new_line_count);

    out = g_string_new(NULL);
    g_string_append_printf(out,
        "replace: ok\npath: %s\nreplaced_range: %ld-%ld\nwritten_lines: %u\n"
        "line_count: %u\n",
        plan.path, plan.from_line, plan.to_line, plan.n_lines,
        new_line_count);
    /* L'invariant est affirme a voix haute : s'il sautait jamais, le
     * resultat le dirait au lieu de le cacher. */
    if (new_line_count != line_count)
        g_string_append_printf(out,
            _("warning: line count moved from %u to %u on a k->k "
              "replacement — this should be unreachable.\n"),
            line_count, new_line_count);
    /* Le map rend l'off-by-one visible : lines[i] EST la ligne N. */
    g_string_append_printf(out,
        _("mapping: lines[0-%u] -> file %ld-%ld\n"),
        plan.n_lines - 1, plan.from_line, plan.to_line);
    cdb_echo_authored(out, fresh, noff, new_line_count,
                      (guint)plan.from_line, (guint)plan.to_line);
    if (plan.to_line == (long)line_count && !eol.final_nl)
        g_string_append(out,
            _("note: the file has no trailing newline and the last line "
              "kept it that way.\n"));
    result = g_string_free(out, FALSE);
    out = NULL;

done:
    cdb_queue_text_result(c, tool_call_id,
                          cdb_kind_label(CDB_SPEC_REPLACE),
                          result != NULL ? result : err, NULL, FALSE);
    g_free(result);
    g_free(err);
    if (out != NULL)
        g_string_free(out, TRUE);
    if (noff != NULL)
        g_array_free(noff, TRUE);
    if (off != NULL)
        g_array_free(off, TRUE);
    g_free(block);
    g_free(fresh);
    g_free(content);
    if (gerr != NULL)
        g_error_free(gerr);
    cdb_plan_clear(&plan);
}

/* create : cree un fichier texte NEUF.
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
        m = g_strdup_printf(_("invalid JSON arguments for %s."), "create");
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    path = cdb_json_str(root, "path");
    content = cdb_json_str(root, "content");
    clen = (content != NULL) ? strlen(content) : 0;
    if (path == NULL || path[0] == '\0') {
        m = g_strdup(_("missing path."));
        goto done;
    }
    if (path[0] != '/') {
        m = g_strdup(_("absolute path required."));
        goto done;
    }
    if (content == NULL) {
        m = g_strdup(_("missing content: set content empty to create an "
                     "empty file."));
        goto done;
    }
    if (!g_utf8_validate(content, (gssize)clen, NULL)) {
        m = g_strdup(_("non-UTF-8 content: refused."));
        goto done;
    }
    dir = g_path_get_dirname(path);
    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        m = g_strdup_printf(_("parent folder absent: %s (create never "
                            "creates a folder)."), dir);
        goto done;
    }
    if (!cdb_file_create_exclusive(path, content, clen, &gerr)) {
        if (gerr != NULL && gerr->domain == G_IO_ERROR &&
            gerr->code == G_IO_ERROR_EXISTS)
            m = g_strdup(_("file already exists: use replace with its "
                         "block_hash, or delete then create."));
        else
            m = g_strdup_printf(_("cannot create: %s"),
                                gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }

    /* Ce qui est sur disque est exactement ce qu'on vient d'ecrire : tout
     * est fourni par le modele, donc authored_range couvre le fichier. */
    out = g_string_new(NULL);
    if (clen == 0) {
        g_string_append_printf(out,
            _("create: ok\npath: %s\nline_count: 0\n"
              "note: empty file created; no hash struck.\n"), path);
    } else {
        off = g_array_new(FALSE, FALSE, sizeof(gsize));
        textops_line_offsets(content, clen, off, &line_count);
        hblock = textops_hash4(content, clen);
        hfirst = textops_hash4(content, g_array_index(off, gsize, 1) -
                                  g_array_index(off, gsize, 0));
        hlast  = textops_hash4(content +
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

/* delete : destruction en DEUX PASSES, comme demande par Eric.
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
        m = g_strdup_printf(_("invalid JSON arguments for %s."), "delete");
        goto done;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    path = cdb_json_str(root, "path");
    file_hash = cdb_json_str(root, "file_hash");

    if (path == NULL || path[0] == '\0') {
        m = g_strdup(_("missing path."));
        goto done;
    }
    if (path[0] != '/') {
        m = g_strdup(_("absolute path required."));
        goto done;
    }
    if (lstat(path, &st) != 0) {
        m = g_strdup(_("file absent: nothing to delete."));
        goto done;
    }
    /* lstat, pas stat : un lien se detruit lui-meme alors que son contenu
     * se lit a travers la cible. Le hash rendu n'aurait rien a voir avec
     * ce qui serait efface. */
    if (S_ISLNK(st.st_mode)) {
        m = g_strdup(_("symlink refused: its content is read through the "
                     "target, but deleting would destroy the link. Act on "
                     "the target directly."));
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        m = g_strdup(_("not a regular file: not deleted (folder, "
                     "pipe, device)."));
        goto done;
    }
    if (!g_file_get_contents(path, &content, &len, &gerr)) {
        m = g_strdup_printf(_("cannot read: %s"),
                            gerr != NULL ? gerr->message : "?");
        if (gerr != NULL) { g_error_free(gerr); gerr = NULL; }
        goto done;
    }
    /* Un binaire peut se supprimer : le hash prouve l'identite du fichier,
     * pas sa lisibilite. On le signale seulement. */
    if (!g_utf8_validate(content, (gssize)len, NULL))
        binary = TRUE;

    off = g_array_new(FALSE, FALSE, sizeof(gsize));
    textops_line_offsets(content, len, off, &line_count);
    fh = textops_hash4(content, len);
    out = g_string_new(NULL);
    if (file_hash == NULL || file_hash[0] == '\0') {
        g_string_append_printf(out,
            _("delete: confirmation required\npath: %s\nline_count: %u\n"
              "octets: %lu\nbinary: %s\nfile_hash: %s\n"
              "note: re-run delete(path, file_hash) to destroy it. If "
              "the file changes between the two calls, the second pass "
              "will be refused.\n"),
            path, line_count, (gulong)len, binary ? "yes" : "no", fh);
    } else if (g_strcmp0(fh, file_hash) != 0) {
        g_free(fh);
        fh = NULL;
        g_string_free(out, TRUE);
        out = NULL;
        m = g_strdup(_("file_hash stale: the file is no longer the one "
                     "confirmed. Re-run delete without a hash for "
                     "the current fingerprint."));
        goto done;
    } else {
        if (unlink(path) != 0) {
            g_free(fh);
            fh = NULL;
            g_string_free(out, TRUE);
            out = NULL;
            m = g_strdup_printf(_("cannot delete: %s"),
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
    /* Zero-init obligatoire : la porte NUL qui suit peut sortir par
     * « goto done » AVANT que le plan soit parse, et cdb_plan_clear()
     * doit alors trouver une structure propre.
     *
     * Le parseur local de cette fonction a disparu avec l'ancienne
     * validation : c'est cdb_plan_parse qui ouvre l'arbre JSON, qui rend
     * le meme « invalid JSON arguments for %s. » avec le nom canonique de
     * l'outil (cdb_kind_label) plutot que celui, libre, que le modele a
     * ecrit. Le plan le referme. */
    CdbToolPlan plan = { 0 };
    char       *error = NULL;
    char       *summary = NULL;
    /* Un nul intercalaire dans text/content serait tronque silencieusement
     * par strlen : json-glib n'expose pas la longueur reelle de ses chaines.
     * On le refuse a la porte, en detectant la sequence d'chappement JSON qui la produit. */
    if (tc->arguments_json != NULL &&
        strstr(tc->arguments_json, "\\u0000") != NULL) {
        error = g_strdup(_("NUL sequence (\\u0000) refused in arguments."));
        goto done;
    }

    /* Une seule regle, un seul endroit : le plan que l'execution
     * re-verifiera a l'instant. Ce que la barre d'approbation annonce est
     * donc, par construction, exactement ce qui sera ecrit. Avant, ces
     * deux cotes validaient en copie collee et leurs compteurs avaient
     * deja diverge. */
    if (!cdb_plan_parse(kind, tc->arguments_json, &plan, &error))
        goto done;

    switch (kind) {
    case CDB_SPEC_READ:
        summary = g_strdup_printf("read  %s  %ld-%ld",
                                  plan.path, plan.from_line, plan.to_line);
        break;

    case CDB_SPEC_INSERT: {
        guint n = textops_logical_lines(plan.text, strlen(plan.text));

        summary = g_strdup_printf(
            ngettext("insert  %s  after %ld / before %ld  (+%u line)",
                     "insert  %s  after %ld / before %ld  (+%u lines)", n),
            plan.path, plan.before_line, plan.after_line, n);
        break;
    }

    case CDB_SPEC_REMOVE: {
        guint n = (guint)(plan.to_line - plan.from_line + 1);

        summary = g_strdup_printf(
            ngettext("remove  %s  %ld-%ld  (-%u line)",
                     "remove  %s  %ld-%ld  (-%u lines)", n),
            plan.path, plan.from_line, plan.to_line, n);
        break;
    }

    case CDB_SPEC_REPLACE:
        /* k -> k : le plan tient lines.length == to-from+1, donc les deux
         * chiffres affiche ici sont le MEME nombre et ne peuvent plus
         * s'ecarter de ce que la jointure produira. */
        summary = g_strdup_printf(
            ngettext("replace  %s  %ld-%ld  [%u line -> %u line]",
                     "replace  %s  %ld-%ld  [%u lines -> %u lines]",
                     plan.n_lines),
            plan.path, plan.from_line, plan.to_line,
            plan.n_lines, plan.n_lines);
        break;

    case CDB_SPEC_CREATE: {
        guint added = textops_logical_lines(plan.content,
                                            strlen(plan.content));

        /* Un seul compteur : le pluriel porte la ligne entiere, donc
         * l'ordre des mots reste libre pour chaque langue. */
        summary = g_strdup_printf(
            ngettext("create  %s  +%u line  (new file)",
                     "create  %s  +%u lines  (new file)", added),
            plan.path, added);
        break;
    }

    case CDB_SPEC_DELETE: {
        char     *cnt = NULL;
        gsize     cl = 0;
        guint     removed = 0;
        gboolean  absent = TRUE;

        /* Compter les lignes detruites ici, pour qu'Eric voie la taille du
         * degat dans la barre d'approbation. Lecture seule. */
        if (g_file_get_contents(plan.path, &cnt, &cl, NULL) && cnt != NULL) {
            GArray *o = g_array_new(FALSE, FALSE, sizeof(gsize));
            guint   lc = 0;

            absent = FALSE;
            textops_line_offsets(cnt, cl, o, &lc);
            removed = lc;
            g_array_free(o, TRUE);
            g_free(cnt);
        }
        summary = g_strdup_printf(
            ngettext("delete  %s  -%u line%s%s",
                     "delete  %s  -%u lines%s%s", removed),
            plan.path, removed,
            absent ? _("   [ABSENT OR UNREADABLE]") : "",
            (plan.file_hash != NULL && plan.file_hash[0] != '\0')
                ? _("   [DESTRUCTION CONFIRMED]") : "");
        break;
    }

    case CDB_SPEC_BASH:
        /* Inatteignable : cdb_dispatch_native_call ne route vers cette
         * fonction qu'un kind != BASH. Pas de default — un kind neuf
         * doit etre traite ici, pas absorbe en silence. */
        break;
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
    cdb_plan_clear(&plan);
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
            _("tool call ignored: missing API id."));
        return;
    }
    if (c->answered_tools != NULL &&
        g_hash_table_contains(c->answered_tools, tc->id)) {
        core_cdb_announce(c, _("duplicate tool call ignored by CDB."));
        return;
    }

    if (g_strcmp0(tc->name, CDB_TOOL_NAME) == 0)
        kind = CDB_SPEC_BASH;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_READ) == 0)
        kind = CDB_SPEC_READ;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_INSERT) == 0)
        kind = CDB_SPEC_INSERT;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_REMOVE) == 0)
        kind = CDB_SPEC_REMOVE;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_REPLACE) == 0)
        kind = CDB_SPEC_REPLACE;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_CREATE) == 0)
        kind = CDB_SPEC_CREATE;
    else if (g_strcmp0(tc->name, CDB_TOOL_NAME_DELETE) == 0)
        kind = CDB_SPEC_DELETE;
    else {
        error = g_strdup_printf(
            _("unknown tool \"%s\": this tool does not exist in CDB."),
            tc->name != NULL ? tc->name : _("(unnamed)"));
        goto done;
    }

    /* Mode effectif (profil actif). OFF = l'outil n'est pas annoncé au
     * modèle ; un appel ici est une hallucination : on répond quand même
     * (tout tool_call_id doit recevoir une réponse) sans exécuter. */
    mode = llm_tools_effective_mode(tc->name);
    if (mode == LLM_TOOL_OFF) {
        error = g_strdup_printf(
            _("tool \"%s\" disabled in the current profile."), tc->name);
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
        error = g_strdup_printf(_("invalid JSON arguments for %s."), "bash");
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
        error = g_strdup(_("invalid terminal: expected 0 to 9."));
        goto done;
    }
    if (command == NULL || command[0] == '\0') {
        error = g_strdup(_("missing or empty command."));
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
                _("incomplete tool call ignored: id or function.name absent."));
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
 * pl->core et ignorer ceux que l'annulation a déjà répondus.
 *
 * Quatre choses tiennent la boucle vivante, et la liste est fermée : une
 * requête en vol, une décision ASK à trancher, une commande à exécuter, un
 * résultat à livrer. Les deux dernières se ressemblent et ne se recouvrent
 * pas — cmd_queue est l'outil qu'on va faire, cdb_results est la réponse
 * qu'on doit au modèle. Les tool calls malformés ne touchent jamais la
 * première et remplissent la seconde : omettre l'une des deux fait sonner
 * une fin de tour qui n'a pas eu lieu. */
gboolean
core_agent_loop_alive(LlmCore *c)
{
    if (c == NULL || c->stop_requested)
        return FALSE;
    if (c->cur_req != NULL || c->decision != NULL)
        return TRUE;
    if (c->cmd_queue != NULL && !g_queue_is_empty(c->cmd_queue))
        return TRUE;
    /* Un résultat d'outil qui attend sa livraison EST la boucle : il reste
     * à le rendre au modèle, qui répondra par-dessus. Cette clause est là
     * pour les tool calls MALFORMÉS — arguments JSON invalides, chemin
     * relatif refusé, hash absent : leur refus part du « done: » du dispatch,
     * donc directement en cdb_results, SANS jamais entrer en cmd_queue. Le
     * prédicat les voyait donc mourir à la libération de la requête : chrono
     * re-basé et ding d'une fin qui n'a pas eu lieu, deux lignes avant que
     * llm_cdb_next ne relance la requête. */
    if (c->cdb_results != NULL && !g_queue_is_empty(c->cdb_results))
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
                                _("Cancelled by the user."), TRUE);
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
                                    _("Cancelled by the user."), TRUE);
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
                                        _("Cancelled by the user."), TRUE);
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
    /* Le message du dernier tour, lui, n'a pas de tentative suivante pour le
     * deloger : c'est ici qu'il doit partir. Sur une conversation de 2,7 Mo
     * c'etait 2,7 Mo par tour qui restaient (mesure : +2053 Ko/tour). */
    g_clear_object (&req->msg);
    if (req->pending != NULL)
        g_string_free(req->pending, TRUE);
    g_clear_object (&req->stream);
    /* Une seule liberation, et ce n'est pas un drapeau plante dans req qui
     * la garantit : req est rendue juste en dessous, donc une seconde entree
     * ici lirait de la memoire deja rendue AVANT meme de pouvoir tester
     * quoi que ce soit. L'ancien « if (req->done) return; » etait
     * decoratif — un lecteur, un scripteur, dans le meme appel. L'invariant
     * reel tient ailleurs et tient bien : chaque operation asynchrone ne
     * livre QU'UN callback (garantie GAsyncReadyCallback) et core->cur_req
     * est remis a NULL en tete de cette fonction, avant toute liberation. */
    g_free(req);
    /* Le tour se termine ici, et c'est ICI qu'il faut rendre les pages, pas
     * seulement apres la sauvegarde du live. L'ordre d'un tour est : push ->
     * llm_live_save (trim la) -> llm_send -> llm_body_build (arbre json de
     * la conversation entiere + son texte) -> g_strdup du corps dans le
     * message -> streaming (reply, pending, un JsonParser par evenement SSE)
     * -> cette liberation. Le pic de ~6 Mo d'un fil de 800 Ko nait DONC
     * apres le dernier trim disponible, et comme la reponse qui suit a deja
     * pose des objets sur les blocs libres, malloc_trim ne peut plus rien
     * reprendre : le trou reste inscrit au plancher. Mesure sur la session
     * reelle (tick.sh, 186 s, 13 messages) : +10 108 Ko de RAM privee pour
     * +16 Ko de conversation, soit ~778 Ko par tour — la taille de la
     * conversation, pas celle du message. Avec le trim de llmlive.c, les
     * sauvegardes rendaient bien leurs pages (-1036 et -256 Ko constates),
     * mais le plancher montait quand meme a chaque tour : c'est ce pic-la
     * qui n'etait jamais rendu. Un appel par tour, 0,2 a 1 ms (mem.c). */
    cdb_mem_trim();
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
            hist_append(v, _("\n〔cancelled〕\n"));
            llm_busy_set(v, FALSE);
        }
        llm_request_free(req);
        return;
    }

    if (error != NULL) {
        gboolean cancelled = g_error_matches(error, G_IO_ERROR,
                                             G_IO_ERROR_CANCELLED);
        gboolean timed_out = !cancelled &&
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);

        if (cancelled) {
            g_error_free(error);
            core_history_push(c, LLMACTOR_LLM, FALSE, c->reply->str);
            for (vi = 0; vi < c->views->len; vi++) {
                LlmTile *v = g_ptr_array_index(c->views, vi);

                hist_flush_reply(v);
                llm_slots_title_update(v);
                hist_append(v, _("\n〔cancelled〕\n"));
                llm_busy_set(v, FALSE);
            }
            llm_request_free(req);
            return;
        }
        if (timed_out) {
            /* Timeout en cours de flux (idle ~120 s du provider/proxy).
             * On relance la requête entière sur la config 5xx. Le
             * contenu partiel déjà rendu dans les vues sera remplacé
             * par hist_update_reply au prochain appel (incohérence
             * len < rendered_len → re-rendu complet depuis reply_mark). */
            LlmRetry5xx rc;
            gboolean    infinite;

            llm_retry5xx_load(&rc);
            infinite = rc.max_retries == 0;
            if (rc.retry && (infinite || req->attempt < rc.max_retries)) {
                req->attempt++;
                g_string_truncate(req->pending, 0);
                g_string_truncate(c->reply, 0);
                c->in_reasoning = FALSE;
                if (req->stream != NULL) {
                    g_input_stream_close(req->stream, NULL, NULL);
                    g_clear_object(&req->stream);
                }
                if (req->attempt == 1) {
                    core_cdb_announce(c,
                        _("\n[CDB] Stream timeout — retries in progress…\n"));
                }
                g_error_free(error);
                g_timeout_add((guint)rc.delay_ms, llm_retry_tick, req);
                return;
            }
        }
        if (!timed_out) {
            for (vi = 0; vi < c->views->len; vi++)
                hist_append(g_ptr_array_index(c->views, vi), error->message);
        }
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
        r->text = g_strdup(text != NULL ? text : _("(no error message)"));
    }

    if (pl->core->cdb_results == NULL)
        pl->core->cdb_results = g_queue_new();
    g_queue_push_tail(pl->core->cdb_results, r);

    /* Le « plus » (bash : ALLOW+ et ASK+) : la capture est faite, on
     * remplace l'onglet par un shell FRAIS — la prochaine commande repart
     * d'un environnement propre, comme si Éric avait cliqué « x » puis rouvert. */
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
            _("shell %s does not start (spawn failed?)."),
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
            _("terminal %s was closed during execution."),
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
        gboolean timed_out = !cancelled &&
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);

        if (timed_out) {
            /* G_IO_ERROR_TIMED_OUT : le mur idle (~120 s) du provider ou
             * d'un proxy. Retryable, sur la même config que les 5xx. */
            LlmRetry5xx rc;
            gboolean    retry_on;
            gboolean    infinite;
            int         max_retries, delay_ms;
            llm_retry5xx_load(&rc);
            retry_on = rc.retry;
            max_retries = rc.max_retries;
            delay_ms = rc.delay_ms;
            infinite = max_retries == 0;

            if (retry_on && (infinite || req->attempt < max_retries)) {
                req->attempt++;
                g_string_truncate(req->pending, 0);
                if (req->attempt == 1) {
                    core_cdb_announce(c,
                        _("\n[CDB] Timeout — retries in progress…\n"));
                }
                g_error_free(error);
                g_timeout_add((guint)delay_ms, llm_retry_tick, req);
                return;
            }
        }

        if (!cancelled && !timed_out) {
            char *note = g_strdup_printf(_("\n[error: %s]\n"),
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
                        _("\n[CDB] HTTP %u — retries in progress…\n"),
                        status);

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
/* Un essai d'envoi : reconstruit un SoupMessage neuf depuis la requête
 * stockée. Contre l'intuition — et contre l'ancien commentaire, qui disait
 * que « la session possède le message après send_async » : send_async ne vole
 * PAS la référence de l'appelant, la session tient la sienne et la rend à la
 * fin. C'est donc bien à nous de rendre la nôtre, voir plus bas. */
void
llm_send_attempt(LlmRequest *req)
{
    SoupMessage *msg = soup_message_new("POST", req->url);

    /* Une URL de provider malformée (un api_url saisi à la main) donne ici
     * msg == NULL, et libsoup se contente d'un g_return_if_fail dans
     * send_async : le callback ne vient JAMAIS. Sans cette garde, la
     * requête partait avec ses vues marquées busy plus bas — bouton pause
     * éternel sur une boucle morte, et ta loi « play/pause = état de la
     * boucle » mentirait à l'écran. On reprend l'idiome de l'échec d'envoi :
     * on annonce, on rend la main sur chaque vue, on libère. */
    if (msg == NULL) {
        LlmCore *c = req->core;

        core_cdb_announce(c, _("\n[error: invalid provider URL]\n"));
        for (guint vi = 0; vi < c->views->len; vi++)
            llm_busy_set(g_ptr_array_index(c->views, vi), FALSE);
        llm_request_free(req);
        return;
    }

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
    /* Le message de la tentative precedente — une ratee, un retry 429 —
     * n'est tenu que par nous : libsoup rend SA reference quand la requete
     * se termine (verifie au weak pointer, pas au souvenir). Sans ce clear,
     * chaque tentative laisse un SoupMessage et sa copie du corps — la
     * conversation entiere serialisee — sans plus aucun pointeur dessus. */
    g_clear_object (&req->msg);
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
        _("Éric has REFUSED this tool call. This is not a bug: "
          "it is a decision. Adapt and propose something else.");

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

/* Lance l'exécution d'une commande déjà validée. Chemin unique pour ASK
 * et ASK+ (après clic) comme pour ALLOW et ALLOW+ (auto, sans UI).
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
            _("terminal %s unavailable (bash panel absent?)"),
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
            _("terminal %s unavailable (bash panel absent?)"),
            pl->tab_label);
        core_tool_result_commit(c, pl->tool_call_id, note, TRUE);
        g_free(note);
        g_free(pl->tool_call_id);
        g_free(pl->tab_label);
        g_free(pl);
        llm_cdb_next(c);
    }
}

/* Exécuter une spec déjà validée. Chemin unique pour ASK/ASK+ (après clic)
 * et ALLOW/ALLOW+ (direct). Ne libère rien : l'appelant possède la spec, et
 * c'est sp->mode — jamais un booléen apporté — qui désigne l'effet « plus ».
 * Bash est asynchrone ; les outils fichiers ont déjà rendu leur résultat. */
static void
cdb_run_spec(LlmCore *c, CdbCmdSpec *sp)
{
    /* Le « plus » se LIT ici, dans le mode effectif pose au dispatch : ni
     * ASK-apres-clic ni le dispatch n'ont a le connaitre avant. Seul bash
     * honore l'effet ; un outil fichier en ASK+ s'execute pareil. */
    gboolean allowplus = llm_tool_mode_has_plus(sp->mode);

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
    case CDB_SPEC_REMOVE:
        cdb_tool_file_remove(c, sp->tool_call_id, sp->args_json);
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

    /* ASK / ASK+ approuvé : la boîte a verdit, puis l'action part. Le « plus »
     * ne se décide pas ici — cdb_run_spec le lit dans d->spec->mode. */
    is_bash = (d->spec != NULL && d->spec->kind == CDB_SPEC_BASH);
    cdb_run_spec(c, d->spec);
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

        /* ALLOW / ALLOW+ : demande ACCEPTEE D'AVANCE : elle s'exécute sans
         * attendre Éric, mais ce n'est pas une absence de demande — c'est une
         * demande accordée d'avance, et elle reste visible : chaque vue la met
         * dans SA boîte, zone déjà verte, libellée « autorisé » (dire
         * « exécuté » mentirait : personne n'a cliqué). L'output y entrera
         * comme pour un ASK. Is_auto ENUMERE les permissifs : nier ASK
         * rendrait auto tout mode ajoute demain — ASK+ compris, un ASK. */
        if (llm_tool_mode_is_auto(sp->mode)) {
            gboolean allowplus = llm_tool_mode_has_plus(sp->mode);

            for (guint vi = 0; vi < c->views->len; vi++)
                llm_tile_box_auto(g_ptr_array_index(c->views, vi),
                                  sp->summary, sp->tool_call_id, allowplus);
            cdb_run_spec(c, sp);   /* le « plus » se relit dans sp->mode */
            cdb_cmd_spec_free(sp);
            if (is_bash)
                return;   /* le poll rappellera llm_cdb_next */
            continue;     /* fichier : déjà fini, on enchaîne */
        }

        /* ASK et ASK+ : la décision POSSÈDE la spec — rien n'est copié ici,
         * donc rien ne s'oublie à la libération. sp->mode porte le « plus ». */
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

/* Schéma de l'outil bash (canal natif). */
static void
tools_schema_cdb_bash(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "bash");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(
        builder,
        _("Runs a shell command in a CDB terminal. The command is "
          "submitted to Éric for approval before execution."));
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
        builder, _("CDB terminal number (0 to 9)."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "command");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(
        builder, _("Full shell command to run."));
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

/* Schéma de l'outil read. */
static void
tools_schema_cdb_read(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "read");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Reads an exact range of lines from a text file on disk "
          "(absolute path). from_line/to_line are 1-based, inclusive. "
          "Returns the lines and a short hash (4 base36 chars) of the "
          "exact octets of the range, to replay in insert/replace."));
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

/* Schéma de l'outil insert. */
static void
tools_schema_cdb_insert(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "insert");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Insert text verbatim between two ADJACENT lines of a text file "
          "on disk (absolute path). The model must have read the line "
          "before and the line after SEPARATELY (read with "
          "from_line==to_line) and replay their two hashes. before_line=0 "
          "or after_line=0 denotes a file bound: no real line, so no hash. "
          "The newlines of text belong to the model. Does not create "
          "the file and never writes into the "
          "editor's dirty buffer."));
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
        _("Real line after which to insert; 0 = file head."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "hash_before");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Hash returned by read(before_line,before_line). Omit only when "
          "before_line=0 — giving it at the head of the file is refused."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "after_line");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "integer");
    json_builder_set_member_name(builder, "minimum");
    json_builder_add_int_value(builder, 0);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Real line before which to insert; equals before_line+1, or 0 "
          "to insert at the end of the file."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "hash_after");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Hash returned by read(after_line,after_line). Omit only when "
          "after_line=0 — giving it at the tail is refused."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "text");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Text inserted verbatim, newlines included. Unlike replace, "
          "insert may add any number of lines."));
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

/* Schéma de l'outil remove. Miroir destructif de insert : il retire des
 * lignes ENTIERES et n'en écrit aucune. */
static void
tools_schema_cdb_remove(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "remove");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Removes n WHOLE lines (from_line..to_line, inclusive, 1-based) "
          "from a text file on disk. Nothing is written, nothing is "
          "fused: the lines and their newlines go away together. Like "
          "insert, you must prove the context: hash_before is the line "
          "above the block (omit it when from_line is 1), hash_target is "
          "read(from_line,to_line) itself, hash_after is the line below "
          "(omit it ONLY when the block ends at the last line). To drop "
          "10 lines, read those 10 lines and remove them in one call. "
          "The result announces how the following lines shifted. Never "
          "touches the editor's dirty buffer."));
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
        _("Last line removed, inclusive. Must exist."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "hash_before");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Hash of line from_line-1, from read(N,N). Omit when "
          "from_line is 1."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "hash_target");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Hash of the block itself, from read(from_line, to_line). "
          "This is the proof you read the very lines you destroy."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "hash_after");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Hash of line to_line+1, from read(N,N). Omit only when the "
          "block ends at the last line — omitting it elsewhere is "
          "refused."));
    json_builder_end_object(builder);

    json_builder_end_object(builder); /* properties */
    json_builder_set_member_name(builder, "required");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "path");
    json_builder_add_string_value(builder, "from_line");
    json_builder_add_string_value(builder, "to_line");
    json_builder_add_string_value(builder, "hash_target");
    json_builder_end_array(builder);
    json_builder_end_object(builder); /* parameters */
    json_builder_end_object(builder); /* function */
    json_builder_end_object(builder); /* tool */
}

/* Schéma de l'outil replace. Contrat k -> k, lignes entieres. */
static void
tools_schema_cdb_replace(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "replace");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Replaces a block of whole lines (from_line..to_line, inclusive, "
          "1-based) of a text file on disk, ONE LINE FOR ONE LINE: send "
          "exactly as many entries in lines as the block has. replace "
          "never changes the file's line count — that is the whole point. "
          "To shrink, use remove; to grow, use insert; to rewrite a whole "
          "file, remove it then insert it. Each element is the full new "
          "content of one line WITHOUT its newline: an empty string \"\" "
          "writes an EMPTY line, it does not delete one. CDB joins the "
          "lines with the newline style the file already uses and never "
          "adds or removes the file's final newline. Proof required: "
          "hash_target is read(from_line,to_line), plus hash_before "
          "(line above, omit at line 1) and hash_after (line below, omit "
          "only at the last line). Writing back the block unchanged is "
          "refused. Never touches the editor's dirty buffer."));
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
        _("Last line replaced, inclusive. Must exist."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "hash_before");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Hash of line from_line-1, from read(N,N). Omit when from_line "
          "is 1."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "hash_target");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Hash of the block itself, from read(from_line, to_line). A "
          "refusal never gives it back: re-read."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "hash_after");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Hash of line to_line+1, from read(N,N). Omit only when the "
          "block ends at the last line."));
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "lines");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "array");
    json_builder_set_member_name(builder, "items");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "string");
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("The new lines, one element per line, WITHOUT newlines. The "
          "count must equal to_line - from_line + 1 exactly. A single "
          "element may be \"\" (an empty line) but may not contain \\n or "
          "\\r."));
    json_builder_end_object(builder);

    json_builder_end_object(builder); /* properties */
    json_builder_set_member_name(builder, "required");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "path");
    json_builder_add_string_value(builder, "from_line");
    json_builder_add_string_value(builder, "to_line");
    json_builder_add_string_value(builder, "hash_target");
    json_builder_add_string_value(builder, "lines");
    json_builder_end_array(builder);
    json_builder_end_object(builder); /* parameters */
    json_builder_end_object(builder); /* function */
    json_builder_end_object(builder); /* tool */
}

/* Schéma de l'outil create. */
static void
tools_schema_cdb_create(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "create");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Creates a NEW text file on disk (absolute path) with this "
          "content verbatim. Refuses if the file already exists (use "
          "replace) and if the parent folder is missing (never creates "
          "folders). The write is exclusive (O_EXCL): no race can overwrite "
          "a file that appeared in the meantime. Empty content creates an "
          "empty file."));
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
        _("Content written verbatim, newlines included. Empty string = "
          "empty file."));
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

/* Schéma de l'outil delete. */
static void
tools_schema_cdb_delete(JsonBuilder *builder)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "function");
    json_builder_set_member_name(builder, "function");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "delete");
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder,
        _("Destroys a file on disk (absolute path) in TWO passes. Without "
          "file_hash: deletes NOTHING and returns the file's current "
          "fingerprint. With a file_hash that still matches: deletes. With "
          "a stale hash: refusal. Refuses directories, symlinks and "
          "anything that is not a regular file."));
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
        _("Fingerprint returned by the first delete(path). Omit for the "
          "confirmation request."));
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
        N_("\n\n# CDB native tools\n\n"
        "Use only the native tools to act.\n"
        "A tool result with content:null means there is no new "
        "content compared with the previous results of the same "
        "terminal.\n\n"
        "## bash\n"
        "Runs a shell command in a CDB terminal (0-9).\n\n"
        "## read\n"
        "Reads an exact range of lines (absolute path, 1-based inclusive). "
        "Returns the lines + a short hash (4 base36 chars) covering "
        "the exact octets of the range read. That hash proves you read the "
        "zone and must be replayed by the writing tools. To get "
        "the hash of ONE line, read exactly that line "
        "(from_line==to_line).\n\n"
        "## insert\n"
        "Inserts text between two adjacent lines of a file on the "
        "disk. You MUST have read the line before and the line after, "
        "each by its own read(N,N), and replay their two "
        "hashes. before_line=0 or after_line=0 denotes a bound of the "
        "file: there, no hash. The text is inserted verbatim, like "
        "cat: the newlines are yours, and text that does "
        "not end with a newline merges with the next line. "
        "CDB returns the REAL range and only strikes a hash on the "
        "lines entirely provided by you (authored_range): "
        "a line mixing your text with existing content stays "
        "without a hash, so without an immediate right to write.\n\n"
        "## replace\n"
        "Replaces the lines from_line..to_line (inclusive, 1-based) with your "
        "text verbatim. block_hash is MANDATORY and must come from a "
        "read of that exact range: without it, or if it no longer "
        "matches, refusal. The replaced range includes the trailing newline "
        "of to_line. Empty text deletes the lines. A refusal will never "
        "give you the current hash: re-read.\n\n"
        "## create\n"
        "Creates a NEW file (absolute path) with content verbatim. Refuses "
        "if the file exists or if the parent is missing; never creates a "
        "folder. All content coming from you, authored_range covers the "
        "whole file.\n\n"
        "## delete\n"
        "TWO passes, mandatory: delete(path) deletes nothing and "
        "returns file_hash; delete(path, file_hash) deletes only if "
        "the fingerprint is still the right one. That hash is not proof of "
        "reading: it certifies that the file did not change between your "
        "discovery and its destruction. folder and symlink = refusal.\n");

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
                                                           "bash");
        LlmToolMode        bash_mode = llm_tool_pref_mode(bash_pref, prof);
        gboolean           announce_bash = (bash_mode != LLM_TOOL_OFF);
        const LlmToolPref *read_pref = llm_tools_pref_find(prefs,
                                                           "read");
        LlmToolMode        read_mode = llm_tool_pref_mode(read_pref, prof);
        gboolean           announce_read = (read_mode != LLM_TOOL_OFF);
        const LlmToolPref *ins_pref = llm_tools_pref_find(prefs,
                                                         "insert");
        LlmToolMode        ins_mode = llm_tool_pref_mode(ins_pref, prof);
        gboolean           announce_insert = (ins_mode != LLM_TOOL_OFF);
        const LlmToolPref *rem_pref = llm_tools_pref_find(prefs,
                                                          "remove");
        LlmToolMode        rem_mode = llm_tool_pref_mode(rem_pref, prof);
        gboolean           announce_remove = (rem_mode != LLM_TOOL_OFF);
        const LlmToolPref *rep_pref = llm_tools_pref_find(prefs,
                                                          "replace");
        LlmToolMode        rep_mode = llm_tool_pref_mode(rep_pref, prof);
        gboolean           announce_replace = (rep_mode != LLM_TOOL_OFF);
        const LlmToolPref *cre_pref = llm_tools_pref_find(prefs,
                                                          "create");
        LlmToolMode        cre_mode = llm_tool_pref_mode(cre_pref, prof);
        gboolean           announce_create = (cre_mode != LLM_TOOL_OFF);
        const LlmToolPref *del_pref = llm_tools_pref_find(prefs,
                                                          "delete");
        LlmToolMode        del_mode = llm_tool_pref_mode(del_pref, prof);
        gboolean           announce_delete = (del_mode != LLM_TOOL_OFF);
        guint              n_enabled = (announce_bash ? 1 : 0) +
                                       (announce_read ? 1 : 0) +
                                       (announce_insert ? 1 : 0) +
                                       (announce_remove ? 1 : 0) +
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
            if (announce_remove)
                tools_schema_cdb_remove(builder);
            if (announce_replace)
                tools_schema_cdb_replace(builder);
            if (announce_create)
                tools_schema_cdb_create(builder);
            if (announce_delete)
                tools_schema_cdb_delete(builder);
            /* futurs outils : autres schémas + tests de mode ici.
             * Note pour la passe suivante : ces sept blocs de quatre
             * lignes sont la derniere copie collee du fichier. Une table
             * { nom, schema } les remplacerait, mais le garde
             * n_enabled > 0 est imbrique avec l'ouverture de "messages"
             * (un provider qui refuse les outils doit quand meme recevoir
             * sa persona) : a refondre ensemble, pas a moitie. */
            json_builder_end_array(builder);

            json_builder_set_member_name(builder, "tool_choice");
            json_builder_add_string_value(builder, "auto");

            json_builder_set_member_name(builder, "messages");
            json_builder_begin_array(builder);

            /* Persona utilisateur + politique du canal tools. */
            base_persona = llm_persona_load(t);
            persona = g_strconcat(base_persona != NULL
                                      ? base_persona : "",
                                  _(tools_policy), NULL);
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
    g_object_set(c->soup, "timeout", 300, NULL);  /* secondes */
    c->cancel = g_cancellable_new();
    c->reply = g_string_new(NULL);
    c->history = g_array_new(FALSE, FALSE, sizeof(LlmMsg));
    c->views = g_ptr_array_new();
    c->answered_tools = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    {
        LlmHarnessNames n;

        /* Noms des acteurs du fil : lus une fois ici, reposés par
         * Settings → LLM → Harness → Noms (llm_views_names_changed). */
        llm_harness_names_load(&n);
        c->name_user = n.user;
        c->name_assistant = n.assistant;
    }
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
    g_free(c->name_user);
    g_free(c->name_assistant);
    g_free(c);
    /* La conversation vient de rendre son historique, ses buffers et ses
     * vues — le plus gros lot de petits objets que CDB libère. Ces pages ne
     * reviennent pas au noyau d'elles-mêmes : sans ce trim, le pic de la
     * session dernière resterait l'empreinte de la session suivante. */
    cdb_mem_trim();
}
