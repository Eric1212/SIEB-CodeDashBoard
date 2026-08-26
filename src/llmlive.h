/*
 * llmlive.h : persistance « dirty » de la conversation du core.
 *
 * llm_live.json (dossier de session) miroite l'historique COMPLET du
 * core pour survivre crashes/redémarrages. Ce n'est PAS une
 * sauvegarde réelle : les slots restent la seule vérité sauvegardée
 * (loi d'Éric, fil 9).
 */

#ifndef CDB_LLM_LIVE_H
#define CDB_LLM_LIVE_H

#include "llm.h"

/* Sérialise c->history vers llm_live.json ; historique vide → fichier
 * supprimé. */
void llm_live_save(LlmCore *c);

/* Recharge llm_live.json dans c->history (au boot du core). Fichier
 * absent ou corrompu → historique vide, sans erreur. */
void llm_live_load(LlmCore *c);

/* Supprime llm_live.json (« Vider le chat »). */
void llm_live_wipe(void);

#endif /* CDB_LLM_LIVE_H */
