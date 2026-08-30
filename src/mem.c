/*
 * mem.c : rendre au noyau ce que glibc garde.
 *
 * free() ne diminue PAS le tas. Mesuré sur ce poste : 80 Mo de petits blocs
 * (1 KiB, la taille d'un objet GObject), tous libérés, laissent 83 940 KiB
 * résidents — et un malloc_trim(0) les rend, redescendant à 756 KiB. Sans
 * cet appel, un pic de démarrage reste inscrit dans la mémoire de la session
 * entière alors que plus personne n'en est propriétaire : le bouton du
 * sélecteur de modèles realize des centaines de widgets, la liste d'un
 * gateway en 354 lignes allocate puis libère DOM, GList et clés.
 *
 * Le trim n'est pas un nettoyage et ne peut pas corriger une fuite : il ne
 * rend que des pages déjà libres. Un objet vivant n'est jamais concerné, ce
 * qui rend l'appel sûr à n'importe quel moment de la boucle.
 *
 * Coût mesuré aussi : 0,05 ms sur un tas de 100 Mo. C'est un brk qui
 * redescend, un appel système, pas une chasse octet par octet. On peut donc
 * le poser à chaque point de repos sans en payer le prix à l'écran.
 *
 * Pourquoi un fichier pour une ligne : malloc_trim n'est déclaré par
 * <malloc.h> que sous __USE_MISC, et le build est en -std=c23 — où une
 * déclaration implicite est une ERREUR, pas un avertissement. _GNU_SOURCE
 * reste ici, le core n'a pas à le porter pour un seul appel.
 */
#define _GNU_SOURCE
#include "mem.h"

#include <malloc.h>

void
cdb_mem_trim(void)
{
    malloc_trim(0);
}
