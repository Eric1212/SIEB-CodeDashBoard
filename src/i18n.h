/*
 * i18n.h : internationalisation de CDB via gettext.
 *
 * <glib/gi18n.h> fournit les macros de marquage et n'exige aucun symbole :
 *   _()    traduction simple      -> gettext(String), domaine courant
 *   N_()   marquage seul          -> (String) ; pour les chaînes stockées
 *            (tableaux statiques, libellés résolus plus tard)
 *   C_()   traduction à contexte
 * ngettext() (pluriels) vient de <libintl.h>, inclus par gi18n.h.
 *
 * GETTEXT_PACKAGE est le nom de notre domaine — celui que i18n_init() lie
 * par bindtextdomain()/textdomain() et le nom du catalogue, cdb.mo. On le
 * définit avant l'inclusion par convention : seul <glib/gi18n-lib.h>,
 * réservé aux bibliothèques, impose réellement ce symbole.
 *
 * L'initialisation (setlocale + bindtextdomain + textdomain) est faite par
 * i18n_init(), à appeler en tout début de main(), avant toute création de
 * widget GTK.
 *
 * Règles de marquage : voir docs/I18N_PLAN.md §6 ; sort des diagnostics : §5.
 */
#ifndef CDB_I18N_H
#define CDB_I18N_H

#define GETTEXT_PACKAGE "cdb"
#include <glib/gi18n.h>

/* Initialise la locale système et le domaine de traduction.
 * LOCALEDIR est résolu à l'exécution, relatif au binaire : en développement
 * les catalogues compilés vivent dans <dir_du_binaire>/po/locale, ce qui
 * permet à `make run` de fonctionner sans rien installer sur le système. */
void i18n_init(void);

/* Langues : le sélecteur propose les CATALOGUES présents à côté du binaire
 * (pas une liste compilée), chaque langue s'affiche sous son propre nom
 * (endonyme, jamais traduit — cf. i18n.c) et i18n_apply() change la locale
 * en cours de session. Code NULL ou vide = langue de l'environnement. */
GList      *i18n_languages(void);        /* codes triés, à libérer (g_free)  */
const char *i18n_language_name(const char *code);
const char *i18n_apply(const char *code);

/* Deux façons de lire « la langue », volontairement séparées :
 *   i18n_environment_language() : ce que LC_ALL/LANG DEMANDE — ce que
 *     l'utilisateur veut lire, et ce que le sélecteur compare aux catalogues ;
 *   i18n_current_language() : ce que la libc a INSTALLÉ — sert à voir qu'une
 *     locale inconnue est retombée en « C » et que les formats sont à réparer.
 * « fr_CA.UTF-8 » -> « fr » dans les deux cas. Tampons statiques. */
const char *i18n_environment_language(void);
const char *i18n_current_language(void);

#endif /* CDB_I18N_H */
