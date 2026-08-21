/*
 * session.c : sessions isolées par numéro (000-999) — voir session.h.
 *
 * Détection d'instance : scan de /proc, comparaison des cibles réelles
 * de /proc/<pid>/exe (readlink) avec /proc/self/exe. Le kernel est la
 * source de vérité : pas de lock disque, un PID mort disparaît seul.
 */

#define _POSIX_C_SOURCE 200809L
#include "session.h"
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

int sieb_session = 0;

/* ------------------------------------------------------------------ */
/* Détection d'instances                                              */
/* ------------------------------------------------------------------ */

/* Cible réelle (readlink) du symlink exe du processus pid. */
static char *
proc_exe(pid_t pid)
{
    char     link[64];
    char     buf[PATH_MAX];
    ssize_t  n;

    g_snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
    n = readlink(link, buf, sizeof(buf) - 1);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    return g_strdup(buf);
}

gboolean
session_other_instance_running(void)
{
    GDir       *dir;
    const char *name;
    char       *self_exe;
    gboolean    found = FALSE;

    self_exe = proc_exe(getpid());
    if (self_exe == NULL)
        return FALSE;

    dir = g_dir_open("/proc", 0, NULL);
    if (dir == NULL) {
        g_free(self_exe);
        return FALSE;
    }

    while ((name = g_dir_read_name(dir)) != NULL && !found) {
        pid_t  pid;
        char  *end;
        char  *exe;

        pid = (pid_t)strtol(name, &end, 10);
        if (*end != '\0' || pid <= 0 || pid == getpid())
            continue; /* pas un PID / nous-mêmes */
        exe = proc_exe(pid);
        if (exe == NULL)
            continue; /* process parti ou sans droits */
        found = g_strcmp0(exe, self_exe) == 0;
        g_free(exe);
    }
    g_dir_close(dir);
    g_free(self_exe);
    return found;
}

/* ------------------------------------------------------------------ */
/* Chemins                                                            */
/* ------------------------------------------------------------------ */

char *
session_dir(void)
{
    return g_build_filename(g_get_user_config_dir(), "cdb", NULL);
}

static char *
session_dir_n(int n)
{
    char *base = session_dir();
    char *out;
    char  num[8];

    g_snprintf(num, sizeof(num), "%03d", n);
    out = g_build_filename(base, num, NULL);
    g_free(base);
    return out;
}

char *
session_config_path(const char *path)
{
    char *dir = session_dir_n(sieb_session);

    if (g_path_is_absolute(path)) {
        /* Compat : certains appelants passent déjà un chemin complet
         * construit ailleurs — on le renvoie tel quel. */
        g_free(dir);
        return g_strdup(path);
    }
    {
        char *out = g_build_filename(dir, path, NULL);

        g_free(dir);
        return out;
    }
}

/* Plus petit numéro de session existant sur disque (-1 si aucun). */
static int
session_lowest_existing(void)
{
    char       *base = session_dir();
    GDir       *dir = g_dir_open(base, 0, NULL);
    const char *name;
    int         lowest = -1;

    g_free(base);
    if (dir == NULL)
        return -1;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *end;
        long  v;

        if (strlen(name) != 3)
            continue;
        v = strtol(name, &end, 10);
        if (*end != '\0' || v < SESSION_MIN || v > SESSION_MAX)
            continue;
        if (lowest < 0 || v < lowest)
            lowest = (int)v;
    }
    g_dir_close(dir);
    return lowest;
}

void
session_ensure(void)
{
    char *dir = session_dir_n(sieb_session);
    int   src;

    if (g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        g_free(dir);
        return;
    }

    src = session_lowest_existing();
    if (src >= 0 && src != sieb_session) {
        /* Copie du plus petit canal existant. */
        char *src_dir = session_dir_n(src);
        char *cmd;

        cmd = g_strdup_printf("cp -a %s %s", src_dir, dir);
        g_free(src_dir);
        int rc = system(cmd);
        (void)rc;
        g_free(cmd);
    } else {
        g_mkdir_with_parents(dir, 0755);
    }
    g_free(dir);
}

/* ------------------------------------------------------------------ */
/* Spawn                                                              */
/* ------------------------------------------------------------------ */

gboolean
session_spawn_new(int n)
{
    char     self[PATH_MAX];
    ssize_t  r;
    char     num[8];
    char    *argv[2];
    posix_spawnattr_t attr;
    pid_t    pid = -1;
    char    *env_set;
    char   **envp;
    gsize    envc = 0, i;

    r = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (r <= 0)
        return FALSE;
    self[r] = '\0';

    g_snprintf(num, sizeof(num), "%03d", n);
    env_set = g_strdup_printf("CDB_SESSION=%s", num);

    /* environ + notre variable. */
    while (environ[envc] != NULL)
        envc++;
    envp = g_new(char *, envc + 2);
    for (i = 0; i < envc; i++)
        envp[i] = environ[i];
    envp[envc] = env_set;
    envp[envc + 1] = NULL;

    argv[0] = self;
    argv[1] = NULL;
    posix_spawnattr_init(&attr);
    {
        int rc = posix_spawn(&pid, self, NULL, &attr, argv, envp);

        posix_spawnattr_destroy(&attr);
        g_free(envp);
        g_free(env_set);
        return rc == 0;
    }
}

/* ------------------------------------------------------------------ */
/* Dialogue de choix de session                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    GtkWindow *parent;
    GtkWindow *dialog;   /* fenêtre du dialogue (pour activate) */
    int        result;   /* -1 = annulé */
} SessionPickCtx;

static void
on_pick_cancel(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    SessionPickCtx *ctx = data;

    ctx->result = -1;
    gtk_window_destroy(GTK_WINDOW(gtk_widget_get_ancestor(GTK_WIDGET(b),
                                                          GTK_TYPE_WINDOW)));
}

static void
on_pick_ok(GtkButton G_GNUC_UNUSED *b, gpointer data)
{
    SessionPickCtx *ctx = data;
    GtkWidget      *win = GTK_WIDGET(ctx->dialog);
    GtkWidget      *entry = g_object_get_data(G_OBJECT(win), "entry");
    const char     *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
    char           *end;
    long            v;

    v = strtol(txt, &end, 10);
    if (*end != '\0' || txt[0] == '\0'
        || v < SESSION_MIN || v > SESSION_MAX) {
        ctx->result = -1;
    } else {
        ctx->result = (int)v;
    }
    gtk_window_destroy(GTK_WINDOW(win));
}

static void
on_entry_activate(GtkEntry G_GNUC_UNUSED *e, gpointer data)
{
    on_pick_ok(NULL, data);
}

int
session_pick_dialog(GtkWindow *parent)
{
    SessionPickCtx  ctx = { parent, NULL, -1 };
    GtkWidget      *win;
    GtkWidget      *box;
    GtkWidget      *lbl;
    GtkWidget      *entry;
    GtkWidget      *row;
    GtkWidget      *cancel;
    GtkWidget      *ok;
    GMainLoop      *loop;

    win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Nouvelle session");
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 320, -1);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    lbl = gtk_label_new("Numéro de session (000-999) :");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_append(GTK_BOX(box), lbl);

    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "000");
    gtk_editable_set_text(GTK_EDITABLE(entry), "000");
    gtk_box_append(GTK_BOX(box), entry);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("Annuler");
    ok = gtk_button_new_with_label("Ouvrir");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_pick_cancel), &ctx);
    g_signal_connect(ok, "clicked", G_CALLBACK(on_pick_ok), &ctx);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), &ctx);
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(row), ok);
    gtk_box_append(GTK_BOX(box), row);

    g_object_set_data(G_OBJECT(win), "entry", entry);
    ctx.dialog = GTK_WINDOW(win);

    /* Boucle modale locale : la fenêtre principale attend le choix. */
    loop = g_main_loop_new(NULL, FALSE);
    g_signal_connect_swapped(win, "destroy", G_CALLBACK(g_main_loop_quit),
                             loop);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(entry);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return ctx.result;
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

gboolean
session_init(void)
{
    const char *env = g_getenv("CDB_SESSION");

    if (env != NULL && env[0] != '\0') {
        char *end;
        long  v = strtol(env, &end, 10);

        if (*end == '\0' && v >= SESSION_MIN && v <= SESSION_MAX) {
            sieb_session = (int)v;
            session_ensure();
            return TRUE;
        }
        /* Valeur invalide : on retombe sur la logique standard. */
    }

    if (!session_other_instance_running()) {
        sieb_session = 0;
        session_ensure();
        return TRUE;
    }

    /* Instances existantes : demander le numéro. */
    {
        int picked = session_pick_dialog(NULL);

        if (picked < 0)
            return FALSE; /* annulé */
        sieb_session = picked;
        session_ensure();
        return TRUE;
    }
}
