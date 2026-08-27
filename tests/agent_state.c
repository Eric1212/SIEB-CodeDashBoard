/*
 * tests/agent_state.c : table de vérité de la boucle agentique.
 *
 * Le bouton play/pause ET le chrono du tour dependent d'un seul predicat
 * du core, core_agent_loop_alive() :
 *   vivant -> icone pause (un clic annule tout, decision ASK comprise)
 *             et compteur qui tourne ;
 *   mort   -> play et horloge arretee.
 * C'est la loi dictee par Eric le 27 aout, et elle est verifiee ici sans
 * GTK, sans reseau et sans clic.
 *
 * Le binaire lie tous les objets sauf main.o (le test a son propre main)
 * et ne cree AUCUNE vue : views reste un GPtrArray vide, donc les deux
 * points de peinture (llm_request_free, llm_cdb_next) parcourent zero vue
 * et n'appellent jamais llm_busy_set — qui, lui, exige un bouton reel.
 * On teste la DECISION, pas le rendu (le rendu se verifie a l'oeil avec
 * CDB_DEBUG=1, trace "[btn] busy=? alive=?").
 */

#define _POSIX_C_SOURCE 200809L
#include "../src/llm.h"

#include <stdio.h>
#include <string.h>

static int fails;
static int checks;

#define CHECK(want, got, what)                                        \
    do {                                                              \
        checks++;                                                     \
        if ((int) (want) != (int) (got)) {                            \
            fails++;                                                  \
            printf("  ECHEC %-50s attendu=%d obtenu=%d\n",            \
                   (what), (int) (want), (int) (got));                \
        } else {                                                      \
            printf("  ok    %-50s -> %d\n", (what), (int) (got));     \
        }                                                             \
    } while (0)

#define CHECK_PTR(want, got, what)                                    \
    do {                                                              \
        checks++;                                                     \
        if ((want) != (got)) {                                        \
            fails++;                                                  \
            printf("  ECHEC %-50s attendu=%p obtenu=%p\n",            \
                   (what), (void *) (want), (void *) (got));          \
        } else {                                                      \
            printf("  ok    %-50s -> %p\n", (what), (void *) (got));  \
        }                                                             \
    } while (0)

/* Un core nu : assez pour les predicats, sans reseau ni vue. */
static LlmCore *
core_new_bare(void)
{
    LlmCore *c = g_new0(LlmCore, 1);

    c->views = g_ptr_array_new();
    c->reply = g_string_new(NULL);
    return c;
}

static void
core_free_bare(LlmCore *c)
{
    g_ptr_array_free(c->views, TRUE);
    g_string_free(c->reply, TRUE);
    g_free(c);
}

static CdbPoll *
poll_new(LlmCore *c, gboolean cancelled)
{
    CdbPoll *pl = g_new0(CdbPoll, 1);

    pl->core      = c;
    pl->cancelled = cancelled;
    cdb_poll_register(pl);
    return pl;
}

static void
poll_drop(CdbPoll *pl)
{
    cdd_poll_unregister(pl);
    g_free(pl);
}

/* ---- la table de verite ---- */

static void
test_alive(LlmCore *c)
{
    CHECK(FALSE, core_agent_loop_alive(c),
          "core nu : boucle morte -> play, horloge arretee");
}

static void
test_request_in_flight(LlmCore *c)
{
    LlmRequest *req = g_new0(LlmRequest, 1);

    req->core  = c;
    c->cur_req = req;
    CHECK(TRUE, core_agent_loop_alive(c),
          "requete en vol : pause + chrono");
    c->cur_req = NULL;
    g_free(req);
}

static void
test_ask_pending(LlmCore *c)
{
    CdbDecision *d = g_new0(CdbDecision, 1);

    c->decision = d;
    /* LA loi d'Eric : le systeme EST busy pendant un ASK. Le clic sur le
     * bouton annule donc la decision (llm_cancel_current) et le compteur
     * continue de tourner pendant qu'il reflechit. */
    CHECK(TRUE, core_agent_loop_alive(c),
          "ASK en attente : busy (pause) + chrono qui tourne");
    c->decision = NULL;
    g_free(d);
}

static void
test_queue_nonempty(LlmCore *c)
{
    c->cmd_queue = g_queue_new();
    g_queue_push_tail(c->cmd_queue, g_new0(CdbCmdSpec, 1));
    CHECK(TRUE, core_agent_loop_alive(c), "file non vide : boucle vivante");
    g_free(g_queue_pop_head(c->cmd_queue));
    g_queue_free(c->cmd_queue);
    c->cmd_queue = NULL;
    CHECK(FALSE, core_agent_loop_alive(c), "file videe : boucle morte");
}

static void
test_poll_active(LlmCore *c)
{
    CdbPoll *pl = poll_new(c, FALSE);

    CHECK(TRUE, core_agent_loop_alive(c), "poll bash : boucle vivante");
    poll_drop(pl);
    CHECK(FALSE, core_agent_loop_alive(c), "poll parti : ne compte plus");
}

static void
test_poll_cancelled(LlmCore *c)
{
    CdbPoll *pl = poll_new(c, TRUE);

    /* Annule : sa reponse a deja ete envoyee, il ne tient plus rien. */
    CHECK(FALSE, core_agent_loop_alive(c), "poll annule : ne compte pas");
    poll_drop(pl);
}

static void
test_poll_isolation(LlmCore *c)
{
    LlmCore *autre = core_new_bare();
    CdbPoll *pl    = poll_new(autre, FALSE);

    /* Les polls sont globaux, partages par tous les cores : un poll d'un
     * AUTRE fil ne doit pas rendre celui-ci vivant, sinon deux sessions
     * se marchent sur le bouton. */
    CHECK(FALSE, core_agent_loop_alive(c),
          "poll d'un autre core : ne compte pas");
    poll_drop(pl);
    core_free_bare(autre);
}

static void
test_stop_dominates(LlmCore *c)
{
    LlmRequest  *req = g_new0(LlmRequest, 1);
    CdbDecision *d   = g_new0(CdbDecision, 1);

    req->core         = c;
    c->cur_req        = req;  /* pas encore liberee par le callback lecture */
    c->decision       = d;    /* et une decision traine encore */
    c->stop_requested = TRUE;
    /* Une boucle annulee est MORTE, quel que soit l'etat des pointeurs :
     * c'est ce qui empeche le chrono de tourner dans le vide apres pause. */
    CHECK(FALSE, core_agent_loop_alive(c), "pause cliquee : boucle morte");
    c->stop_requested = FALSE;
    c->cur_req  = NULL;
    c->decision = NULL;
    g_free(req);
    g_free(d);
}

/* LE bug d'origine, ecrit en clair : la requete n1 se termine pendant que
 * llm_cdb_next() en a deja relance une n2. L'ancien code posait busy=FALSE
 * en sortant de n1, ce qui peignait play et coupait le chrono en plein
 * tour de tools — et ouvrait la porte a une SECONDE requete concurrentlye
 * (un clic sur play envoie au lieu d'annuler). */
static void
test_tool_turn_regression(LlmCore *c)
{
    LlmRequest *n2 = g_new0(LlmRequest, 1);

    n2->core   = c;
    c->cur_req = n2;
    CHECK(TRUE, core_agent_loop_alive(c),
          "tour de tools : requete suivante en vol -> pause");
    c->cur_req = NULL;
    g_free(n2);
}

/* ---- les deux points de peinture ---- */

/* llm_request_free est le premier : la derniere requete du fil meurt-elle
 * que le bouton retombe (si rien d'autre ne tient la boucle). Sans vue, la
 * diffusion ne peint rien : on verifie l'etat decide, pas le rendu. */
static void
test_request_free(LlmCore *c)
{
    LlmRequest *req = g_new0(LlmRequest, 1);

    req->core  = c;
    c->cur_req = req;
    llm_request_free(req);
    CHECK_PTR(NULL, c->cur_req, "request_free : cur_req detachee");
    CHECK(FALSE, core_agent_loop_alive(c),
          "request_free seul : boucle morte, bouton play");

    /* La garde anti double-liberation ne doit pas decrocher la requete
     * vivante d'un tour de tools. */
    {
        LlmRequest *mort = g_new0(LlmRequest, 1);
        LlmRequest *vive = g_new0(LlmRequest, 1);

        mort->core = c;
        mort->done = 1;
        vive->core = c;
        c->cur_req = vive;
        llm_request_free(mort);
        CHECK_PTR(vive, c->cur_req,
                  "request_free d'une requete morte : cur_req intact");
        g_free(mort);   /* done=1 : llm_request_free ne libere pas le bloc */
        c->cur_req = NULL;
        g_free(vive);
    }
}

/* llm_cdb_next est le second : chaque avancee de la file repeint. Appelee
 * sur un core nu, elle ne doit RIEN casser (file vide, zero vue, pas de
 * re-requete possible) et laisser la boucle morte. */
static void
test_next_no_views(LlmCore *c)
{
    llm_cdb_next(c);
    CHECK(FALSE, core_agent_loop_alive(c),
          "llm_cdb_next sur core nu : rien ne s'invente");
    CHECK_PTR(NULL, c->cur_req, "llm_cdb_next sans vue : pas de requete");
}

/* ... et si un poll roule pendant l'avancee, la boucle reste vivante :
 * c'est la fenetre ou l'outil s'execute sans requete reseau. */
static void
test_next_with_poll(LlmCore *c)
{
    CdbPoll *pl = poll_new(c, FALSE);

    llm_cdb_next(c);
    CHECK(TRUE, core_agent_loop_alive(c),
          "llm_cdb_next avec poll : la boucle survit a l'avancee");
    poll_drop(pl);
}

int
main(void)
{
    LlmCore *c = core_new_bare();

    printf("== boucle agentique : vivant = pause + chrono, mort = play ==\n");
    test_alive(c);
    test_request_in_flight(c);
    test_ask_pending(c);
    test_queue_nonempty(c);
    test_poll_active(c);
    test_poll_cancelled(c);
    test_poll_isolation(c);
    test_stop_dominates(c);
    test_tool_turn_regression(c);
    test_request_free(c);
    test_next_no_views(c);
    test_next_with_poll(c);

    core_free_bare(c);

    printf("== %d verifications, %d echec(s) ==\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
