/*
 * i18n.h : internationalisation de CDB via gettext.
 *
 * Définit GETTEXT_PACKAGE avant d'inclure <glib/gi18n.h> (qui exige ce
 * symbole), puis expose les macros de marquage :
 *   _()       traduction simple
 *   N_()      marquage sans traduction (pour extraction, traduit plus tard)
 *   ngettext  formes plurielles
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
