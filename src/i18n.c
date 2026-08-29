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
#include <string.h>
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

/* ------------------------------------------------ */
/* Langues disponibles et changement de langue        */
/* ------------------------------------------------ */

/* Endonymes : chaque langue s'appelle comme elle s'appelle ELLE-MÊME, et ces
 * chaînes ne passent volontairement par AUCUN _(). C'est un principe de
 * secours pour l'utilisateur final : un francophone qui hérite d'une interface
 * anglaise doit retrouver « English », et un hispanophone « Español », sans
 * avoir à lire une langue qu'il ne parle pas. Traduire ces noms les rendrait
 * inutiles précisément dans le cas où ils servent.
 * Un code absent de cette table s'affiche tel quel : jamais de devinette. */
static const struct {
    const char *code;
    const char *name;
} LANG_NAMES[] = {
    { "ca", "Català"      }, { "cs", "Čeština"     },
    { "da", "Dansk"       }, { "de", "Deutsch"     },
    { "el", "Ελληνικά"    }, { "en", "English"     },
    { "es", "Español"     }, { "fi", "Suomi"       },
    { "fr", "Français"    }, { "hu", "Magyar"      },
    { "it", "Italiano"    }, { "ja", "日本語"       },
    { "ko", "한국어"       }, { "nl", "Nederlands"  },
    { "no", "Norsk"       }, { "pl", "Polski"      },
    { "pt", "Português"   }, { "ru", "Русский"     },
    { "sv", "Svenska"     }, { "tr", "Türkçe"      },
    { "uk", "Українська"  }, { "zh", "中文"         },
};

const char *
i18n_language_name(const char *code)
{
    gsize i;

    if (code == NULL || code[0] == '\0')
        return "";
    for (i = 0; i < G_N_ELEMENTS(LANG_NAMES); i++)
        if (g_strcmp0(LANG_NAMES[i].code, code) == 0)
            return LANG_NAMES[i].name;
    return code;
}

/* Codes des catalogues RÉELLEMENT présents à côté du binaire — c'est le
 * catalogue qui fait la langue, pas une liste compilée : déposer fr.po et
 * faire `make mo` ajoute « Français » au sélecteur sans toucher au code.
 * Renvoie une GList triée de chaînes à libérer (g_list_free_full(l, g_free)). */
GList *
i18n_languages(void)
{
    const char *base = i18n_localedir();
    GDir       *d;
    GList      *out = NULL;
    const char *entry;

    d = g_dir_open(base, 0, NULL);
    if (d == NULL)
        return NULL;            /* aucun catalogue : l'anglais pivot suffit */
    while ((entry = g_dir_read_name(d)) != NULL) {
        char *mo = g_build_filename(base, entry, "LC_MESSAGES",
                                    GETTEXT_PACKAGE ".mo", NULL);

        if (g_file_test(mo, G_FILE_TEST_IS_REGULAR))
            out = g_list_prepend(out, g_strdup(entry));
        g_free(mo);
    }
    g_dir_close(d);
    return g_list_sort(out, (GCompareFunc)g_strcmp0);
}

/* Applique une langue ; code NULL ou vide = celle de l'environnement.
 * Renvoie la locale installée (chaîne statique de la libc : à copier si on la
 * garde), NULL si AUCUNE locale du système ne porte cette langue — auquel cas
 * rien n'a changé, et l'appelant doit le dire au lieu de faire semblant.
 *
 * Pourquoi un escalier de candidats et pas setlocale(code) : la libc ne
 * connaît ni « en » ni « fr » (vérifié sur ce poste, les deux échouent), elle
 * n'accepte que des noms complets « langue_TERRITOIRE.CODAGE ». Le catalogue,
 * lui, est cherché par langue seule (libintl coupe le territoire — Jalon B,
 * vérifié par strace). Il faut donc une locale RÉELLE dont la LANGUE est celle
 * demandée.
 *
 * L'ordre est un choix délibéré, pas une liste de chance : le TERRITOIRE de
 * l'utilisateur passe avant les territoires par défaut du langage. Ce poste est
 * en fr_CA.UTF-8 ; quelqu'un qui y choisit « English » doit se retrouver en
 * anglais CANADIEN (ses dates, ses devises, son heure), pas à Washington. D'où,
 * dans l'ordre : la locale complète de l'environnement si elle est déjà de la
 * langue demandée, puis même territoire avec la langue demandée, puis les
 * territoires courants de cette langue, puis la forme nue. */
const char *
i18n_apply(const char *code)
{
    static const char *TERRS[] = { "US", "GB", "CA", "AU", "FR",
                                   "BE", "CH", "DE" };
    const char *env, *got = NULL;
    char        terr[16] = "";
    int         i;

    if (code == NULL || code[0] == '\0')
        return setlocale(LC_ALL, "");

    env = g_getenv("LC_ALL");
    if (env == NULL || env[0] == '\0')
        env = g_getenv("LANG");

      /* Le territoire de l'utilisateur, QUELQUE SOIT le langage demandé :
     * c'est lui qu'on veut conserver en changeant de langue. (Le lire
     * seulement quand la langue correspond déjà serait rater précisément le
       * cas qui intéresse : fr_CA -> anglais.) */
    if (env != NULL) {
        const char *u = strchr(env, '_');

        if (u != NULL) {
            const char *end = strchr(u + 1, '.');
            size_t      len = end ? (size_t)(end - u - 1) : strlen(u + 1);

            if (len > 0 && len < sizeof terr) {
                memcpy(terr, u + 1, len);
                terr[len] = '\0';
            }
        }
    }

    /* 1. Environnement déjà dans la bonne langue : rien ne bouge. */
    if (env != NULL && env[0] != '\0' &&
        g_str_has_prefix(env, code) && env[strlen(code)] == '_')
        got = setlocale(LC_ALL, env);

    /* 2. Même territoire, langue demandée. */
    for (i = 0; got == NULL && terr[0] != '\0' && i < 2; i++) {
        char *c = g_strdup_printf("%s_%s%s", code, terr,
                                  i == 0 ? ".UTF-8" : "");

        got = setlocale(LC_ALL, c);
        g_free(c);
    }
    /* 3. Territoires courants de cette langue. */
    for (i = 0; got == NULL && i < (int)G_N_ELEMENTS(TERRS); i++) {
        char *c = g_strdup_printf("%s_%s.UTF-8", code, TERRS[i]);

        got = setlocale(LC_ALL, c);
        g_free(c);
    }
    /* 4. Forme nue, dernier recours. */
    if (got == NULL)
        got = setlocale(LC_ALL, code);

    return got;
}

/* Langue réellement en vigueur, lue dans la locale INSTALLÉE et non dans la
 * variable d'environnement : i18n_apply() a pu trancher autrement, et c'est la
 * locale qui décide du catalogue. « fr_CA.UTF-8 » -> « fr », « C » -> « C ».
 * Tampon statique : une seule langue à la fois, l'appelant n'a rien à copier. */
const char *
i18n_current_language(void)
{
    static char lang[16];
    const char *loc = setlocale(LC_ALL, NULL);
    size_t      n   = 0;

    if (loc == NULL)
        return "C";
    while (loc[n] != '\0' && loc[n] != '_' && loc[n] != '.' && loc[n] != '@' &&
           n + 1 < sizeof lang) {
        lang[n] = loc[n];
        n++;
    }
    lang[n] = '\0';
    return lang;
}
