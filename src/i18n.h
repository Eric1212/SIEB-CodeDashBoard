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
 * Règles de marquage : voir docs/I18N_PLAN.md §5.
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

#endif /* CDB_I18N_H */
