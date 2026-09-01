/*
 * test_textops.c — critere d'acceptation de la couche pure des outils
 * fichiers. Cible `make check`.
 *
 * Why this file exists : les bugs de cdb_replace (la ligne suivante avalee,
 * le \n final perdu) ne sont pas des bugs d'appel reseau ni de GTK. Ils
 * habitent tous l'adressage des lignes et la politique de terminateur.
 * Les verifier ici se fait sans CDB, sans terminal, sans approbation, et
 * surtout SANS dependre de ma parole : `make check` doit passer avant que
 * quoi que ce soit d'autre soit touche.
 *
 * Une fonction de ce fichier (legacy_splice) reproduit VOLONTAIREMENT la
 * semantique d'aujourd'hui. Elle est la pour demontrer le degat, pas pour
 * servir de modele. Elle est marquee en consequence.
 */

#include "../src/textops.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("  ECHEC  %s:%d  %s\n", __FILE__, __LINE__, (msg));         \
        }                                                                      \
    } while (0)

static GArray *
offsets_of(const char *s, guint *line_count)
{
    GArray *off = g_array_new(FALSE, FALSE, sizeof(gsize));

    textops_line_offsets(s, strlen(s), off, line_count);
    return off;
}

static void
free_offsets(GArray *off)
{
    g_array_free(off, TRUE);
}

static guint
count_of(const char *s)
{
    guint lc = 0;
    GArray *off = offsets_of(s, &lc);

    free_offsets(off);
    return lc;
}

/* ------------------------------------------------------------------ */
/* 1. Adressage : la regle des offsets                                 */
/* ------------------------------------------------------------------ */

static void
test_line_offsets(void)
{
    guint lc;
    GArray *off;

    printf("== line_offsets\n");

    off = offsets_of("A\nB\nC\n", &lc);
    CHECK(lc == 3, "LF avec \n final : 3 lignes");
    CHECK(off->len == 4, "tableau = line_count + 1");
    CHECK(g_array_index(off, gsize, 0) == 0, "off[0] = 0");
    CHECK(g_array_index(off, gsize, 1) == 2, "off[1] = 2");
    CHECK(g_array_index(off, gsize, 3) == 6, "off[3] = len");
    free_offsets(off);

    off = offsets_of("A\nB", &lc);          /* pas de \n final */
    CHECK(lc == 2, "sans \n final : 2 lignes");
    CHECK(g_array_index(off, gsize, 2) == 3, "off[2] = len (pas de trou)");
    free_offsets(off);

    off = offsets_of("", &lc);
    CHECK(lc == 0, "fichier vide : 0 ligne");
    CHECK(off->len == 1, "fichier vide : off a 1 element");
    CHECK(g_array_index(off, gsize, 0) == 0, "fichier vide : off[0] = 0");
    free_offsets(off);

    off = offsets_of("\n", &lc);
    CHECK(lc == 1, "un seul \n : 1 ligne vide terminee");
    free_offsets(off);

    off = offsets_of("A\n\nB\n", &lc);      /* ligne vide au milieu */
    CHECK(lc == 3, "ligne vide du milieu compte comme une ligne");
    free_offsets(off);

    off = offsets_of("A\r\nB\r\n", &lc);    /* CRLF */
    CHECK(lc == 2, "CRLF : 2 lignes");
    CHECK(g_array_index(off, gsize, 1) == 3, "CRLF : une ligne porte 2 octets");
    CHECK(g_array_index(off, gsize, 2) == 6, "CRLF : off[2] = len");
    free_offsets(off);

    /* Invariant : le dernier element de off est TOUJOURS len. Un offset
     * de fin faux est la source directe d'une zone de coupure fausse. */
    const char *probe[] = { "", "\n", "A", "A\n", "A\nB", "A\nB\n",
                            "A\r\nB", "A\r\nB\r\n", "\n\n\n" };
    for (guint i = 0; i < G_N_ELEMENTS(probe); i++) {
        gsize len = strlen(probe[i]);

        off = offsets_of(probe[i], &lc);
        CHECK(g_array_index(off, gsize, off->len - 1) == len,
              "invariant : off[line_count] == len");
        CHECK(off->len == lc + 1, "invariant : taille = line_count + 1");
        free_offsets(off);
    }
}

/* ------------------------------------------------------------------ */
/* 2. Hash : gele, et distinct des contenus qui se ressemblent          */
/* ------------------------------------------------------------------ */

static void
test_hash(void)
{
    char *a, *b, *c;

    printf("== hash4\n");

    a = textops_hash4("B\n", 2);
    b = textops_hash4("B\n", 2);
    c = textops_hash4("XC\n", 3);           /* le cas du remplacement avale */
    CHECK(strlen(a) == 4, "hash = 4 caracteres");
    CHECK(strcmp(a, b) == 0, "deterministe");
    for (int i = 0; i < 4; i++)
        CHECK((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'z'),
              "jeu base36 minuscule");
    CHECK(strcmp(a, c) != 0, "\"B\\n\" et \"XC\\n\" ne se ressemblent pas");

    /* Le hash couvre bien la terminaison : lire une ligne n'est pas lire
     * son contenu seul, et un bloc change de hash si un bord bouge. */
    {
        char *with_nl = textops_hash4("A\n", 2);
        char *no_nl   = textops_hash4("A", 1);

        CHECK(strcmp(with_nl, no_nl) != 0, "la terminaison entre au hash");
        g_free(with_nl);
        g_free(no_nl);
    }
    g_free(a);
    g_free(b);
    g_free(c);
}

/* ------------------------------------------------------------------ */
/* 3. Style de fin de ligne                                            */
/* ------------------------------------------------------------------ */

static void
test_scan_eol(void)
{
    TextopsEol e;

    printf("== scan_eol\n");

    textops_scan_eol("A\nB\n", 4, &e);
    CHECK(e.eol_len == 1, "LF reconnu");
    CHECK(e.final_nl == TRUE, "LF final present");

    textops_scan_eol("A\nB", 3, &e);
    CHECK(e.final_nl == FALSE, "pas de \n final : detecte");

    textops_scan_eol("A\r\nB\r\n", 6, &e);
    CHECK(e.eol_len == 2, "CRLF reconnu");
    CHECK(e.final_nl == TRUE, "CRLF final detecte");

    textops_scan_eol("A\r\nB\n", 5, &e);
    CHECK(e.eol_len == 1, "fichier mele -> LF, pas de normalisation silencieuse");

    textops_scan_eol("", 0, &e);
    CHECK(e.eol_len == 1 && e.final_nl == FALSE,
          "fichier vide : LF, final_nl FALSE (rien a preserver)");

    textops_scan_eol("ABC", 3, &e);
    CHECK(e.eol_len == 1 && e.final_nl == FALSE, "une ligne sans saut");
}

/* ------------------------------------------------------------------ */
/* 4. Zone d'un bloc                                                   */
/* ------------------------------------------------------------------ */

static void
test_block_range(void)
{
    guint lc;
    GArray *off;
    gsize s = 99, t = 99;
    gboolean ok;

    printf("== block_range\n");

    off = offsets_of("A\nB\nC\n", &lc);

    ok = textops_block_range(6, off, lc, 2, 2, &s, &t);
    CHECK(ok && s == 2 && t == 4, "ligne 2 = octets 2..4, terminaison incluse");

    ok = textops_block_range(6, off, lc, 1, 3, &s, &t);
    CHECK(ok && s == 0 && t == 6, "tout le fichier = 0..len");

    ok = textops_block_range(6, off, lc, 1, 1, &s, &t);
    CHECK(ok && s == 0 && t == 2, "premiere ligne");

    s = t = 99;
    ok = textops_block_range(6, off, lc, 1, 4, &s, &t);
    CHECK(!ok, "to hors fichier : refuse");
    CHECK(s == 99 && t == 99, "refus : les sorties ne sont pas ecrites");

    CHECK(!textops_block_range(6, off, lc, 0, 2, &s, &t), "from = 0 refuse");
    CHECK(!textops_block_range(6, off, lc, 3, 2, &s, &t), "to < from refuse");
    CHECK(!textops_block_range(6, NULL, lc, 1, 1, &s, &t), "off NULL refuse");
    free_offsets(off);

    /* Fichier vide : off ne contient qu'un element, aucun bloc ne peut
     * s'y designer. Le cas est distinct de "hors bornes" : ici il n'y a
     * pas de bornes du tout. */
    off = offsets_of("", &lc);
    CHECK(lc == 0, "fichier vide : line_count 0");
    CHECK(!textops_block_range(0, off, lc, 1, 1, &s, &t),
          "fichier vide : aucun bloc");
    free_offsets(off);

    /* Sans \n final, la derniere zone s'arrete a len et pas plus loin. */
    off = offsets_of("A\nB", &lc);
    ok = textops_block_range(3, off, lc, 2, 2, &s, &t);
    CHECK(ok && s == 2 && t == 3, "derniere ligne sans terminaison");
    free_offsets(off);
}

/* ------------------------------------------------------------------ */
/* 5. Jointure : le coeur du nouveau contrat de replace                */
/* ------------------------------------------------------------------ */

static void
test_join_block(void)
{
    TextopsEol lf, crlf;
    gsize len = 0;
    char *out;

    printf("== join_block\n");

    textops_scan_eol("x\n", 2, &lf);
    textops_scan_eol("x\r\n", 3, &crlf);

    /* Milieu de fichier : chaque ligne recoit son terminateur, y compris
     * la derniere, sinon elle fusionnerait avec la suivante. */
    {
        const char *b[] = { "1", "2", "3", "4" };

        out = textops_join_block(b, 4, &lf, FALSE, &len, NULL);
        CHECK(out && strcmp(out, "1\n2\n3\n4\n") == 0,
              "bloc du milieu : 4 lignes terminees");
        g_free(out);
    }

    /* Fin de fichier, convention presente -> le \n final est rendu. */
    {
        const char *b[] = { "1", "2" };

        out = textops_join_block(b, 2, &lf, TRUE, &len, NULL);
        CHECK(out && strcmp(out, "1\n2\n") == 0, "at_eof + final_nl -> \n final garde");
        g_free(out);
    }

    /* Fin de fichier, convention absente -> AUCUN \n final invente.
     * C'est le cas que l'ancien contrat laissait a la charge du modele. */
    {
        const char *b[] = { "1", "2" };
        TextopsEol e = { "\n", 1, FALSE };

        out = textops_join_block(b, 2, &e, TRUE, &len, NULL);
        CHECK(out && strcmp(out, "1\n2") == 0, "at_eof sans final_nl -> rien ajoute");
        g_free(out);
    }

    /* Ligne vide = ligne ECOITE, pas ligne avalee. Le cas de Eric. */
    {
        const char *b[] = { "a", "", "b" };

        out = textops_join_block(b, 3, &lf, FALSE, &len, NULL);
        CHECK(out && strcmp(out, "a\n\nb\n") == 0, "\"\" rend une ligne vide");
        g_free(out);
    }
    {
        const char *b[] = { "", "", "" };

        out = textops_join_block(b, 3, &lf, FALSE, &len, NULL);
        CHECK(out && strcmp(out, "\n\n\n") == 0, "bloc entierement vide");
        CHECK(len == 3, "longueur annoncee = 3");
        g_free(out);
    }
    {
        const char *b[] = { " " };
        TextopsEol e = { "\n", 1, FALSE };

        out = textops_join_block(b, 1, &e, TRUE, &len, NULL);
        CHECK(out && strcmp(out, " ") == 0, "\" \" est une ligne, pas un effacement");
        g_free(out);
    }

    /* CRLF preserve. */
    {
        const char *b[] = { "a", "b" };

        out = textops_join_block(b, 2, &crlf, FALSE, &len, NULL);
        CHECK(out && strcmp(out, "a\r\nb\r\n") == 0, "CRLF joint en CRLF");
        CHECK(len == 6, "longueur CRLF = 6");
        g_free(out);
    }

    /* Un element ne peut pas cacher deux lignes. */
    {
        const char *b[] = { "ok", "mauvaise\nligne" };
        TextopsJoinErr err = { 0, 0 };

        out = textops_join_block(b, 2, &lf, FALSE, &len, &err);
        CHECK(out == NULL, "element avec \\n : refuse");
        CHECK(err.bad_line == 2, "numero de l'element fautif rendu");
        CHECK(err.bad_char == '\n', "caractere fautif rendu");
        g_free(out);
    }
    {
        const char *b[] = { "cr\rache" };
        TextopsJoinErr err = { 0, 0 };

        out = textops_join_block(b, 1, &lf, FALSE, &len, &err);
        CHECK(out == NULL, "element avec \\r : refuse");
        CHECK(err.bad_line == 1 && err.bad_char == '\r', "diagnostic \\r");
        g_free(out);
    }

    /* n = 0 = zone vide (le chemin du remove, pas du replace). */
    out = textops_join_block(NULL, 0, &lf, FALSE, &len, NULL);
    CHECK(out && len == 0 && out[0] == '\0', "bloc de 0 ligne = vide");
    g_free(out);
}

/* ------------------------------------------------------------------ */
/* 6. aller-retour split . join                                        */
/* ------------------------------------------------------------------ */

static void
test_round_trip(void)
{
    const char *files[] = { "A\nB\nC\n", "A\nB", "A\r\nB\r\n", "A\n\nB\n" };

    printf("== split/join round-trip\n");

    for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
        const char *src = files[i];
        guint lc = 0;
        GArray *off = offsets_of(src, &lc);
        GPtrArray *lines = g_ptr_array_new();
        TextopsEol e;
        const char **copy = g_new0(const char *, lc);
        char *back;
        gsize blen = 0;

        textops_scan_eol(src, strlen(src), &e);
        textops_split_block(src, strlen(src), lines);
        CHECK(lines->len == lc, "split rend exactement line_count lignes");

        for (guint k = 0; k < lines->len; k++)
            copy[k] = g_ptr_array_index(lines, k);
        back = textops_join_block(copy, lc, &e, TRUE, &blen, NULL);
        CHECK(back && strcmp(back, src) == 0,
              "rejointure a l'identique (convention preservee)");
        CHECK(back && strlen(back) == blen, "longueur annoncee = longueur rendue");

        g_free(back);
        g_free(copy);
        g_ptr_array_set_free_func(lines, g_free);
        g_ptr_array_free(lines, TRUE);
        free_offsets(off);
    }
}

/* ------------------------------------------------------------------ */
/* 7. Temoin de l'ancien comportement (NE PAS COPIER)                  */
/* ------------------------------------------------------------------ */

/* Reproduction fidele de ce que fait cdb_replace aujourd'hui : la zone
 * remplacee va de off[from-1] a off[to] (terminaison incluse) et le texte
 * du modele est insere VERBATIM, sans que rien n'exige ni n'interdise un
 * \n final. Le reste de la jointure est a la charge du modele. */
static char *
legacy_splice(const char *content, GArray *off, guint from, guint to,
              const char *text)
{
    gsize a = g_array_index(off, gsize, from - 1);
    gsize b = g_array_index(off, gsize, to);
    gsize len = strlen(content);
    GString *s = g_string_new(NULL);

    g_string_append_len(s, content, a);
    g_string_append(s, text);
    g_string_append_len(s, content + b, len - b);
    return g_string_free(s, FALSE);
}

static void
test_legacy_witness(void)
{
    guint lc;
    GArray *off;
    char *after;

    printf("== temoin de l'ancien contrat (prouve le bug)\n");

    /* Cas 1 : remplacer la ligne 2 d'un fichier de 3 lignes par "X".
     * Le modele a lu read(2,2), rejoue son hash, et croit ecrire une
     * ligne. Il vient de detruire la ligne 3. */
    off = offsets_of("A\nB\nC\n", &lc);
    CHECK(lc == 3, "avant : 3 lignes");
    after = legacy_splice("A\nB\nC\n", off, 2, 2, "X");
    CHECK(strcmp(after, "A\nXC\n") == 0, "ancien contrat : la ligne 3 fusionne");
    CHECK(count_of(after) == 2, "ancien contrat : 3 lignes deviennent 2");
    g_free(after);
    free_offsets(off);

    /* Cas 2 : derniere ligne -> le \n final du fichier est perdu. */
    off = offsets_of("A\nB\n", &lc);
    after = legacy_splice("A\nB\n", off, 2, 2, "X");
    CHECK(strcmp(after, "A\nX") == 0, "ancien contrat : \n final avale");
    CHECK(after[strlen(after) - 1] != '\n', "ancien contrat : fichier non termine");
    g_free(after);
    free_offsets(off);

    /* Cas 3 : une ligne vide envoyee comme texte vide SUPPRIME la ligne
     * au lieu de la vider. */
    off = offsets_of("A\nB\nC\n", &lc);
    after = legacy_splice("A\nB\nC\n", off, 2, 2, "");
    CHECK(strcmp(after, "A\nC\n") == 0, "ancien contrat : texte vide avale la ligne");
    g_free(after);
    free_offsets(off);

    /* Meme entree, nouveau contrat : la geometrie ne bouge pas. */
    {
        const char *src = "A\nB\nC\n";
        const char *b[] = { "X" };
        TextopsEol e;
        gsize a = 0, z = 0, jlen = 0;
        char *joined, *fresh;
        GString *s;

        textops_scan_eol(src, strlen(src), &e);
        off = offsets_of(src, &lc);
        CHECK(textops_block_range(strlen(src), off, lc, 2, 2, &a, &z),
              "bloc 2-2 trouve");
        joined = textops_join_block(b, 1, &e, FALSE, &jlen, NULL);
        s = g_string_new(NULL);
        g_string_append_len(s, src, a);
        g_string_append_len(s, joined, jlen);
        g_string_append_len(s, src + z, strlen(src) - z);
        fresh = g_string_free(s, FALSE);

        CHECK(strcmp(fresh, "A\nX\nC\n") == 0,
              "nouveau contrat : ligne 3 intacte");
        CHECK(count_of(fresh) == 3,
              "nouveau contrat : 3 lignes, invariant");
        CHECK(strlen(fresh) == 6,
              "nouveau contrat : meme taille que l'original");
        g_free(fresh);
        g_free(joined);
        free_offsets(off);
    }
}

/* ------------------------------------------------------------------ */
/* 8. Invariant de geometrie : k -> k ne deplace jamais rien           */
/* ------------------------------------------------------------------ */

static void
test_geometry_invariant(void)
{
    struct { const char *file; guint from, to; } cases[] = {
        { "A\nB\nC\n",        1, 1 },
        { "A\nB\nC\n",        2, 2 },
        { "A\nB\nC\n",        3, 3 },
        { "A\nB\nC\n",        1, 3 },
        { "A\nB\nC\n",        2, 3 },
        { "A\nB",             2, 2 },
        { "A\nB",             1, 2 },
        { "A\r\nB\r\nC\r\n",  2, 2 },
        { "A\n\nC\n",         2, 2 },
        { "A\nB\nC",          1, 2 },
    };

    printf("== invariant k -> k\n");

    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        const char *src = cases[i].file;
        guint lc0 = 0, lc1 = 0;
        GArray *off = offsets_of(src, &lc0);
        TextopsEol e;
        gsize a = 0, z = 0, jlen = 0, k = cases[i].to - cases[i].from + 1;
        const char **lines = g_new0(const char *, k);
        char *joined, *fresh;
        GString *s;

        textops_scan_eol(src, strlen(src), &e);
        CHECK(textops_block_range(strlen(src), off, lc0,
                                  cases[i].from, cases[i].to, &a, &z),
              "bloc dans les bornes");
        for (guint j = 0; j < k; j++)
            lines[j] = "N";                    /* meme contenu, n importe */
        joined = textops_join_block(lines, k, &e, cases[i].to == lc0,
                                    &jlen, NULL);
        CHECK(joined != NULL, "jointure acceptee");

        s = g_string_new(NULL);
        g_string_append_len(s, src, a);
        g_string_append_len(s, joined, jlen);
        g_string_append_len(s, src + z, strlen(src) - z);
        fresh = g_string_free(s, FALSE);

        {
            GArray *o1 = offsets_of(fresh, &lc1);

            CHECK(lc1 == lc0,
                  "remplacement k->k : le compte de lignes ne bouge pas");
            free_offsets(o1);
        }
        /* Ce qui est invariant ici est la GEOMETRIE (le nombre de lignes),
         * jamais la taille en octets : remplacer une ligne vide par "N"
         * agrandit le fichier, et c'est legitime. L'ancien contrat, lui,
         * changeait le nombre de lignes. */
        /* La convention de fin de fichier survit. */
        CHECK((fresh[strlen(fresh) - 1] == '\n') == e.final_nl,
              "le \n final du fichier est conserve");

        g_free(fresh);
        g_free(joined);
        g_free(lines);
        free_offsets(off);
    }
}

int
main(void)
{
    test_line_offsets();
    test_hash();
    test_scan_eol();
    test_block_range();
    test_join_block();
    test_round_trip();
    test_legacy_witness();
    test_geometry_invariant();

    printf("\n%d checks, %d echecs\n", g_pass + g_fail, g_fail);
    if (g_fail == 0) {
        printf("ok  textops\n");
        return 0;
    }
    printf("ECHEC textops\n");
    return 1;
}
