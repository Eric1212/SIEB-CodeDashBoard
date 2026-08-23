/*
 * llm.h : tuile LLM — chat avec un provider OpenAI-compatible.
 *
 * Config par session : ~/.config/cdb/<NNN>/llm.json
 *   {
 *     "providers": {
 *       "OpenRouter": {
 *         "api_url":      "https://openrouter.ai/api/v1",
 *         "api_key":      "sk-or-…",
 *         "default_model": "stealth/ox-alpha"
 *       }
 *     },
 *     "active": { "provider": "OpenRouter", "model": "stealth/ox-alpha" }
 *   }
 *
 * Schéma providers inspiré du « openai_compatible » de Zed
 * (~/.config/zed/settings.json).
 */

#ifndef CDB_LLM_H
#define CDB_LLM_H

#include <gtk/gtk.h>

/* Configuration LLM chargée depuis llm.json (possédée par App). */
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

/* Crée/met à jour le provider dans llm.json (api_key + default_model),
 * en préservant les autres providers ; positionne aussi « active ».
 * Crée le fichier s'il n'existe pas. */
void llm_config_save_provider(const char *provider, const char *api_key,
                              const char *default_model);

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

/* Prompt d'initialisation (« Init-Prompt ») :
 * raw = texte brut de prompts/default.txt (fallback défaut intégré),
 * save = écriture (crée les dossiers). Chaînes à libérer. */
char *llm_persona_raw(void);
void  llm_persona_save(const char *text);

/* Charge la config de retry ; applique les défauts si absente/invalide. */
void llm_retry429_load(LlmRetry429 *out);

/* Sauvegarde la config de retry (bornes forcées : 0-5000 et 10-100000). */
void llm_config_save_retry429(gboolean retry, int max_retries, int delay_ms);

/* Crée la VUE de la tuile « llm » (historique + saisie). */
GtkWidget *llm_tile_new(const LlmConfig *cfg,
                        GActionGroup *actions);

#endif /* CDB_LLM_H */
