/*
 * llm.h : tuile LLM — chat avec un provider OpenAI-compatible.
 *
 * Config par session : ~/.config/cdb/<NNN>/llm.json. C'est le fichier d'état
 * de la SESSION, pas seulement du LLM : sept écritures s'y partagent (cinq
 * dans llmcore.c, deux dans llmtoolpref.c), toutes en
 * lecture-modification-écriture, donc un membre survit à l'écriture d'un
 * autre. Forme réelle, relevée sur un fichier vivant :
 *   {
 *     "providers": { "OpenRouter": { "api_url", "api_key",
 *                                    "allowed_models" }, … },
 *     "active":    { "provider", "model", "profile" },
 *     "harness":   { "retry_429", "max_retries_429", "delay_ms_429",
 *                    "retry_5xx", "max_retries_5xx", "delay_ms_5xx" },
 *     "tools":     [ { "name", "modes" }, … ],
 *     "roots":     [ { "path", "kind" }, … ],   <- explorateur (roots.c)
 *     "last_file": "…"                          <- explorateur (roots.c)
 *   }
 *
 * « active » est le SEUL couple provider/modèle utilisé pour le chat ;
 * il est écrit par switch_active (menu de la tuile), jamais par les
 * formulaires Settings. Pas de « default_model » : aucun repli.
 */

#ifndef CDB_LLM_H
#define CDB_LLM_H

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>   /* JsonNode / JsonObject : membres étrangers de llm.json */

/* Configuration LLM chargée depuis llm.json (possédée par App).
 * model peut être NULL/vide : aucun modèle actif tant que l'utilisateur
 * n'en choisit pas un dans le menu de la tuile. */
typedef struct {
    char *provider;        /* nom du provider actif ("OpenRouter") */
    char *model;           /* modèle actif ("stealth/ox-alpha") */
    char *api_url;         /* base API du provider actif */
    char *api_key;         /* clé du provider actif */
} LlmConfig;

/* Charge ~/.config/cdb/<NNN>/llm.json ; NULL si absent/invalide
 * (la tuile affiche alors un message d'aide au lieu du chat). */
LlmConfig *llm_config_load(void);

void llm_config_free(LlmConfig *cfg);

/* Crée/met à jour le provider dans llm.json (api_key), en préservant
 * les autres providers et « active » (posé seulement à la première
 * création). Crée le fichier s'il n'existe pas. */
void llm_config_save_provider(const char *provider, const char *api_key);

/* llm.json est aussi le fichier d'état de la SESSION, pas seulement du LLM :
 * un autre module (l'explorateur, pour ses racines) y loge des membres que le
 * modèle ignore. Ces deux accès sont le SEUL chemin autorisé pour cela — ils
 * relisent, ne remplacent que ce qu'on leur donne, et laissent le reste
 * (providers, api_key, active, harness, tools) intact. Les sept écritures
 * existantes du fichier suivent déjà cette règle. */
JsonNode *llm_config_get_member(const char *key);   /* copie, à libérer */
void      llm_config_merge_members(JsonObject *members);

/* Récupère la liste des modèles du provider (GET {api_url}/models).
 * Async : cb(ids, user_data) sur la boucle principale — ids = tableau
 * NULL-terminé POSSEDÉ par llm.c : valide uniquement PENDANT le
 * callback, ne pas libérer. NULL si échec.
 * Le callback peut toucher l'UI directement (contexte principal). */
/* Un modèle tel que renvoyé par /models. */
typedef struct {
    char *id;    /* slug technique (ex: "stealth/ox-alpha") */
    char *name;  /* nom lisible fourni par le provider, NULL sinon */
} LlmModelInfo;

typedef void (*LlmModelsCallback)(LlmModelInfo *models, gpointer user_data);

void llm_models_fetch(const char *provider, LlmModelsCallback cb,
                      gpointer user_data);

/* Copie/libération de tableaux NULL-terminés (id==NULL). */
LlmModelInfo *llm_models_copy(const LlmModelInfo *models);
void llm_models_free(LlmModelInfo *models);

/* URL de base d'un provider connu ; NULL si inconnu. */
const char *llm_provider_default_url(const char *provider);

/* ------------------------------------------------ */
/* Solde du provider (GET {api_url}/credits)         */
/*                                                    */
/* Liste curated : voir la table CREDITS_LIST dans    */
/* llmcore.c. Ce qui n'y est pas n'est PAS sondé.     */
/* ------------------------------------------------ */

/* Où lire le solde d'UN provider connu, et dans quelle unité. */
typedef struct {
    const char *provider;
    const char *member;       /* nom du champ du solde          */
    gboolean    under_data;   /* OpenRouter nid sous « data »,
                               * HyperCharm est à la racine : il
                               * n'y a rien de commun à déduire  */
    const char *unit;         /* unité d'origine du solde rendu */
    double      usd_per_unit; /* multiplicateur vers USD        */
} CreditsProvider;

/* Entrée de la liste curated ; NULL si le provider n'y est pas — donc
 * si aucune requête ne doit être émise. */
const CreditsProvider *llm_credits_entry(const char *provider);

/* available = FALSE ⇒ aucun solde LISIBLE (hors liste, pas de clé,
 * erreur HTTP, forme inattendue) : le badge reste à « — », jamais ne
 * tombe à 0. usd est déjà converti ; raw est la valeur telle que rendue
 * par le provider (la tooltip s'en sert). Le callback peut toucher l'UI.
 * Renvoie FALSE si AUCUNE requête n'est partie — callback jamais appelé,
 * c'est à l'appelant d'afficher « — » et d'en dire la raison. */
typedef void (*LlmCreditsCallback)(gboolean available, double usd,
                                   double raw, gpointer user_data);

gboolean llm_credits_fetch(const char *provider, LlmCreditsCallback cb,
                           gpointer user_data);

/* Clé API sauvegardée d'un provider (indépendant du provider actif).
 * get : chaîne g_strdup à libérer ; NULL si absente. */
char *llm_config_get_api_key(const char *provider);

/* Filtre de modèles autorisés du provider (chaîne brute : liste séparée
 * par virgules d'ids exacts). NULL/"" = tous les modèles.
 * get : chaîne g_strdup à libérer ; NULL si absente. */
char *llm_config_get_allowed_models(const char *provider);
void llm_config_set_allowed_models(const char *provider, const char *filter);

/* TRUE si id passe le filtre (vide/tout, ou présent après trim). */
gboolean llm_model_allowed(const char *filter, const char *id);

/* Noms des providers connus (clés de la map « providers »).
 * Tableau NULL-terminé à libérer (g_strfreev) ; NULL si aucun. */
char **llm_config_provider_names(void);

void llm_config_switch_active(LlmConfig *cfg, const char *provider,
                              const char *model);

/* Configuration du retry sur HTTP 429 (section Harness des settings,
 * persistée dans llm.json : harness.retry_429 / max / delay_ms). */
typedef struct {
    gboolean retry;        /* réessayer automatiquement ? (défaut TRUE) */
    int      max_retries;  /* 200 par défaut ; 0 = infini ; borne 5000 */
    int      delay_ms;     /* attente entre essais (défaut 250 ; 10-100000) */
} LlmRetry429;

#define LLM_RETRY429_DEFAULTS     (LlmRetry429){ .retry = TRUE, .max_retries = 200, .delay_ms = 250 }

/* Configuration du retry sur HTTP 5xx (500-504 : erreurs transitoires
 * de l'upstream — surcharge, maintenance). Rythme plus lent que le 429 :
 * un serveur en 503 a besoin de temps, pas d'insistance. */
typedef struct {
    gboolean retry;        /* réessayer automatiquement ? (défaut TRUE) */
    int      max_retries;  /* 120 par défaut ; 0 = infini ; borne 5000  */
    int      delay_ms;     /* attente entre essais (défaut 1000 ; 10-100000) */
} LlmRetry5xx;

#define LLM_RETRY5XX_DEFAULTS     (LlmRetry5xx){ .retry = TRUE, .max_retries = 120, .delay_ms = 1000 }

/* Prompt d'initialisation (« Init-Prompt ») :
 * raw = texte brut de prompts/default.txt (fallback défaut intégré),
 * save = écriture (crée les dossiers). Chaînes à libérer. */
char *llm_persona_raw(void);
void  llm_persona_save(const char *text);

/* Charge la config de retry ; applique les défauts si absente/invalide. */
/* Modes d'exposition d'un outil natif. OFF signifie tout simplement que
 * l'outil n'est PAS dans le tableau « tools » envoyé au modèle : de son
 * point de vue, l'outil n'existe pas. Aucun message d'erreur à renvoyer. */
typedef enum {
    LLM_TOOL_OFF = 0,
    LLM_TOOL_ASK,       /* annoncé ; approbation d'Éric avant exécution */
    LLM_TOOL_ALLOW,     /* annoncé ; exécution directe */
    LLM_TOOL_ALLOWPLUS  /* ALLOW + effet propre à l'outil (bash : reset) */
} LlmToolMode;

/* Les trois profils, jamais plus. Colonnes de la grille Settings. */
typedef enum {
    LLM_PROFILE_MINIMAL = 0,
    LLM_PROFILE_DEFAULT,
    LLM_PROFILE_YOLO,
    LLM_PROFILE_COUNT
} LlmToolProfile;

extern const char *const LLM_PROFILE_NAMES[LLM_PROFILE_COUNT];
/* Libellés affichables : distincts des NOMS ci-dessus, qui sont des CLES
 * persistées (llm.json active.profile) et comparées en code. */
extern const char *const LLM_PROFILE_LABELS[LLM_PROFILE_COUNT];
extern const char *const LLM_TOOL_MODE_NAMES[4];

/* Préférence d'un outil natif : un mode PAR profil. */
typedef struct {
    char        *name;
    LlmToolMode  modes[LLM_PROFILE_COUNT];
} LlmToolPref;

void llm_retry429_load(LlmRetry429 *out);

/* Sauvegarde la config de retry (bornes forcées : 0-5000 et 10-100000). */
void llm_config_save_retry429(gboolean retry, int max_retries, int delay_ms);

/* Config de retry 5xx : mêmes principes, section harness.retry_5xx. */
void llm_retry5xx_load(LlmRetry5xx *out);
void llm_config_save_retry5xx(gboolean retry, int max_retries, int delay_ms);


/* Crée la VUE de la tuile « llm » (historique + saisie).
 * roots/multi_paths : résolution du projet courant pour les
 * substitutions [PROJET]/[CHEMIN] du prompt (empruntés à App).
 * modal_count : compteur de modales d'App (modale « Voir le JSON »,
 * limité à MODAL_MAX — emprunté). */


#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

/* ===== Types migrés depuis llmcore.c (split C0) ===== */


/* ------------------------------------------------ */
/* Liste des modèles (GET {api_url}/models)          */
/* ------------------------------------------------ */

typedef struct {
    LlmModelsCallback cb;
    gpointer          user_data;
    SoupSession      *soup;
    char             *provider;
    SoupMessage      *msg;    /* notre référence, rendue à md_deliver */
} ModelsFetch;

/* Solde : même mécanique que ModelsFetch. cp pointe dans une table
 * statique du core — durée de vie infinie, rien à libérer. */
typedef struct {
    LlmCreditsCallback     cb;
    gpointer               user_data;
    SoupSession           *soup;
    char                  *provider;
    const CreditsProvider *cp;
    SoupMessage           *msg; /* notre référence, rendue au callback */
} CreditsFetch;

/* ------------------------------------------------ */
/* models.dev : noms lisibles des modèles            */
/*                                                   */
/* Certains providers (OpenCode Zen) ne renvoient    */
/* aucun nom dans /models. Les métadonnées viennent  */
/* du tree models.dev vendoré sous                   */
/* third_party/models-dev/models, lu à la demande    */
/* par md_enrich() — plus d'api.json, plus de        */
/* chargement initial, plus de file d'attente.       */
/*                                                   */
/* Voir llmcore.c pour les règles de résolution.     */
/* ------------------------------------------------ */
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
typedef enum {
    LLM_MSG_TEXT,
    LLM_MSG_ASSISTANT_TOOL_CALLS,
    LLM_MSG_TOOL_RESULT
} LlmMsgKind;

typedef struct {
    char *id;
    char *name;
    char *arguments_json;
} LlmToolCall;

typedef struct {
    long     index;
    char    *id;
    char    *name;
    GString *arguments;
} LlmPendingToolCall;

/* Un échange de l'historique de conversation.
 * local = TRUE : affiché dans le fil mais JAMAIS envoyé au modèle
 * (annonces CDB : erreurs HTTP, changements d'état…). */
typedef struct {
    LlmActor actor;
    gboolean local;
    LlmMsgKind kind;
    char *content;
    GPtrArray *images;

    /* LLM_MSG_ASSISTANT_TOOL_CALLS */
    GPtrArray *tool_calls;

    /* LLM_MSG_TOOL_RESULT */
    char *tool_call_id;
} LlmMsg;

void llm_msg_clear(LlmMsg *m);
GPtrArray *llm_tool_calls_new(void);

/* Requête en cours : définie plus bas, référencée par LlmTile. */
typedef struct LlmRequest LlmRequest;

typedef struct LlmTile LlmTile;

/* Entrée de la file agentique. Définie plus bas (avec CdbResult) mais
 * anticipée ici : une décision en possède une et la libère. */
struct CdbCmdSpec;

/* Décision d'approbation d'outil : état au CORE, rendu par les vues.
 * Une seule instance active à la fois (tête de la file agentique). */
typedef enum {
    CDB_A_PENDING,
    CDB_A_APPROVED,
    CDB_A_REFUSED
} CdbApprovalState;

/* La décision ne copie plus rien : elle POSSÈDE la spec. Un seul
 * libérateur (cdb_decision_free) et aucun champ oubliable. */
typedef struct {
    struct CdbCmdSpec *spec;
    CdbApprovalState   state;
} CdbDecision;

/* État conversationnel PARTAGÉ : possède le réseau et la conversation,
 * survit aux vues (Phase 1 : singleton App ; vue unique transitoire). */
typedef struct {
    SoupSession  *soup;      /* session réseau du fil */
    GCancellable *cancel;    /* annulation de la requête en cours */
    LlmRequest   *cur_req;   /* requête active */
    gboolean      stop_requested; /* pause cliquée : jette tout entrant */
    /* Arête de la boucle agentique pour le ding de fin de tour. La mort
     * d'une requête ne suffit pas à dire que le tour est fini (round d'outils
     * en cours, file non vide) : seul core_agent_loop_alive le sait. On mémorise
     * l'état tenu pour ne sonner QUE le passage vivant→inerte, une fois par
     * core et non par vue. g_new0 le naît à FALSE : un core qui n'a jamais
     * travaillé ne dinge pas. */
    gboolean      loop_alive;
    gboolean      in_reasoning; /* delta courant = thinking */
    GString      *reply;     /* réponse en cours d'accumulation */
    GArray       *history;   /* LlmMsg[] : fil de conversation envoyé */
    GPtrArray    *views;     /* vues attachées (pointeurs empruntés) */
    /* Ressources de session (empruntées à App, vivent plus longtemps) */
    /* Numéro de session dans la titlebar (étiquette « 000 »). Emprunté à
     * App, mais contrairement à cfg/roots il meurt AVANT le core (la fenêtre
     * part avant llm_core_free) : on le détache sur son signal « destroy »
     * pour qu'une complétion réseau en retard ne pose jamais une classe CSS
     * sur un widget finalisé. NULL = pas de fenêtre principale. */
    GtkWidget    *header_session;
    /* Nom de la fenêtre (taskbar / alt-tab). Le titre WM est du texte BRUT :
     * pas de gras possible là-bas — c'est pourquoi l'occupation y est rendue
     * par un préfixe « ▶ », jamais par une graisse. Ces deux pointeurs sont
     * empruntés à App et suivent la VIE DE LA FENÊTRE, pas celle du core :
     * ils sont remis à NULL par header_session_destroyed, l'unique ancre qui
     * éteint à la fois l'étiquette de titlebar et ce callback. */
    void         (*title_sync)(void *app);  /* (void*)window_title_sync */
    void         *title_user;               /* l'App, rendue au callback    */
    /* Mémorise le paramètre `busy` posé par llm_busy_set — la MEME variable
     * que lit l'icône play/pause. Le titre la relit au lieu de recalculer
     * core_agent_loop_alive (qui est busy||alive), sinon un busy=FALSE posé
     * sur boucle vivante afficherait « ▶ » sur une fenêtre dont le bouton
     * est déjà revenu à play. g_new0 : FALSE au départ. */
    gboolean      session_busy;
    LlmConfig    *cfg;
    GListStore   *roots;
    GHashTable   *multi_paths;
    /* Décision en attente (NULL si aucune) */
    CdbDecision  *decision;

    GQueue      *cmd_queue;   /* appels d'outils valides en attente */
    GQueue      *cdb_results; /* résultats pendants */

    /* Boucle tools native : fragments SSE en cours et réponses déjà
     * livrées pour un tool_call_id donné. */
    GPtrArray  *pending_tool_calls;
    GHashTable *answered_tools;
} LlmCore;

typedef struct LlmTile {      /* historique (GtkTextView, non éditable) */
    LlmCore      *core;      /* état partagé (possédé par App) */
    GtkWidget   *view;      /* historique (GtkTextView, non éditable) */
    CdbDecision *shown_decision; /* décision actuellement rendue */
    /* BOÎTES INTERACTIVES de CETTE vue : tool_call_id → boîte encore
     * ouverte. Une TABLE, et non un slot unique, parce qu'un même tour
     * peut aligner plusieurs demandes — surtout maintenant qu'un outil en
     * ALLOW produit lui aussi sa boîte (allow = demande acceptée
     * d'avance, pas absence de demande). Les valeurs sont EMPRUNTÉES au
     * TextView : la boîte meurt avec le fil et son signal « destroy » la
     * retire d'ici. Une boîte y reste même après avoir reçu son résultat,
     * pour pouvoir encore être colorée (le chemin d'annulation livre
     * avant de résoudre). */
    GHashTable  *iboxes;
    GtkTextBuffer *hist;    /* buffer de l'historique */
    GtkWidget   *entry;     /* saisie multi-lignes (GtkTextView) */
    GtkTextBuffer *entry_buf; /* buffer de la saisie */
    GtkWidget   *entry_scroll; /* fenêtre scrollée de la saisie */
    GtkWidget   *compose;      /* bloc de composition (réf pour largeur pop) */
    GtkWidget   *chevron;      /* label ▾/▴ du sélecteur de modèle */
    GtkWidget   *model_phrase; /* label phrasique du sélecteur */
    GtkWidget   *send_btn;
    GtkWidget   *hist_view; /* la vue (pour les boutons dans le fil) */
    LlmConfig   *cfg;
    gboolean     busy;      /* requête en cours */
    gboolean     in_reasoning; /* delta courant = thinking */
    gboolean     follow;     /* scroll auto actif (user en bas) */
    GtkAdjustment *adj;      /* adjustment vertical de l'historique :
                              * scroll mécanique (set_value), sans
                              * déplacer le curseur ni dépendre du
                              * layout asynchrone de la vue */
    gsize        rendered_len; /* nb d'octets de reply déjà rendus dans
                               * le fil (rendu incrémental) */
    GtkTextMark *reply_mark;/* marque de fin de la réponse en streaming */
    GPtrArray   *pending_images; /* images collées, envoyées au prochain tour */
    GtkWidget   *model_btn; /* sélecteur de modèle (menu, label = actif) */
    GtkWidget   *slots_btn; /* bouton menu persistance (slots JSON) */
    GtkWidget   *profile_btn;   /* bouton menu profil actif (global session) */
    GtkWidget   *profile_title; /* titre du popover : profil courant */
    int         *modal_count; /* compteur de modales d'App (emprunté) */
    GtkWidget   *model_pop; /* popover : recherche + sections provider */
    GtkWidget   *model_search; /* filtre live des rangées */
    GtkWidget   *rows_box;  /* conteneur vertical des sections */
    GPtrArray   *sections;  /* ModelSection[] par provider */
    gboolean     menu_built;/* popover peuplé au premier ouvert */
    GActionGroup *actions;  /* pour « Configurer… » (ref ; emprunté sinon) */
    GListStore  *roots;     /* résolution du projet courant (empruntés, */
    GHashTable  *multi_paths; /* comme BashPanel) */
    int          slot_origin;     /* -1 = non sauvegardé, sinon numéro du slot */
    int          turns_since_ref; /* tours utilisateur depuis la référence */
    gsize        ref_body_size;   /* taille du body à la référence */
    GtkWidget   *slots_title;     /* label titre du popover slots */
    GtkWidget   *status_rev;      /* bandeau activité / bilan tokens */
    GtkWidget   *status_logo;       /* animation tournante type RClot */
    int          status_logo_frame;
    GtkWidget   *status_label;    /* temps écoulé */
    GtkWidget   *status_sent_label;
    GtkWidget   *status_received_label;
    GtkWidget   *status_context_label;
    /* Les trois icônes du compteur sont stockées pour être masquées au
     * repos : la rangée de statut reste visible en permanence (c'est le
     * badge de solde qui la justifie) et ne doit pas porter trois flèches
     * symboliques sans chiffres à côté. */
    GtkWidget   *status_up_icon;
    GtkWidget   *status_down_icon;
    GtkWidget   *status_context_icon;
    GtkWidget   *credits_label;     /* solde USD, collé à droite */
    guint        credits_timer_id;  /* keep-alive du solde ; 0 = désarmé */
    guint        status_timeout_id;
    gint64       status_started_us;
    gint64       status_elapsed_us;
    long         tokens_sent;
    long         tokens_received;
    long         tokens_context;
    gboolean     tokens_estimated;
} LlmTile;

/* Résultat d'exécution en attente de livraison. */
typedef struct {
    char    *tool_call_id;
    char    *label;      /* « bash-N » */
    char    *raw_text;   /* capture originale du terminal, ou NULL */
    char    *text;       /* contenu réellement envoyé au modèle */
    gboolean shown;      /* déjà affiché localement (refus/erreur immédiat) */
} CdbResult;

/* Ce que la file agentique sait exécuter. */
typedef enum {
    CDB_SPEC_BASH = 0,
    CDB_SPEC_READ,
    CDB_SPEC_INSERT,
    CDB_SPEC_REPLACE,
    CDB_SPEC_CREATE,
    CDB_SPEC_DELETE
} CdbSpecKind;

/* Spécification d'un appel d'outil natif en file d'attente.
 *   bash    : tab + cmd.
 *   fichiers: args_json, re-parse a l'EXECUTION — c'est ce qui garantit
 *             que l'etat du disque est verifie au moment d'ecrire, et non
 *             au moment de demander (l'approbation peut attendre).
 * summary   : ligne rendue telle quelle par la barre d'approbation. La
 *             tuile ne connait le nom d'aucun outil : le core decide. */
typedef struct CdbCmdSpec {
    char        *tool_call_id;
    CdbSpecKind  kind;
    int          tab;
    char        *cmd;
    char        *args_json;
    char        *summary;
    LlmToolMode  mode;  /* ASK / ALLOW / ALLOWPLUS au moment du dispatch */
} CdbCmdSpec;

/* Une section de provider dans le sélecteur : en-tête + listbox. */
typedef struct {
    char          *provider;
    GtkWidget     *header;
    GtkWidget     *list;   /* GtkListBox single-click */
    LlmModelInfo  *models; /* tableau NULL-terminé (copie possédée) */
} ModelSection;


/* Persistance slots (menu du bouton folder-templates). */

/* Fetch des /models d'UNE section. La ref sur l'ancre garantit que la
 * tuile est vivante au callback, même après un re-rendu du layout. */
typedef struct {
    LlmTile      *t;
    GtkWidget    *anchor; /* ref possédée pendant le vol */
    ModelSection *sec;
} SectionFetchCtx;

struct LlmRequest {
    LlmCore    *core;   /* état partagé — survit aux vues */
    SoupMessage  *msg;  /* NOTRE référence : rendue à llm_send_attempt (essai
                         * suivant) et à llm_request_free (dernier essai) */
    GInputStream *stream;
    char          scratch[4096]; /* buffer du read en cours */
    GString      *pending;      /* lignes SSE partielles (dynamique :
                                  * une ligne data: peut dépasser 8 Ko
                                  * quand le serveur agrège les deltas) */
    /* Pas de drapeau « une seule libération » ici : req est rendue en bas
     * de llm_request_free, donc toute seconde entrée lirait une mémoire
     * déjà rendue avant même de pouvoir tester quoi que ce soit. */
    char         *url;          /* pour reconstruire les essais 429 */
    char         *body;         /* corps JSON de la requête */
    char         *auth;         /* header Authorization ou NULL */
    int           attempt;      /* numéro d'essai courant (0 = premier) */
};

/* Détache la requête en vol d'une tuile sur le point de mourir :
 * les callbacks async verront req->tile == NULL et se retireront
 * sans déréférencer la tuile déjà libérée (sinon use-after-free).
 * Définie ici car le type LlmRequest doit être complet ; appelée
 * depuis llm_tile_free via la déclaration anticipée plus haut. */

/* ------------------------------------------------ */
/* Acteur CDB : contrôle à distance des terminaux    */
/*                                                   */
/* Le modèle demande l'exécution via un tool_call    */
/* (cdb_bash) ; CDB demande l'approbation d'Éric,    */
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

typedef struct {
    LlmCore *core;
    char    *tool_call_id;
    char    *tab_label;
    int      tab;        /* index d'onglet surveillé */
    gchar   *prev_tail;  /* dernière ligne du round précédent */
    int      rounds;     /* rounds consécutifs finissant par un prompt */
    char    *pending_cmd; /* commande en attente du spawn du shell */
    gboolean cancelled;   /* réponse tool déjà envoyée par l'annulation */
    gboolean allowplus;   /* effet spécial « plus » après capture (bash :
                           * reset = fermer l'onglet et le rouvrir frais) */
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

typedef struct {
    GtkWindow *dialog;
    int        result; /* -1 = annulé / invalide */
} NumPickCtx;

/* Dialogue BLOQUANT de confirmation. `destructive` style le bouton OK. */
typedef struct {
    GtkWindow *dialog;
    gboolean   ok;
} ConfirmCtx;

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

/* ===== Globales partagées ===== */
#include "llmcore.h"
#include "llmtile.h"
#include "llmtoolpref.h"

#endif /* CDB_LLM_H */
