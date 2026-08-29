/*
 * llmtoolpref.h : préfs des outils natifs par PROFIL.
 *
 * llm.json porte :
 *   "tools" : [ { "name" : "cdb_bash",
 *                 "modes" : [ "off", "ask", "allow" ] } ]   // par profil
 *   "active" : { ..., "profile" : "DEFAULT" }
 *
 * Trois profils fixes (MINIMAL, DEFAULT, YOLO), quatre modes par outil.
 * OFF = l'outil n'est pas annoncé au modèle (il n'existe pas pour lui).
 *
 * En-tête inclus PAR llm.h (les types LlmToolMode/LlmToolPref y vivent).
 */

#ifndef CDB_LLMTOOLPREF_H
#define CDB_LLMTOOLPREF_H

#include <glib.h>

/* Tableaux de noms (index = LlmToolMode / LlmToolProfile). */
const char *llm_profile_name(LlmToolProfile p);   /* CLE persistée        */
const char *llm_profile_label(LlmToolProfile p);  /* libellé traduisable  */
const char *llm_tool_mode_name(LlmToolMode m);

/* Mode d'un outil pour un profil donné ; OFF si l'outil est inconnu. */
LlmToolMode llm_tool_pref_mode(const LlmToolPref *pref,
                               LlmToolProfile profile);

/* Préfs chargées (défauts appliqués si absentes) ; à libérer. */
GPtrArray  *llm_tools_prefs_load(void);
void        llm_tools_prefs_free(GPtrArray *prefs);

/* La préférence d'un outil par son nom ; NULL si absente. */
const LlmToolPref *llm_tools_pref_find(GPtrArray *prefs, const char *name);

/* Mode effectif d'un outil pour le PROFIL ACTIF. */
LlmToolMode llm_tools_effective_mode(const char *name);

/* Écriture : mode d'un outil dans un profil (upsert complet). */
void llm_config_save_tool_mode(const char *name, LlmToolProfile profile,
                               LlmToolMode mode);

/* Profil actif (persisté dans llm.json active.profile). */
LlmToolProfile llm_config_active_profile(void);
void           llm_config_set_active_profile(LlmToolProfile profile);

#endif /* CDB_LLMTOOLPREF_H */
