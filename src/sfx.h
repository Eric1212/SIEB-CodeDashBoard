/*
 * sfx.h : les deux « ding » de CDB.
 *
 *   turn-done (long)  — la boucle agentique vient de finir un tour.
 *   feedback  (court) — un changement vient d'être confirmé (ibox tranchée,
 *                       réglage enregistré…).
 *
 * Implémentation : sfx.c. Les fichiers vivent dans resources/sounds/,
 * résolus relativement au binaire (comme les catalogues i18n). Aucun effet
 * si le lecteur ou le fichier manque : le son n'est jamais critique.
 */
#ifndef CDB_SFX_H
#define CDB_SFX_H

#include <glib.h>

/* Après session_init() : lit resources/sounds/, détecte le lecteur, les
 * préférences d'activation. Idempotent et sans appel de widget. */
void sfx_init(void);

/* Joue si l'effet est activé. feedback() est throttlée (anti-mitraillette
 * quand un spin envoie un « changed » par crant). */
void sfx_play_turn_done(void);
void sfx_play_feedback(void);

/* Ignorent l'activation — le bouton ▶ des réglages, pour entendre un son
 * qu'on n'a pas encore activé. Respectent la disponibilité réelle (le
 * fichier et le lecteur existent-ils ? c'est la vraie question du test). */
void sfx_preview_turn_done(void);
void sfx_preview_feedback(void);

/* Activation, persistée dans layout.json au set. */
gboolean sfx_enabled_turn_done(void);
gboolean sfx_enabled_feedback(void);
void     sfx_set_enabled_turn_done(gboolean on);
void     sfx_set_enabled_feedback(gboolean on);

#endif /* CDB_SFX_H */
