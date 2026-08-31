/*
 * sfx.c : lecture des deux ding, sans aucune dépendance audio liée.
 *
 * Chemin des assets. CDB n'a ni préfixe d'installation ni GResource (vérifié :
 * zéro occurrence dans le projet). Le seul précédent d'asset EMBARQUE — non
 * par-session — est po/locale, résolu relativement au binaire via
 * /proc/self/exe (voir i18n.c). On calque exactement ce modèle :
 * <dir_du_binaire>/resources/sounds. `make run` depuis la racine fonctionne
 * donc sans rien installer, comme la langue, et un resources/ absent se tait
 * au lieu de casser.
 *
 * Lecteur. Aucun -dev audio n'est pkg-config-able ici (gstreamer-1.0,
 * libcanberra, pipewire : tous absents). Grossir la PKGS du Makefile (une
 * ligne gcc à la main) pour un son de notification serait lourd. On spawn donc
 * un lecteur CLI déjà présent, détecté une fois : pw-play (PipeWire natif) →
 * paplay (PulseAudio) → ffplay (avec -nodisp -autoexit, sinon une fenêtre
 * SDL s'ouvrirait pour 700 ms de bruit). g_spawn_async + g_child_watch : la
 * boucle GTK n'est jamais bloquée, le pid est rendu, aucun zombie.
 *
 * Dégradation. Pas de lecteur, pas de fichier, effet éteint : silence. On
 * n'écrase rien, on ne crache rien — la même philosophie que gettext qui
 * retombe sur le msgid quand le catalogue manque.
 */
#include "sfx.h"
#include "layout.h"

#include <glib.h>
#include <glib/gstdio.h>   /* g_file_read_link */

/* Sons embarqués (sous <dir binaire>/resources/sounds/).
 * turn-done.mp3 : 1,62 s. feedback.mp3 : 0,72 s. */
#define SND_LONG   "turn-done.mp3"
#define SND_SHORT  "feedback.mp3"

/* Clés de layout.json — sœurs de "language", réglages généraux et non LLM.
 * Valeur "1"/"0" (layout_pref_get ne lit que des chaînes). Absent = activé. */
#define PREF_LONG   "sound_turn_done"
#define PREF_SHORT  "sound_feedback"

#define FEEDBACK_THROTTLE_US 120000   /* 120 ms */

static const char *player;           /* argv[0] ; NULL si aucun lecteur trouvé */
static gboolean    is_ffplay;
static char       *path_long;         /* <dir>/resources/sounds/turn-done.mp3 */
static char       *path_short;
static gboolean    have_long, have_short;   /* lecteur + fichier présents */
static gboolean    en_long = TRUE, en_short = TRUE;
static gboolean    inited;
static gint64      last_short;

/* <dir_du_binaire>/resources/sounds. Calque i18n_localedir(), repli relatif
 * au cwd si /proc/self/exe est illisible (le fichier manquera → silence). */
static char *
sound_dir(void)
{
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe == NULL)
        return g_strdup("resources/sounds");
    gchar *dir = g_path_get_dirname(exe);
    gchar *out = g_build_filename(dir, "resources", "sounds", NULL);

    g_free(dir);
    g_free(exe);
    return out;
}

/* Le premier lecteur CLI trouvé dans le PATH. */
static const char *
pick_player(void)
{
    static const char *const cand[] = { "pw-play", "paplay", "ffplay" };

    for (guint i = 0; i < G_N_ELEMENTS(cand); i++) {
        char *p = g_find_program_in_path(cand[i]);

        if (p != NULL) {
            g_free(p);
            return cand[i];
        }
    }
    return NULL;
}

/* layout_pref_get ne renvoie que des chaînes ; NULL (clé absente) = dflt.
 * On n'écrit que "1"/"0", mais on tolère "true" par robustesse de saisie. */
static gboolean
pref_bool(const char *key, gboolean dflt)
{
    char    *v = layout_pref_get(key);
    gboolean r = dflt;

    if (v != NULL)
        r = (g_strcmp0(v, "1") == 0 || g_ascii_strcasecmp(v, "true") == 0);
    g_free(v);
    return r;
}

void
sfx_init(void)
{
    char *dir;

    if (inited)
        return;
    inited = TRUE;

    dir = sound_dir();
    path_long  = g_build_filename(dir, SND_LONG,  NULL);
    path_short = g_build_filename(dir, SND_SHORT, NULL);
    g_free(dir);

    player = pick_player();
    if (player != NULL)
        is_ffplay = (g_strcmp0(player, "ffplay") == 0);

    have_long  = (player != NULL &&
                  g_file_test(path_long,  G_FILE_TEST_IS_REGULAR));
    have_short = (player != NULL &&
                  g_file_test(path_short, G_FILE_TEST_IS_REGULAR));

    en_long  = pref_bool(PREF_LONG,  TRUE);
    en_short = pref_bool(PREF_SHORT, TRUE);
}

/* Rend le pid au système : sans ça, chaque ding laisserait un zombie
 * (g_spawn_async ne réapique PAS automatiquement quand on pose
 * G_SPAWN_DO_NOT_REAP_CHILD, qui est justement ce qui évite le blocage). */
static void
child_done(GPid pid, gint status, gpointer data)
{
    (void) status;
    (void) data;
    g_spawn_close_pid(pid);
}

static void
play(const char *path, gboolean have)
{
    const gchar *av[8];
    gchar      **argv = (gchar **) av;
    GPid         pid;
    GError      *e = NULL;
    int          i = 0;

    if (!have || player == NULL || path == NULL)
        return;

    av[i++] = player;
    if (is_ffplay) {               /* pas de fenêtre, sortie auto, discret */
        av[i++] = "-nodisp";
        av[i++] = "-autoexit";
        av[i++] = "-loglevel";
        av[i++] = "quiet";
    }
    av[i++] = path;
    av[i]   = NULL;

    if (g_spawn_async(NULL, argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                      NULL, NULL, &pid, &e))
        g_child_watch_add(pid, child_done, NULL);
    else if (e != NULL)
        g_error_free(e);           /* silence assumé : le son n'est pas vital */
}

void
sfx_play_turn_done(void)
{
    if (en_long)
        play(path_long, have_long);
}

void
sfx_play_feedback(void)
{
    gint64 now;

    if (!en_short)
        return;
    now = g_get_monotonic_time();
    if (last_short != 0 && now - last_short < FEEDBACK_THROTTLE_US)
        return;
    last_short = now;
    play(path_short, have_short);
}

void
sfx_preview_turn_done(void)
{
    play(path_long, have_long);
}

void
sfx_preview_feedback(void)
{
    play(path_short, have_short);
}

gboolean
sfx_enabled_turn_done(void)
{
    return en_long;
}

gboolean
sfx_enabled_feedback(void)
{
    return en_short;
}

void
sfx_set_enabled_turn_done(gboolean on)
{
    en_long = on;
    layout_pref_set(PREF_LONG, on ? "1" : "0");
}

void
sfx_set_enabled_feedback(gboolean on)
{
    en_short = on;
    layout_pref_set(PREF_SHORT, on ? "1" : "0");
}
