/*
 * llmbashrules.h : moteur pur des règles bash par motif (spec docs/bashrules-spec.md).
 *
 * Module SANS dépendance GTK, SANS disque, SANS JSON : glib seule, comme
 * src/textops.c. Le JSON (lecture/écriture/migration) vit dans
 * src/llmtoolpref.c (spec §6) ; l'UI vit dans src/main.c (spec §7).
 *
 * Vocabulaire (§3) : 7 valeurs longues, SANS "+" :
 *   off, ask, askplus, allow, allowplus, deny, mode.
 * "ask+"/"allow+" ne sont jamais acceptés. Valeur inconnue = ignorée
 * (spec §2) : llm_bash_rule_new() rend NULL et l'appelant journalise
 * (motif + valeur + profil, CDB_DEBUG=1, _()) — le moteur ne logue pas.
 *
 * Sémantique (§2 + 4.2) : motif pris au mot entre les deux `"`, seul
 * `*` est spécial (n'importe quoi, vide compris). TOUT autre caractère
 * (espace compris) est littéral. NOTA : on n'utilise PAS GPatternSpec
 * tel quel — sonde /tmp/pat.c (P0) : `?` y est un joker sans mécanisme
 * d'échappement, contraire à 4.2. Le matcher maison ci-dessous ne
 * connaît que `*`, ce qui rend 4.2 vrai par construction.
 *
 * Résolution (§2) : plus de littéraux gagne (len - nb('*')),
 * tie = premier déclaré. `"*"` vaut 0 littéral (perd contre tout).
 * (rend le mode du profil). Bypass 4.9 : sans règles → fallback,
 * sans découper ni matcher.
 *
 * Découpage (4.1) : coupent `;`, `&&`, `||`, `|`, `&` hors atome, `\n` ;
 * ne coupent pas `<`, `>`, `(`, `)`, `$` seul, backtick, `{`, `}` ;
 * atomes (jamais séparateurs) : `N>&M`, `N<&M`, `&>f`, `>>` — en
 * pratique on ne protège que le `&` de redirection, `>`/`<` ne coupant
 * jamais. Conscient des citations, trim double, vides ignorés.
 * Pire gagne : deny > off > askplus > ask > allowplus > allow ;
 * le `+` suit le segment gagnant (4.1, 4.8 : pas d'interaction tab).
 *
 * Le verdict LlmBashVerdict est VOLONTAIREMENT séparé de LlmToolMode
 * (src/llm.h:206) : ajouter `deny` à l'enum outil ferait tomber les deux
 * static_assert + le cycle main.c:4286. La conversion vers LlmToolMode
 * (et le refus off/deny via error + goto done, 4.7) appartient à
 * l'appelant (src/llmcore.c, §5).
 */

#ifndef CDB_LLMBASHRULES_H
#define CDB_LLMBASHRULES_H

#include <glib.h>

/* Verdict d'une règle. INVALID = valeur inconnue (ligne ignorée, §2) ;
 * MODE = variable naturelle `"mode"` (rend le mode du profil). */
typedef enum {
    LLM_BASH_OFF = 0,
    LLM_BASH_ASK,
    LLM_BASH_ASKPLUS,
    LLM_BASH_ALLOW,
    LLM_BASH_ALLOWPLUS,
    LLM_BASH_DENY,
    LLM_BASH_MODE,
    LLM_BASH_INVALID
} LlmBashVerdict;

/* Une règle compilée. `ordre` = rang de déclaration (tie → premier). */
typedef struct {
    char          *pattern;   /* motif brut, possédé */
    int            litteraux; /* strlen(pattern) - nb('*') */
    LlmBashVerdict verdict;
    guint          ordre;
} LlmBashRule;

/* "off".."mode" → verdict ; NULL/inconnu → LLM_BASH_INVALID. */
LlmBashVerdict llm_bash_verdict_parse(const char *s);

/* Nom statique pour tests/debug (clés hors gettext, jamais traduit). */
const char *llm_bash_verdict_name(LlmBashVerdict v);

/* Sévérité worst-of : DENY(5) > OFF(4) > ASKPLUS(3) > ASK(2) >
 * ALLOWPLUS(1) > ALLOW(0) ; MODE(-1), INVALID(-2). */
int llm_bash_severity(LlmBashVerdict v);

/* len - nb('*'), en octets (motifs shell ASCII). */
int llm_bash_litteraux(const char *pattern);

/* Match exact 4.2 : seul `*` est spécial. NULL → FALSE. */
gboolean llm_bash_match(const char *pattern, const char *segment);

/* Découpe 4.1 → GPtrArray de char* (g_free). Jamais NULL :
 * commande NULL/vide → tableau vide. Vides ignorés. */
GPtrArray *llm_bash_split(const char *command);

/* Construit une règle ; NULL si motif vide ou verdict inconnu
 * (ligne ignorée, §2 — l'appelant journalise). */
LlmBashRule *llm_bash_rule_new(const char *pattern,
                               const char *verdict_str,
                               guint ordre);
void llm_bash_rule_free(gpointer p);

/* Libère un tableau de LlmBashRule* (NULL accepté). */
void llm_bash_rules_free(GPtrArray *rules);

/* Meilleur verdict pour UN segment ; LLM_BASH_MODE si aucun match. */
LlmBashVerdict llm_bash_verdict_segment(GPtrArray *rules,
                                        const char *segment);

/* Verdict d'une commande : bypass si !rules ou vide ou commande
 * vide → fallback, sans découper (4.9, perf). Sinon pire des
 * segments, `"mode"` résolu vers fallback, `+` du gagnant. */
LlmBashVerdict llm_bash_verdict_command(GPtrArray *rules,
                                        const char *command,
                                        LlmBashVerdict fallback);

#endif /* CDB_LLMBASHRULES_H */
