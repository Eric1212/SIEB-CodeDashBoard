/*
 * i18n.c : initialisation de la traduction au démarrage.
 *
 * Le répertoire des catalogues est déduit de l'emplacement du binaire
 * (/proc/self/exe) : <dir_du_binaire>/po/locale. Ainsi `make run` depuis la
 * racine du projet charge ./po/locale/<lang>/LC_MESSAGES/cdb.mo sans aucune
 * installation. Si le dossier est absent, gettext retombe silencieusement
 * sur les msgid (anglais pivot) — aucun crash possible.
 */
#include "i18n.h"

#include <locale.h>
#include <libintl.h>
#include <glib.h>
#include <glib/gstdio.h>

/* Construit <dir_du_binaire>/po/locale. Chaîne statique (appel unique). */
static const char *
i18n_localedir(void)
{
    static gchar  localedir[4096];
    static gboolean resolved = FALSE;

    if (resolved)
        return localedir;
    resolved = TRUE;

    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe == NULL)
        return "po/locale";   /* repli relatif au cwd si lecture impossible */

    gchar *dir = g_path_get_dirname(exe);
    g_snprintf(localedir, sizeof localedir, "%s/po/locale", dir);

    g_free(dir);
    g_free(exe);
    return localedir;
}

void
i18n_init(void)
{
    /* Locale = celle de l'environnement (LANG / LC_*). */
    setlocale(LC_ALL, "");

    /* Domaine "cdb" : catalogues dans <dir_binaire>/po/locale. */
    bindtextdomain(GETTEXT_PACKAGE, i18n_localedir());
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
}
