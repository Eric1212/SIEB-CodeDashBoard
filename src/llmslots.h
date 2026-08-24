/*
 * llmslots.h : persistance des slots JSON de la tuile LLM.
 *
 * Chaque session (000-999) possède son dossier de slots :
 *   ~/.config/cdb/<NNN>/llm_slots/
 * Un slot vide = fichier absent. Le « dernier envoyé » est last.json.
 */

#ifndef CDB_LLMSLOTS_H
#define CDB_LLMSLOTS_H

#include <glib.h>

/* Répertoire des slots de la session courante (créé au besoin).
 * Renvoie un chemin g_strdup — libérer avec g_free. */
char *llm_slots_dir(void);

/* TRUE si le répertoire llm_slots de la session `session` existe. */
gboolean llm_slots_dir_exists(int session);

/* Écrit last.json (dernier JSON réellement envoyé). */
gboolean llm_slots_last_save(const char *json);

/* Lit last.json. Renvoie g_strdup ou NULL si absent. */
char *llm_slots_last_load(void);

/* Écrit <slot>.json (0-999). Écrase si existant. */
gboolean llm_slots_save(int slot, const char *json);

/* Lit <slot>.json. Renvoie g_strdup ou NULL si absent/invalide. */
char *llm_slots_load(int slot);

/* TRUE si <slot>.json existe. */
gboolean llm_slots_exists(int slot);

/* TRUE si <slot>.json existe DANS la session `session` (import). */
gboolean llm_slots_exists_in(int session, int slot);

/* Supprime <slot>.json. Silencieux si absent. */
void llm_slots_clear(int slot);

/* Copie le slot src_slot de la session src_session vers dst_slot
 * de la session courante. Renvoie FALSE si la source est absente. */
gboolean llm_slots_import(int src_session, int src_slot, int dst_slot);

#endif /* CDB_LLMSLOTS_H */
