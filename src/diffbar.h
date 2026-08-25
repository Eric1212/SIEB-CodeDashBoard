/*
 * DiffBar : barre transparente sur la scrollbar montrant les changements
 * dirty du fichier courant (carte de tout le fichier, comme VS Code).
 *
 * Par ligne, on distingue trois cas (diff au niveau caractère) :
 *  - vert  : contenu AJOUTÉ (l'ancienne ligne est une sous-séquence de la
 *            nouvelle)      -> ex: « Je suis lion. » -> « Je suis UN lion. »
 *  - rouge : contenu RETIRÉ (la nouvelle ligne est une sous-séquence de
 *            l'ancienne)    -> ex: « Je suis lion. » -> « Je lion. »
 *  - orange: ligne REMPLACÉE (ni l'un ni l'autre)
 *                       -> ex: « Je suis lion. » -> « J'aime les lions. »
 */

#ifndef CDB_DIFFBAR_H
#define CDB_DIFFBAR_H

#include <gtk/gtk.h>

#define CDB_TYPE_DIFF_BAR (cdb_diff_bar_get_type())
G_DECLARE_FINAL_TYPE(CdbDiffBar, cdb_diff_bar, CDB, DIFF_BAR, GtkWidget)

/* Types de segment. */
enum {
    DIFF_ADD = 0, /* vert   */
    DIFF_DEL = 1, /* rouge  */
    DIFF_MOD = 2  /* orange */
};

/* Un segment coloré de la barre. pos/size en fractions de la hauteur
 * totale (0..1), donc indépendant du rendu et du scroll. */
typedef struct {
    double  pos;   /* début (fraction de la hauteur) */
    double  size;  /* hauteur (fraction de la hauteur) */
    gint    type;  /* DIFF_ADD / DIFF_DEL / DIFF_MOD */
} SiebdDiffRange;

/* Crée la barre (largeur ~10 px, fond transparent, n'intercepte pas les
 * clics -> la scrollbar reste utilisable). */
GtkWidget *cdb_diff_bar_new(void);

/* Remplace les segments affichés. total_lines = nombre de lignes du buffer
 * courant ; sert de dénominateur pour positionner les segments. */
void cdb_diff_bar_set_ranges(CdbDiffBar *bar, GPtrArray *ranges,
                               guint total_lines);

/* Calcule les marqueurs de diff entre la référence (saved) et le buffer
 * (current). Remplit out (GPtrArray de SiebdDiffRange* alloués via
 * g_malloc, à libérer) et définit *total_lines au nombre de lignes de
 * current. */
void cdb_diff_compute(const char *saved, const char *current,
                        GPtrArray *out, guint *total_lines);

#endif /* CDB_DIFFBAR_H */