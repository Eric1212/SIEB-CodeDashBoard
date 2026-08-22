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
typedef void (*LlmModelsCallback)(char **ids, gpointer user_data);

void llm_models_fetch(const char *provider, LlmModelsCallback cb,
                      gpointer user_data);

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

/* Bascule provider + modèle actifs : persiste « active » dans llm.json
 * ET met cfg à jour en place (provider/model/api_url/api_key) — le
 * prochain envoi partira chez le provider choisi. */
void llm_config_switch_active(LlmConfig *cfg, const char *provider,
                              const char *model);

/* Crée la VUE de la tuile « llm » (historique + saisie). */
GtkWidget *llm_tile_new(const LlmConfig *cfg,
                        GActionGroup *actions);

#endif /* CDB_LLM_H */
