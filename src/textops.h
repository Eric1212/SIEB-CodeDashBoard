/*
 * textops.h — couche pure des outils fichiers : adressage par lignes,
 * hash de synchronisation, politique de terminateur.
 *
 * Dependances : glib seul. Pas de GTK, pas de LlmCore, pas de disque, pas
 * de gettext. Tout ce qui est ici est deterministe et testable hors de
 * CDB (voir tools/test_textops.c, cible `make check`).
 *
 * Pourquoi ce module existe : la totalite des erreurs de cdb_replace
 * habite cette couche — le compte des lignes, le \n de terminaison qui
 * appartient ou non a la zone, la fusion avec la ligne suivante. Ces
 * regles etaient recopiees dans chaque outil et ne sont demostrables
 * que separement de l'appel reseau qui les déclenche.
 */
#ifndef CDB_TEXTOPS_H
#define CDB_TEXTOPS_H

#include <glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Adressage des lignes                                                */
/* ------------------------------------------------------------------ */

/* Regle unique, celle de toute la couche :
 *   off[k] = offset du debut de la (k+1)-ieme ligne ; off[line_count] =
 *   fin de la derniere ligne. Une ligne INCLUT son \n de terminaison,
 *   sauf la derniere si le fichier ne finit pas par \n.
 * len == 0 (fichier vide) => line_count == 0 et off a un seul element.
 * Le tableau fait toujours line_count + 1 elements. */
void    textops_line_offsets(const char *content, gsize len, GArray *off,
                             guint *line_count);

/* Numero (1-based) de la ligne contenant un offset. */
guint   textops_line_at(GArray *off, guint line_count, gsize pos);

/* Nombre de lignes logiques d'un bloc, meme regle que ci-dessus. */
guint   textops_logical_lines(const char *t, gsize len);

/* ------------------------------------------------------------------ */
/* Hash de synchronisation                                             */
/* ------------------------------------------------------------------ */

/* Hash court d'une plage : 4 caracteres base36 (36^4 = 1 679 616).
 * Garde-fou de synchronisation, pas une signature. Chaine allouee, a
 * liberer par l'appelant. L'algorithme est gele : les hashes rendus au
 * modele dans les conversations reprises de llm_live.json doivent rester
 * comparables d'une version a l'autre. */
char   *textops_hash4(const void *buf, gsize len);

/* ------------------------------------------------------------------ */
/* Style de fin de ligne                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *eol;       /* "\n" ou "\r\n" — never NULL */
    gsize       eol_len;   /* 1 ou 2 */
    gboolean    final_nl;  /* la derniere ligne du fichier est terminee */
} TextopsEol;

/* Determine le style du fichier. CRLF n'est retenu que si CHAQUE \n est
 * precede d'un \r : un fichier mele renvoie LF, et le mele est visible
 * ensuite a la jointure plutot que silencieusement normalise.
 * len == 0 : LF, final_nl FALSE (rien a preserver ; un fichier vide n'a
 * pas de convention, il n'a pas a en inventer une). */
void    textops_scan_eol(const char *content, gsize len, TextopsEol *out);

/* ------------------------------------------------------------------ */
/* Blocs de lignes entieres                                            */
/* ------------------------------------------------------------------ */

/* Zone octet du bloc from..to (1-based, inclus), TERMINATION COMPRISE
 * quand la ligne to en a une — c'est exactement la zone couverte par le
 * hash rendu par un read(from, to).
 * Renvoie FALSE et ne touche a rien si les bornes sont invalides
 * (from < 1, to < from, to > line_count, fichier vide). */
gboolean textops_block_range(gsize len, GArray *off, guint line_count,
                             guint from, guint to,
                             gsize *start, gsize *end);

/* Diagnostic de jointure. bad_line = 0 quand tout est bon, sinon numero
 * (1-based) de l'element fautif. */
typedef struct {
    guint bad_line;
    char  bad_char;   /* '\n' ou '\r' trouve dans l'element */
} TextopsJoinErr;

/* lines[] (SANS terminateurs) -> octets du bloc.
 *
 * at_eof = TRUE quand le bloc se termine a la derniere ligne du fichier :
 * la derniere ligne recoit un terminateur seulement si le fichier en
 * avait un. C'est la que vit la politique : ni creation ni perte du \n
 * final, et le modele n'a pas a y penser.
 *
 * Un element contenant \n ou \r est refuse : un element = une ligne.
 * Retourne NULL et remplit err (si non NULL) dans ce cas. n == 0 rend
 * une chaine vide de longueur 0. */
gchar  *textops_join_block(const char *const *lines, guint n,
                           const TextopsEol *eol, gboolean at_eof,
                           gsize *out_len, TextopsJoinErr *err);

/* Octets -> lignes SANS terminateurs. GPtrArray de gchar*, propriete de
 * l'appelant (g_ptr_array_free + g_free de chaque element, ou
 * g_ptr_array_new_with_free_func(g_free)). */
void    textops_split_block(const char *bytes, gsize len, GPtrArray *out);

G_END_DECLS

#endif /* CDB_TEXTOPS_H */
