/*
 * llmbashrules.c : moteur pur des règles bash par motif.
 * Voir llmbashrules.h pour le contrat (spec docs/bashrules-spec.md).
 *
 * glib seule. Pas de GTK, pas de disque, pas de JSON, pas de log :
 * une valeur inconnue rend NULL (l'appelant journalise, §2 + 4.10).
 */

#include "llmbashrules.h"

#include <string.h>

LlmBashVerdict
llm_bash_verdict_parse(const char *s)
{
    if (s == NULL)
        return LLM_BASH_INVALID;
    if (g_strcmp0(s, "off") == 0)
        return LLM_BASH_OFF;
    if (g_strcmp0(s, "ask") == 0)
        return LLM_BASH_ASK;
    if (g_strcmp0(s, "askplus") == 0)
        return LLM_BASH_ASKPLUS;
    if (g_strcmp0(s, "allow") == 0)
        return LLM_BASH_ALLOW;
    if (g_strcmp0(s, "allowplus") == 0)
        return LLM_BASH_ALLOWPLUS;
    if (g_strcmp0(s, "deny") == 0)
        return LLM_BASH_DENY;
    if (g_strcmp0(s, "mode") == 0)
        return LLM_BASH_MODE;
    /* "ask+", "allow+", "profilsettings", "Ask", "denyy" : inconnus
     * (§2, §3) — jamais de retombée silencieuse. */
    return LLM_BASH_INVALID;
}

const char *
llm_bash_verdict_name(LlmBashVerdict v)
{
    switch (v) {
    case LLM_BASH_OFF:
        return "off";
    case LLM_BASH_ASK:
        return "ask";
    case LLM_BASH_ASKPLUS:
        return "askplus";
    case LLM_BASH_ALLOW:
        return "allow";
    case LLM_BASH_ALLOWPLUS:
        return "allowplus";
    case LLM_BASH_DENY:
        return "deny";
    case LLM_BASH_MODE:
        return "mode";
    default:
        return "invalid";
    }
}

int
llm_bash_severity(LlmBashVerdict v)
{
    switch (v) {
    case LLM_BASH_DENY:
        return 5;
    case LLM_BASH_OFF:
        return 4;
    case LLM_BASH_ASKPLUS:
        return 3;
    case LLM_BASH_ASK:
        return 2;
    case LLM_BASH_ALLOWPLUS:
        return 1;
    case LLM_BASH_ALLOW:
        return 0;
    case LLM_BASH_MODE:
        return -1;
    default:
        return -2;
    }
}

int
llm_bash_litteraux(const char *pattern)
{
    int n = 0;

    if (pattern == NULL)
        return 0;
    for (const char *p = pattern; *p != '\0'; p++)
        if (*p != '*')
            n++;
    return n;
}

/* Match 4.2 : seul `*` est spécial (n'importe quoi, vide compris).
 * Classique deux-pointeurs avec reprise sur la dernière étoile.
 * `?`, `[`, `{`, `\` sont littéraux — pas de GPatternSpec, qui leur
 * donnerait un sens (sonde P0 /tmp/pat.c). */
gboolean
llm_bash_match(const char *pattern, const char *segment)
{
    const char *p;
    const char *s;
    const char *star = NULL;
    const char *ss = NULL;

    if (pattern == NULL || segment == NULL)
        return FALSE;
    p = pattern;
    s = segment;
    while (*s != '\0') {
        if (*p == '*') {
            star = p++;
            ss = s;
            continue;
        }
        if (*p == *s) {
            p++;
            s++;
            continue;
        }
        if (star != NULL) {
            p = star + 1;
            s = ++ss;
            continue;
        }
        return FALSE;
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

/* Termine le segment courant : trim double, vide ignoré (4.1). */
static void
split_cut(GPtrArray *out, GString *cur)
{
    g_strstrip(cur->str);
    if (cur->str[0] != '\0')
        g_ptr_array_add(out, g_strdup(cur->str));
    g_string_truncate(cur, 0);
}

GPtrArray *
llm_bash_split(const char *command)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GString   *cur;
    gsize      n;
    gsize      i;
    char       q = '\0'; /* citation en cours : '\'' ou '"' */

    if (command == NULL || command[0] == '\0')
        return out;
    cur = g_string_new(NULL);
    n = strlen(command);
    for (i = 0; i < n; i++) {
        char c = command[i];

        if (q != '\0') {
            g_string_append_c(cur, c);
            if (c == '\\' && q == '"' && i + 1 < n) {
                /* Dans "...", `\x` vaut x : on garde les deux. */
                i++;
                g_string_append_c(cur, command[i]);
            } else if (c == q) {
                q = '\0';
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            g_string_append_c(cur, c);
            continue;
        }
        if (c == '\\' && i + 1 < n) {
            /* Hors citations, `\x` vaut x : on garde les deux,
             * au mot près (4.2) — le motif voit le même texte. */
            g_string_append_c(cur, c);
            i++;
            g_string_append_c(cur, command[i]);
            continue;
        }
        if (c == ';' || c == '\n' || c == '|') {
            split_cut(out, cur);
            continue;
        }
        if (c == '&') {
            /* `&` coupe, SAUF dans un atome de redirection :
             * `>&`, `<&` (précédé de < ou >), `&>` (suivi de >).
             * `&&` coupe deux fois (le vide est ignoré) ; `&` seul
             * (background) coupe une fois. */
            char prev = (i > 0) ? command[i - 1] : '\0';
            char next = (i + 1 < n) ? command[i + 1] : '\0';

            if (prev == '>' || prev == '<' || next == '>') {
                g_string_append_c(cur, c);
                continue;
            }
            split_cut(out, cur);
            continue;
        }
        /* `<`, `>`, `(`, `)`, `$`, backtick, `{`, `}` : jamais des
         * séparateurs (4.1 acté). Gardes textuelles (`*>*`,
         * `*$(*)*`) décidées ailleurs. */
        g_string_append_c(cur, c);
    }
    split_cut(out, cur);
    g_string_free(cur, TRUE);
    return out;
}

LlmBashRule *
llm_bash_rule_new(const char *pattern, const char *verdict_str, guint ordre)
{
    LlmBashVerdict v;
    LlmBashRule   *r;

    if (pattern == NULL || pattern[0] == '\0')
        return NULL;
    v = llm_bash_verdict_parse(verdict_str);
    if (v == LLM_BASH_INVALID)
        return NULL;
    r = g_new0(LlmBashRule, 1);
    r->pattern = g_strdup(pattern);
    r->litteraux = llm_bash_litteraux(pattern);
    r->verdict = v;
    r->ordre = ordre;
    return r;
}

void
llm_bash_rule_free(gpointer p)
{
    LlmBashRule *r = p;

    if (r == NULL)
        return;
    g_free(r->pattern);
    g_free(r);
}

void
llm_bash_rules_free(GPtrArray *rules)
{
    if (rules == NULL)
        return;
    for (guint i = 0; i < rules->len; i++)
        llm_bash_rule_free(g_ptr_array_index(rules, i));
    g_ptr_array_unref(rules);
}

LlmBashVerdict
llm_bash_verdict_segment(GPtrArray *rules, const char *segment)
{
    LlmBashVerdict best = LLM_BASH_MODE;
    int            best_lit = -1;
    guint          best_ordre = G_MAXUINT;

    if (rules == NULL || segment == NULL)
        return LLM_BASH_MODE;
    for (guint i = 0; i < rules->len; i++) {
        LlmBashRule *r = g_ptr_array_index(rules, i);
        int          lit;

        if (r == NULL || r->pattern == NULL)
            continue;
        if (r->verdict == LLM_BASH_INVALID)
            continue;
        if (!llm_bash_match(r->pattern, segment))
            continue;
        lit = r->litteraux;
        if (lit > best_lit ||
            (lit == best_lit && r->ordre < best_ordre)) {
            best = r->verdict;
            best_lit = lit;
            best_ordre = r->ordre;
        }
    }
    return best;
}

LlmBashVerdict
llm_bash_verdict_command(GPtrArray *rules, const char *command,
                         LlmBashVerdict fallback)
{
    GPtrArray     *segs;
    LlmBashVerdict worst;
    int            worst_sev;

    /* Bypass 4.9 : sans règles (ou commande vide), le hook ne fait
     * rien — pas de découpage, pas de motifs, verdict = mode. */
    if (rules == NULL || rules->len == 0 ||
        command == NULL || command[0] == '\0')
        return fallback;
    segs = llm_bash_split(command);
    if (segs->len == 0) {
        g_ptr_array_unref(segs);
        return fallback;
    }
    worst = LLM_BASH_MODE;
    worst_sev = llm_bash_severity(LLM_BASH_MODE);
    for (guint i = 0; i < segs->len; i++) {
        LlmBashVerdict v =
            llm_bash_verdict_segment(rules, g_ptr_array_index(segs, i));

        /* `"mode"` et absence de match : neutre, ne vote pas
         * (le fallback ne gagne que si tout est neutre). */
        if (v == LLM_BASH_MODE)
            continue;
        /* Pire gagne ; le `+` suit le segment gagnant (4.1) : le
         * verdict porté est lui-même le verdict plus. */
        if (llm_bash_severity(v) > worst_sev) {
            worst = v;
            worst_sev = llm_bash_severity(v);
        }
    }
    g_ptr_array_unref(segs);
    if (worst == LLM_BASH_MODE)
        return fallback;
    return worst;
}
