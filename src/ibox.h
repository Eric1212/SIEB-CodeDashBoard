/*
 * ibox.h : la BOÎTE INTERACTIVE — trois zones, trois temps d'une demande.
 *
 *   1. input   ce qui est demandé   — texte NOIR sur GRIS PÂLE, pliable
 *   2. choix   la décision          — TOUJOURS affichée EN ENTIER :
 *                                     grise pâle avec les deux options
 *                                     tant que rien n'est choisi, puis
 *                                     fond VERT (oui) ou ROUGE (non)
 *   3. output  ce qui est revenu    — texte BLANC sur NOIR, pliable
 *
 * Les couleurs sont VOLONTAIREMENT absolues (pas @view_bg_color, pas de
 * variante sombre) : la boîte est un instrument, elle doit se lire d'un
 * coup d'oeil au milieu du fil, en thème clair comme en thème sombre.
 *
 * ---------------------------------------------------------------------------
 * POURQUOI DES GtkTextView ET PAS DES GtkLabel (mesuré, pas intuitionné)
 *
 * Le fil de CDB est un GtkTextView : il est PARESSEUX, il n'engage que les
 * lignes visibles. 100 000 lignes y coutent 0,0 ms de mesure. Une GtkLabel,
 * elle, layoute TOUT ce qu'on lui confie — et super-linéairement :
 *
 *     GtkTextView  1 000/10 000/100 000 lignes : 0,0 / 0,0 / 0,0 ms
 *     GtkLabel     1 000 lignes wrap=non       :   215 ms
 *     GtkLabel    10 000 lignes wrap=non       : 15 829 ms   (15 s !)
 *     GtkLabel    10 000 lignes wrap=oui       : 62 303 ms   (1 min !)
 *     GtkLabel   100 000 lignes               : jamais revenu (timeout)
 *
 * Une GtkLabel dans la boîte, c'etait donc une minute de gel a chaque gros
 * resultats de bash. Les zones input et output sont des GtkTextView
 * non éditables (selectionnable, monospace, wrap=NONE comme un terminal)
 * dans un ScrolledWindow a hauteur calculée. Consequence heureuse :
 * AUCUNE TRONCATURE N'EST NECESSAIRE. Le texte integral est dans la boîte,
 * affiche integralement, et le defilement interne fait le reste.
 *
 * « Diminuer / agrandir » : le chevron plie la zone (0 px) ou la déplie a
 * une hauteur calculee sur le nombre de lignes, plafonnee (au-dela du
 * plafond, la zone defile sur elle-meme). La zone choix, elle, ne se plie
 * jamais et ne change jamais de hauteur.
 * ---------------------------------------------------------------------------
 *
 * La boîte ne connait NI les outils, NI le LLM, NI CDB : du texte entre,
 * un choix sort. Qui la pose (la tuile, dans le fil) et ce que signifient
 * ses zones reste ailleurs.
 */

#ifndef CDB_IBOX_H
#define CDB_IBOX_H

#include <gtk/gtk.h>

typedef enum {
    IB_CHOICE_NONE = 0,   /* gris pâle, les deux options sont offertes */
    IB_CHOICE_YES,        /* barre verte, figée */
    IB_CHOICE_NO          /* barre rouge, figée */
} IboxChoice;

/* Clic sur une option. JAMAIS appelé quand la décision vient d'ailleurs :
 * ibox_set_choice ne rejoue aucun callback — sinon le miroir multi-vues
 * ferait décider chaque vue par les clics des autres, en boucle. */
typedef void (*IboxChosen)(GtkWidget *box, IboxChoice choice,
                           gpointer user_data);

/* « ⤢ tout voir » : full_text est la copie possédée par la boîte (ne pas
 * libérer, ne pas modifier). Déclenché seulement quand le contenu dépasse
 * la hauteur affichée — l'appelant ouvre alors une modale (vue scrollable,
 * donc paresseuse elle aussi). */
typedef void (*IboxShowAll)(GtkWidget *box, const char *full_text,
                            gpointer user_data);

GtkWidget *ibox_new(void);

/* Callbacks. L'appelant ne possède rien. */
void ibox_on_choice(GtkWidget *box, IboxChosen cb, gpointer user_data);
void ibox_on_show_all(GtkWidget *box, IboxShowAll cb, gpointer user_data);

/* Contenu. set_output(NULL ou "") retire la zone output : une demande
 * n'a pas encore de réponse, et une boîte sans output ne doit pas étaler
 * une bande vide. */
void        ibox_set_input(GtkWidget *box, const char *text);
const char *ibox_get_input(GtkWidget *box);
void        ibox_set_output(GtkWidget *box, const char *text);
const char *ibox_get_output(GtkWidget *box);

/* Tranche la barre de choix. Idempotent ; IB_CHOICE_NONE ne « dé-décide »
 * pas : une fois tranchée, une boîte est tranchée (l'état résolu reste dans
 * le fil).
 *
 *   animate = TRUE  → le gagnant mange le perdant sur toute la barre, sous
 *     les yeux d'Éric. C'est le cas d'une décision prise à l'écran.
 *   animate = FALSE → état final posé direct. C'est le cas d'un ALLOW : la
 *     demande était acceptée d'avance, l'animer prétendrait qu'elle se
 *     décide maintenant. */
void       ibox_set_choice(GtkWidget *box, IboxChoice choice,
                           gboolean animate);
IboxChoice ibox_get_choice(GtkWidget *box);

/* Libellé de la barre résolue. À poser AVANT ibox_set_choice().
 * Par défaut la boîte dit « ✔ exécuté » ou « ✖ refusé » — ce qui décrit
 * un clic d'Éric. Une demande ACCEPTÉE D'AVANCE (outil en ALLOW ou
 * ALLOW+) n'a été cliquée par personne : la tuile y met « autorisé ».
 * NULL ou "" revient au libellé par défaut. */
void ibox_set_choice_label(GtkWidget *box, const char *text);
/* Pliage des zones input/output (jamais la zone choix). */
void ibox_set_expanded(GtkWidget *box, gboolean input_open,
                       gboolean output_open);
void ibox_get_expanded(GtkWidget *box, gboolean *input_open,
                       gboolean *output_open);

/* Décision PRISE — n'importe laquelle : clic d'Éric, refus, annulation, ou
 * accord d'avance (ALLOW / ALLOW+). La boîte se replie sur sa zone choix
 * (qui, elle, reste toujours affichée en entier) et le pliage est épinglé :
 * un résultat qui arrive ensuite ne la rouvrira plus.
 *
 * Demande en attente  → dépliée : il faut lire ce qu'on approuve.
 * Décision tombée     → repliée : la demande est de l'historique, plus de
 *                      l'actualité du fil. */
void ibox_decided(GtkWidget *box);

#endif /* CDB_IBOX_H */
