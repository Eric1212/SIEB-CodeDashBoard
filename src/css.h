#ifndef CDB_CSS_H
#define CDB_CSS_H

#include <gtk/gtk.h>

/* Installe la CSS applicative de CDB sur le display donné.
 * Si display est NULL, utilise le display par défaut. */
void cdb_css_install(GdkDisplay *display);

#endif /* CDB_CSS_H */
