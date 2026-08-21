/*
 * session.h : sessions isolées par numéro (000-999).
 *
 * Chaque session est un processus CDB indépendant avec son dossier de
 * config complet : ~/.config/cdb/<NNN>/
 * (roots.json, dirty.json, layout.json, window.json, session.json).
 *
 * Le lancement direct du binaire ouvre 000 s'il n'existe aucune autre
 * instance ; sinon un dialogue demande le numéro. CDB_SESSION=<NNN>
 * court-circuite le dialogue (spawn depuis « New Session »).
 */

#ifndef CDB_SESSION_H
#define CDB_SESSION_H

#include <gtk/gtk.h>

#define SESSION_MIN 0
#define SESSION_MAX 999

/* Numéro de session résolu au démarrage (variable globale simple : le
 * module est initialisé une fois dans main avant toute use). */
extern int sieb_session;

/* Initialise la session : lit CDB_SESSION si présent et valide ;
 * sinon détecte les autres instances (même binaire) — 000 si aucune,
 * dialogue interactif sinon. Renvoie FALSE si annulé (quitter). */
gboolean session_init(void);

/* Chemin du fichier path DANS la session courante :
 * ~/.config/cdb/<NNN>/<path>. g_strdup à libérer. */
char *session_config_path(const char *path);

/* Racine de la session courante (~/.config/cdb/<NNN>/).
 * g_strdup à libérer. */
char *session_dir(void);

/* Crée la session si elle n'existe pas : copie du plus petit numéro
 * existant (000 si aucun). Idempotent. */
void session_ensure(void);

/* Spawn d'une nouvelle instance du binaire sur la session n.
 * Renvoie FALSE en cas d'échec du spawn. */
gboolean session_spawn_new(int n);

/* TRUE si au moins un AUTRE processus vivant exécute le même binaire. */
gboolean session_other_instance_running(void);

/* Dialogue modale : numéro de session (000-999). Renvoie -1 si annulé. */
int session_pick_dialog(GtkWindow *parent);

#endif /* CDB_SESSION_H */
