/*
 * Layout : modèle récursif du tiling de tuiles (source de vérité), rendu
 * par un arbre de GtkPaned. L'utilisateur divise / retire des tuiles ; le
 * tout est persisté en JSON et restauré au démarrage.
 *
 * Modèle : LAYOUT_TILE (une pièce) | LAYOUT_HSPLIT / LAYOUT_VSPLIT (2 fils).
 * Le modèle ne porte AUCUN widget : une tuile est une vue sur un morceau
 * (editor/explorer) dont l'état vit dans App et survit aux retraits.
 */

#ifndef CDB_LAYOUT_H
#define CDB_LAYOUT_H

#include <glib.h>
#include <json-glib/json-glib.h>   /* JsonNode / JsonObject : prefs de layout.json */

typedef enum {
    LAYOUT_TILE,
    LAYOUT_HSPLIT,
    LAYOUT_VSPLIT
} LayoutKind;

typedef struct Layout Layout;

struct Layout {
    LayoutKind kind;
    char      *id;        /* tuile : id de pièce ("editor", "explorer", ...) */
    Layout    *a, *b;     /* splits */
    Layout    *parent;    /* pour les manipulations (split/remove) */
    double     fraction;  /* position de la poignée a/(a+b), persistée */
};

/* Crée une tuile. */
Layout *layout_tile(const char *id);

/* Libère récursivement. */
void layout_free(Layout *node);

/* ------------------------------------------------ */
/* Persistance (layout.json)                         */
/* ------------------------------------------------ */

/* Charge depuis ~/.config/cdb/layout.json ; si absent ou
 * invalide, renvoie le layout par défaut (explorateur | éditeur). */
Layout *layout_load(void);

/* Sauvegarde le modèle. */
void layout_save(Layout *root);

/* Préférences d'interface stockées dans layout.json, à côté de l'arbre :
 * langue choisie, état de la fenêtre. Get : chaîne à libérer, NULL si absente.
 * Set : value NULL retire la clé. Merge : fusionne des membres au sommet sans
 * toucher aux autres (ni à "root", ni à "language") ; `members` n'est pas
 * consommé. Get_node : COPIE à libérer par l'appelant (json_node_unref), NULL
 * si absent — le nœud prêté par le parser meurt avec lui, c'est pour cela que
 * cette fonction copie.
 * Règle du fichier à propriétaire unique : aucune de ces écritures ne détruit
 * ce qu'elle ne comprend pas. */
char     *layout_pref_get(const char *key);
void      layout_pref_set(const char *key, const char *value);
JsonNode *layout_pref_get_node(const char *key);
void      layout_merge_members(JsonObject *members);

/* ------------------------------------------------ */
/* Opérations (rendent le (nouveau) root)            */
/* ------------------------------------------------ */

/* Divise la tuile `node` en deux : la tuile existante + une nouvelle pièce
 * `new_id`, selon l'orientation. Renvoie le (nouveau) root. */
Layout *layout_split(Layout *root, Layout *node, gboolean horizontal,
                     const char *new_id);

/* Retire la tuile `node` (fusionne avec sa soeur). Refuse si c'est la
 * seule tuile. Renvoie le (nouveau) root. */
Layout *layout_remove(Layout *root, Layout *node);

/* Transforme la tuile `node` : elle garde sa place, change de pièce.
 * L'état de l'ancienne pièce (buffer/modèle) survit dans App. */
void layout_retile(Layout *node, const char *new_id);

/* Nom lisible d'une pièce (pour l'affichage). */
const char *layout_name(const char *id);

#endif /* CDB_LAYOUT_H */