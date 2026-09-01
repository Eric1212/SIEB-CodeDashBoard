/*
 * ibox.c : boîte interactive à trois zones — voir ibox.h.
 *
 * Aucune dépendance au core, aux outils, ni à la tuile : cette pièce ne
 * fait que rendre du texte et collecter un choix. Qui décide de QUOI que
 * ce soit reste chez l'appelant.
 *
 * Les deux zones de texte sont des GtkTextView non éditables : c'est le
 * seul widget qui accepte 100 000 lignes pour 0 ms (mesure dans ibox.h).
 * Pas de troncature, pas de plafond de données — seulement une hauteur
 * affichée, calculée ligne par ligne et bornée.
 */

#include "ibox.h"
#include "i18n.h"

/* Un GtkTextView avec wrap=NONE rend exactement une ligne visuelle par
 * ligne du texte : la hauteur est donc calculable au caractère près.
 * 20 px = monospace 10pt avec l'interlignage d'Adwaita. */
#define IB_LINE_PX     20
#define IB_IN_MAX_H  140      /* zone input dépliée, au-delà elle défile */
#define IB_OUT_MAX_H 360      /* zone output dépliée idem                 */

typedef struct {
    GtkWidget *self;          /* la boîte (emprunté : elle nous possède) */

    /* zone 1 : input */
    GtkWidget *in_box;
    GtkWidget *in_fold;
    GtkWidget *in_digest;   /* la demande, lisible même zone repliée */
    GtkWidget *in_revealer;
    GtkWidget *in_scroll;
    GtkTextBuffer *in_buf;

    /* zone 2 : choix — jamais pliable, jamais changée de HAUTEUR. Deux
     * moitiés jointives, et non deux boutons posés sur un fond : chacune
     * fait 50 % de la barre, et celle qu'Éric choisit mange l'autre. */
    GtkWidget *ch_box;
    GtkWidget *ch_yes;
    GtkWidget *ch_no;
    char      *choice_text;   /* libellé imposé : n'arbitre que le OUI  */
    gboolean   decided;       /* décision prise : la boîte est repliée et
                               * ne se redéplie plus toute seule */
    gulong     ch_anim;       /* tick de l'animation, 0 si aucune */

    /* zone 3 : output */
    GtkWidget *out_box;
    GtkWidget *out_fold;
    GtkWidget *out_digest;
    GtkWidget *out_revealer;
    GtkWidget *out_scroll;
    GtkWidget *out_more;
    GtkTextBuffer *out_buf;
    char       *input;        /* possédés */
    char       *output;       /* TOUJOURS le texte intégral */
    IboxChoice  choice;

    IboxChosen  cb_choice;   gpointer ud_choice;
    IboxShowAll cb_showall;  gpointer ud_showall;
} Ibox;

static Ibox *
ibox_of(GtkWidget *w)
{
    return g_object_get_data(G_OBJECT(w), "cdb-ibox");
}

/* Lignes : un \n de plus = une ligne de plus, et une chaîne non vide qui
 * ne finit pas par \n compte quand même pour une ligne. */
static gsize
count_lines(const char *s)
{
    gsize n = 0;

    if (s == NULL || s[0] == '\0')
        return 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\n')
            n++;
    }
    if (s[strlen(s) - 1] != '\n')
        n++;
    return n;
}

/* ----- une zone de texte : GtkTextView fainéant, donc tout est permis -- */

static GtkWidget *
pane_new(GtkTextBuffer **buf_out)
{
    GtkWidget   *sw = gtk_scrolled_window_new();
    GtkWidget   *v  = gtk_text_view_new();

    gtk_text_view_set_editable(GTK_TEXT_VIEW(v), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(v), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(v), GTK_WRAP_NONE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(v), TRUE);
    /* Comme un terminal : le texte déborde à droite, il ne se re-wrappe
     * pas — c'est ce qui garde le compte de lignes juste. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), v);
    *buf_out = gtk_text_view_get_buffer(GTK_TEXT_VIEW(v));
    return sw;
}

/* Hauteur affichée = lignes réelles, bornée par le plafond de la zone. */
static void
pane_fill(GtkTextBuffer *buf, GtkWidget *scroll, const char *text,
          int max_h)
{
    gsize lines = count_lines(text);
    int   h     = (int)lines * IB_LINE_PX;

    gtk_text_buffer_set_text(buf, text != NULL ? text : "", -1);
    if (h < IB_LINE_PX)
        h = IB_LINE_PX;
    if (h > max_h)
        h = max_h;
    gtk_widget_set_size_request(scroll, -1, h);
}

/* ----- pliage ------------------------------------------------------- */

static void
on_fold_toggled(GtkToggleButton *btn, gpointer G_GNUC_UNUSED unused)
{
    gboolean  open    = gtk_toggle_button_get_active(btn);
    GtkWidget *arrow  = g_object_get_data(G_OBJECT(btn), "ibox-arrow");
    GtkWidget *digest = g_object_get_data(G_OBJECT(btn), "ibox-digest");

    /* Mêmes chevrons que la barre de statut de la tuile : des icônes GTK
     * nommées, pas des caractères. Elles suivent le thème et s'alignent
     * sur les autres plisages de CDB. */
    if (arrow != NULL)
        gtk_image_set_from_icon_name(GTK_IMAGE(arrow),
                                     open ? "pan-up-symbolic"
                                          : "pan-down-symbolic");

    /* Le digest n'a de raison d'être que ZONE REPLIÉE : dépliée, il
     * répète le contenu (« input bash-0 $ date » au-dessus de
     * « bash-0 $ date »). On le rend INVISIBLE, on ne le cache pas : c'est
     * lui le widget hexpand de la bande, donc un set_visible(FALSE) le
     * retirerait du layout et le chevron glisserait coller à l'étiquette.
     * L'opacité garde la place, le chevron ne bouge pas. Les deux
     * pointeurs sont empruntés : descendants du bouton, ils meurent avec
     * lui. */
    if (digest != NULL)
        gtk_widget_set_opacity(digest, open ? 0.0 : 1.0);
}

/* En-tête d'une zone pliable. LA BANDE ENTIERE EST LE BOUTON : un clic
 * n'importe où dessus plie ou déplie, et le chevron n'est plus qu'un
 * indicateur. C'est le modèle de GtkExpander, et ce n'est pas une
 * fantaisie d'ergonomie : laisser le chevron-bouton LOGÉ dans une bande
 * clictable ferait déclencher les deux sur un clic dessus, qui
 * s'annulerait donc lui-même.
 *
 * Le widget rendu est un GtkToggleButton — les appelants le gardent dans
 * in_fold / out_fold, si bien que la liaison « active -> reveal-child »
 * et les ibox_set_expanded() / ibox_decided() existants marchent sans
 * qu'on y touche. */
static GtkWidget *
zone_head(const char *tag, GtkWidget **fold_out, GtkWidget **digest_out)
{
    GtkWidget *fold  = gtk_toggle_button_new();
    GtkWidget *row   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *tag_l = gtk_label_new(tag);
    GtkWidget *arrow = gtk_image_new_from_icon_name("pan-up-symbolic");

    gtk_image_set_pixel_size(GTK_IMAGE(arrow), 12);
    gtk_widget_add_css_class(tag_l, "ibox-tag");
    gtk_label_set_xalign(GTK_LABEL(tag_l), 0.0);
    gtk_box_append(GTK_BOX(row), tag_l);

    if (digest_out != NULL) {
        GtkWidget *digest = gtk_label_new("");

        /* Ellipsize : la tuile peut être étroite ; le digest doit alors
         * s'abrèger proprement au lieu de couper la boîte ou de forcer sa
         * largeur naturelle au-delà des 90 % demandés. */
        gtk_widget_add_css_class(digest, "ibox-digest");
        gtk_label_set_xalign(GTK_LABEL(digest), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(digest), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(digest, TRUE);
        gtk_box_append(GTK_BOX(row), digest);
        *digest_out = digest;
    } else {
        GtkWidget *gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

        gtk_widget_set_hexpand(gap, TRUE);
        gtk_box_append(GTK_BOX(row), gap);
    }

    gtk_box_append(GTK_BOX(row), arrow);
    gtk_button_set_child(GTK_BUTTON(fold), row);
    /* ibox-head : la CSS aplatit complètement ce bouton (aucun fond,
     * aucune bordure, aucune graisse, couleur héritée) — sinon il
     * peindrait par-dessus la couleur de sa zone et imposerait sa propre
     * hauteur minimale. */
    gtk_widget_add_css_class(fold, "ibox-head");

    /* Le handler ne reçoit que le bouton : il retrouve les deux widgets de
     * la bande par data. Posés AVANT le connect, et le set_active qui suit
     * est ce qui fait l'office de synchro initiale (le bouton nait à
     * FALSE, passer à TRUE émet « toggled »). */
    g_object_set_data(G_OBJECT(fold), "ibox-arrow", arrow);
    if (digest_out != NULL)
        g_object_set_data(G_OBJECT(fold), "ibox-digest", *digest_out);
    g_signal_connect(fold, "toggled", G_CALLBACK(on_fold_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fold), TRUE);

    if (fold_out != NULL)
        *fold_out = fold;
    return fold;
}

/* Pliage commun des deux zones, déclenché par un clic sur la barre de
 * choix. Appelé SEULEMENT quand la décision est prise : masquer l'énoncé
 * d'une décision à prendre serait la faire prendre à l'aveugle.
 *
 * Ce n'est plus un contrôleur de geste sur la boîte mais une fonction
 * appelée par les handlers des deux moitiés. Raison : une fois la décision
 * prise, le perdant est caché, le gagnant couvre toute la barre — et un
 * GtkButton réclame la séquence de clic, donc un geste posé sur la boîte
 * n'aurait jamais vu passer ces clics-là.
 *
 * « les deux » veut dire les deux DANS LE MÊME SENS : si l'une des zones
 * est encore ouverte, on referme tout ; sinon on ouvre tout. */
static void
choice_band_toggle(Ibox *b)
{
    gboolean open = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b->in_fold))
                    || (b->out_fold != NULL
                        && gtk_toggle_button_get_active(
                               GTK_TOGGLE_BUTTON(b->out_fold)));

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->in_fold), !open);
    if (b->out_fold != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->out_fold), !open);
}

/* ----- choix -------------------------------------------------------- */

/* Le mot que dit le gagnant. Par défaut le fait d'Éric (« exécuté »
 * / « refusé ») ; un libellé imposé ne nomme que l'issue OUI — le refus a
 * déjà son mot, écrit une seule fois ici. Un setter de libellé ne doit
 * jamais renommer l'issue qu'il ne décrit pas (ASK+ l'a révélé). */
static const char *
choice_caption(Ibox *b, IboxChoice winner)
{
    if (winner == IB_CHOICE_YES && b->choice_text != NULL)
        return b->choice_text;
    return winner == IB_CHOICE_YES ? _("✔ executed") : _("✖ refused");
}

/* État final de la barre : le gagnant occupe 100 % de la largeur et porte
 * le verdict, le perdant n'existe plus. Atteinte en bout de course par un
 * clic, et directement par un ALLOW — une demande acceptée d'avance n'a
 * rien à jouer, elle n'est pas une décision qui se prend sous ses yeux. */
static void
choice_set_final(Ibox *b, IboxChoice c)
{
    GtkWidget *winner = c == IB_CHOICE_YES ? b->ch_yes : b->ch_no;
    GtkWidget *loser  = c == IB_CHOICE_YES ? b->ch_no  : b->ch_yes;

    gtk_widget_set_size_request(winner, -1, -1);
    gtk_widget_set_size_request(loser, -1, -1);
    gtk_widget_set_visible(loser, FALSE);
    /* Le gagnant ne dit plus « Exécuter » (ce qu'on lui a demandé) mais le
     * verdict (ce qui s'est passé) — maintenant seulement qu'il a toute la
     * place de le dire. */
    gtk_button_set_label(GTK_BUTTON(winner), choice_caption(b, c));
    /* Le gagnant RESTE SENSIBLE, et c'est la correction d'un défaut que
     * j'avais introduit ici même. J'y avais mis set_sensitive(FALSE) pour
     * que le clic remonte à la boîte et la fasse plier — mais un widget
     * insensible, Adwaita l'assombrit, et c'est très exactement la pâleur
     * que Éric a vue sur « ✔ exécuté » comme sur « ✖ refusé » : les deux
     * seuls états où le gagnant était insensible.
     *
     * Le geste sur la boîte est supprimé en parallèle : une fois la
     * décision prise, le perdant est caché et le gagnant couvre toute la
     * barre, donc SON handler de clic suffit à plier les deux zones (voir
     * on_choice_yes). Sensible + handler = blanc intégral, sans lutter
     * contre le thème. */
}

/* 1,4 s : au milieu des 1-3 s demandées. Assez long pour que le geste se
 * lise, assez court pour ne pas retenir l'exécution — la décision, elle,
 * est prise au clic, l'animation ne fait que la montrer. */
#define IB_CHOICE_ANIM_US 1400000

typedef struct {
    Ibox    *b;
    gint64   start;
    int      lose0;      /* largeur du PERDANT au départ, en px */
    gboolean yes_won;
} ChoiceAnim;

/* Celui des deux qui se rétracte. Un seul appel pour ne jamais inverser
 * les deux par étourderie. */
static GtkWidget *
loser_of(Ibox *b, gboolean yes_won)
{
    return yes_won ? b->ch_no : b->ch_yes;
}

static gboolean
on_choice_tick(GtkWidget G_GNUC_UNUSED *widget,
               GdkFrameClock G_GNUC_UNUSED *clock, gpointer data)
{
    ChoiceAnim *a = data;
    double      t = (double)(g_get_monotonic_time() - a->start)
                    / (double)IB_CHOICE_ANIM_US;
    int         lose_w;

    if (t >= 1.0) {
        choice_set_final(a->b, a->yes_won ? IB_CHOICE_YES : IB_CHOICE_NO);
        a->b->ch_anim = 0;
        return G_SOURCE_REMOVE;
    }
    /* Une seule largeur est imposée : celle du perdant, et elle DESCEND.
     * Le gagnant ne reçoit aucun size_request — il a hexpand, donc il prend
     * le reste, mécaniquement de 50 % à 100 %.
     *
     * C'est la correction de fond, et elle rend le débordement impossible
     * au lieu de le régler. Avant : je demandais au gagnant la largeur
     * totale (700 px) ET le perdant gardait les siens (96 px mesurés), soit
     * 796 px réclamés pour 700 alloués — d'où le cadre à 110 % vu par Éric.
     * Maintenant : rien n'est imposé au gagnant, et 96 + ce que je demande
     * au perdant ne peut pas dépasser la place, puisque je ne demande au
     * perdant que ce qu'il a déjà, en moins.
     *
     * Le plancher du perdant tombe à 11 px (mesuré) grâce à ibox-eaten,
     * soit ~1,5 % d'une barre de 700 px : le « 1 % puis disparition ». */
    lose_w = (int)(a->lose0 * (1.0 - t) + 0.5);
    gtk_widget_set_size_request(loser_of(a->b, a->yes_won), lose_w, -1);
    return G_SOURCE_CONTINUE;
}

static void
apply_choice(Ibox *b, IboxChoice c, gboolean animate)
{
    GtkWidget *loser = loser_of(b, c == IB_CHOICE_YES);
    int        lose0;

    if (b->choice == c || c == IB_CHOICE_NONE)
        return;                       /* une décision ne se dé-tranche pas */
    b->choice = c;

    gtk_widget_remove_css_class(b->ch_box, "ibox-ask");
    gtk_widget_add_css_class(b->ch_box, "ibox-resolved");

    /* On part de la largeur RÉELLE du perdant, pas de celle de la barre :
     * la boîte peut changer de taille pendant l'animation (resolve() replie
     * input et output, un ascenseur peut apparaître). Une valeur capturée
     * sur la barre serait fausse en cours de route ; celle du perdant est
     * le point de départ exact de son propre mouvement. */
    lose0 = animate ? gtk_widget_get_width(loser) : 0;
    if (lose0 <= 0) {        /* jamais alloué : rien d'animable */
        choice_set_final(b, c);
        return;
    }
    /* Les deux leviers qui rendent la descente possible, tous deux mesurés
     * (sondage /tmp/probe2 + /tmp/probe3) :
     *
     *   hexpand FALSE  — sinon le perdant absorbe la moitié du surplus et
     *                    ne se contracte jamais, quelle que soit la largeur
     *                    qu'on lui demande ;
     *   ibox-eaten     — remet padding horizontal et min-width à zéro. Sans
     *                    ça, son plancher reste à 50 px (7 %) au lieu de
     *                    11 px (1,5 %). Le padding VERTICAL survit : la
     *                    hauteur de la barre ne change pas, le fil ne saute
     *                    pas. */
    gtk_widget_set_hexpand(loser, FALSE);
    gtk_widget_add_css_class(loser, "ibox-eaten");
    {
        ChoiceAnim *a = g_new0(ChoiceAnim, 1);

        a->b       = b;
        a->start   = g_get_monotonic_time();
        a->lose0   = lose0;
        a->yes_won = (c == IB_CHOICE_YES);
        /* Le tick appartient à ch_box : il meurt avec elle, et g_free
         * libère l'accumulateur — aucun pointeur à garder ailleurs. */
        b->ch_anim = gtk_widget_add_tick_callback(b->ch_box, on_choice_tick,
                                                  a, g_free);
    }
}

static void
on_choice_yes(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    Ibox *b = data;

    if (b->choice != IB_CHOICE_NONE) {
        /* Ce n'est plus une option mais une bande : le gagnant occupe
         * toute la largeur, donc c'est ici que se joue le « clic sur la
         * zone choix plie input ET output ». Le garde sur ch_anim évite
         * de plier PENDANT que la barre est encore en train de manger le
         * perdant. */
        if (b->ch_anim == 0)
            choice_band_toggle(b);
        return;
    }
    apply_choice(b, IB_CHOICE_YES, TRUE);
    if (b->cb_choice != NULL)
        b->cb_choice(b->self, IB_CHOICE_YES, b->ud_choice);
}

static void
on_choice_no(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    Ibox *b = data;

    if (b->choice != IB_CHOICE_NONE) {
        if (b->ch_anim == 0)
            choice_band_toggle(b);
        return;
    }
    apply_choice(b, IB_CHOICE_NO, TRUE);
    if (b->cb_choice != NULL)
        b->cb_choice(b->self, IB_CHOICE_NO, b->ud_choice);
}

static void
on_show_all_clicked(GtkButton G_GNUC_UNUSED *btn, gpointer data)
{
    Ibox *b = data;

    if (b->cb_showall != NULL && b->output != NULL)
        b->cb_showall(b->self, b->output, b->ud_showall);
}

/* ----- construction ------------------------------------------------- */

static void
ibox_priv_free(gpointer data)
{
    Ibox *b = data;

    g_free(b->input);
    g_free(b->output);
    g_free(b->choice_text);
    g_free(b);
}

GtkWidget *
ibox_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    Ibox      *b   = g_new0(Ibox, 1);
    GtkWidget *head;

    b->self   = box;
    b->choice = IB_CHOICE_NONE;

    /* --- 1. input : noir sur gris pâle, pliable --- */
    b->in_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(b->in_box, "ibox-in");
    /* digest_out : comme l'output, l'input affiche un résumé dans son
     * entête — c'est ce qui reste lisible quand la zone est repliée. */
    head = zone_head("input", &b->in_fold, &b->in_digest);
    gtk_box_append(GTK_BOX(b->in_box), head);
    b->in_scroll = pane_new(&b->in_buf);
    gtk_widget_set_size_request(b->in_scroll, -1, IB_LINE_PX);
    b->in_revealer = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(b->in_revealer), b->in_scroll);
    gtk_revealer_set_reveal_child(GTK_REVEALER(b->in_revealer), TRUE);
    gtk_box_append(GTK_BOX(b->in_box), b->in_revealer);
    g_object_bind_property(b->in_fold, "active",
                           b->in_revealer, "reveal-child",
                           G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);
    gtk_box_append(GTK_BOX(box), b->in_box);

    /* --- 2. choix : deux moitiés jointives, pas de boutons flottants ---
     * La barre EST les deux options. Une GtkBox horizontale sans
     * espacement, deux enfants hexpand : chacun fait exactement 50 %.
     * Un GtkButton aplati par la CSS (classe ibox-half) reste le bon
     * widget — clic natif, focus clavier, et surtout le texte du gagnant
     * devient le verdict sans qu'un troisième widget ait à exister
     * (l'ancien ch_lbl est mort avec ch_row). */
    b->ch_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(b->ch_box, "ibox-ask");
    b->ch_yes = gtk_button_new_with_label(_("Run"));
    b->ch_no  = gtk_button_new_with_label(_("Refuse"));
    gtk_widget_add_css_class(b->ch_yes, "ibox-half");
    gtk_widget_add_css_class(b->ch_no,  "ibox-half");
    gtk_widget_add_css_class(b->ch_yes, "ibox-yes");
    gtk_widget_add_css_class(b->ch_no,  "ibox-no");
    gtk_widget_set_hexpand(b->ch_yes, TRUE);
    gtk_widget_set_hexpand(b->ch_no, TRUE);
    {
        GtkWidget *ly = gtk_button_get_child(GTK_BUTTON(b->ch_yes));
        GtkWidget *ln = gtk_button_get_child(GTK_BUTTON(b->ch_no));

        /* ellipsize : un GtkButton ne descend pas sous la largeur de son
         * texte. Sans lui, le perdant se braque et le gagnant n'atteint
         * jamais 100 %.
         *
         * max-width-chars = 1, et surtout PAS width-chars. Mesuré dans
         * /tmp/probe2 sur ce cas précis, avec la naturelle du perdant :
         *
         *     ellipsize seul ...................... 96 px
         *     ellipsize + width-chars=1 ........... 96 px   <- aucun effet
         *     ellipsize + max-width-chars=1 ....... 50 px   <- le bon levier
         *
         * J'avais mis width-chars au tour précédent, en croyant qu'il
         * écrasait la naturelle : c'était un no-op, et c'est pourquoi Éric
         * n'a « pas bien vu de changement ». width-chars exprime une largeur
         * SOUHAITÉE ; c'est max-width-chars qui plafonne la naturelle. Le
         * plancher de 50 px restant est du CSS d'Adwaita (padding +
         * min-width), annulé par ibox-eaten — mesuré à 11 px dans
         * /tmp/probe3. */
        gtk_label_set_ellipsize(GTK_LABEL(ly), PANGO_ELLIPSIZE_END);
        gtk_label_set_ellipsize(GTK_LABEL(ln), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(ly), 1);
        gtk_label_set_max_width_chars(GTK_LABEL(ln), 1);
    }
    g_signal_connect(b->ch_yes, "clicked", G_CALLBACK(on_choice_yes), b);
    g_signal_connect(b->ch_no, "clicked", G_CALLBACK(on_choice_no), b);
    gtk_box_append(GTK_BOX(b->ch_box), b->ch_yes);
    gtk_box_append(GTK_BOX(b->ch_box), b->ch_no);
    /* Pas de contrôleur de geste ici, et c'est un choix — pas un oubli.
     * Une fois la décision prise, le perdant est caché et le gagnant
     * occupe TOUTE la barre : son propre handler de clic suffit à plier
     * les deux zones. Ajouter un geste sur la boîte en plus aurait risqué
     * le double déclenchement — plier puis déplier dans la même seconde.
     * (Et surtout, cela m'aurait permis de laisser le gagnant SENSIBLE,
     * seul moyen d'échapper à l'assombrissement d'Adwaita : voir
     * choice_set_final.) */
    gtk_box_append(GTK_BOX(box), b->ch_box);

    /* --- 3. output : blanc sur noir, pliable, absent tant qu'inconnu --- */
    b->out_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(b->out_box, "ibox-out");
    gtk_widget_set_visible(b->out_box, FALSE);
    head = zone_head("output", &b->out_fold, &b->out_digest);
    gtk_box_append(GTK_BOX(b->out_box), head);
    {
        GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

        b->out_scroll = pane_new(&b->out_buf);
        gtk_widget_set_size_request(b->out_scroll, -1, IB_LINE_PX);
        gtk_box_append(GTK_BOX(body), b->out_scroll);
        b->out_more = gtk_button_new_with_label(_("⤢ show all"));
        gtk_widget_add_css_class(b->out_more, "ibox-more");
        gtk_widget_add_css_class(b->out_more, "flat");
        gtk_widget_set_halign(b->out_more, GTK_ALIGN_START);
        gtk_widget_set_visible(b->out_more, FALSE);
        g_signal_connect(b->out_more, "clicked",
                         G_CALLBACK(on_show_all_clicked), b);
        gtk_box_append(GTK_BOX(body), b->out_more);
        b->out_revealer = gtk_revealer_new();
        gtk_revealer_set_child(GTK_REVEALER(b->out_revealer), body);
        gtk_revealer_set_reveal_child(GTK_REVEALER(b->out_revealer), TRUE);
    }
    gtk_box_append(GTK_BOX(b->out_box), b->out_revealer);
    g_object_bind_property(b->out_fold, "active",
                           b->out_revealer, "reveal-child",
                           G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);
    gtk_box_append(GTK_BOX(box), b->out_box);

    gtk_widget_add_css_class(box, "ibox");
    g_object_set_data_full(G_OBJECT(box), "cdb-ibox", b, ibox_priv_free);
    return box;
}

/* ----- API ---------------------------------------------------------- */

void
ibox_on_choice(GtkWidget *box, IboxChosen cb, gpointer user_data)
{
    Ibox *b = ibox_of(box);

    if (b != NULL) {
        b->cb_choice = cb;
        b->ud_choice = user_data;
    }
}

void
ibox_on_show_all(GtkWidget *box, IboxShowAll cb, gpointer user_data)
{
    Ibox *b = ibox_of(box);

    if (b != NULL) {
        b->cb_showall = cb;
        b->ud_showall = user_data;
    }
}

/* Extrait de la première ligne, coupé proprement. La coupe se fait en
 * octets puis recule tant qu'elle tomberait dans la masse de continuation
 * d'un caractère UTF-8 — jamais une boîte n'affichera de caractère brisé. */
static char *
first_line_snippet(const char *s, gsize max)
{
    const char *nl  = strchr(s, '\n');
    gsize       len = (nl != NULL) ? (gsize)(nl - s) : strlen(s);
    gboolean    cut = FALSE;

    if (len > max) {
        len = max;
        cut = TRUE;
        while (len > 0 && (((const unsigned char *) s)[len] & 0xC0) == 0x80)
            len--;
    }
    if (nl != NULL)
        cut = TRUE;                       /* la demande avait plusieurs
                                           * lignes : la suite est cachée */
    if (cut) {
        char *head = g_strndup(s, len);
        char *with = g_strdup_printf("%s…", head);

        g_free(head);
        return with;
    }
    return g_strdup(s);
}

void
ibox_set_input(GtkWidget *box, const char *text)
{
    Ibox *b = ibox_of(box);

    if (b == NULL)
        return;
    g_free(b->input);
    b->input = g_strdup(text != NULL ? text : "");
    pane_fill(b->in_buf, b->in_scroll, b->input, IB_IN_MAX_H);

    /* Le digest est ce que la zone input dit d'ELLE-MÊME quand elle est
     * repliée. Sans lui, une boîte repliée ne nomme plus la demande
     * qu'elle enregistre — elle devient une barre décorative. */
    if (b->in_digest != NULL) {
        char *snip = first_line_snippet(b->input, 72);

        gtk_label_set_text(GTK_LABEL(b->in_digest), snip);
        g_free(snip);
    }
}

const char *
ibox_get_input(GtkWidget *box)
{
    Ibox *b = ibox_of(box);

    return b != NULL ? b->input : "";
}

void
ibox_set_output(GtkWidget *box, const char *text)
{
    Ibox  *b = ibox_of(box);
    gsize  n;

    if (b == NULL)
        return;
    g_free(b->output);
    b->output = g_strdup(text != NULL ? text : "");
    n = count_lines(b->output);

    if (n == 0) {                     /* pas encore de réponse : effacée */
        gtk_widget_set_visible(b->out_box, FALSE);
        gtk_label_set_text(GTK_LABEL(b->out_digest), "");
        gtk_widget_set_visible(b->out_more, FALSE);
        return;
    }

    gtk_widget_set_visible(b->out_box, TRUE);
    {
        /* « 1 ligne » et « 2 lignes » : le pluriel vient de ngettext, pas
         * d'un "s" plaqué sur le nombre. Le digest se lit comme une phrase. */
        char *d = g_strdup_printf(ngettext("%lu line", "%lu lines",
                                           (gulong)n), (gulong)n);

        gtk_label_set_text(GTK_LABEL(b->out_digest), d);
        g_free(d);
    }
    pane_fill(b->out_buf, b->out_scroll, b->output, IB_OUT_MAX_H);

    /* ⤢ n'a de sens que si le contenu déborde la hauteur affichée. */
    gtk_widget_set_visible(b->out_more,
                           b->cb_showall != NULL &&
                           (int)n * IB_LINE_PX > IB_OUT_MAX_H);

    /* Dépliage automatique, mais seulement tant que RIEN N'EST DÉCIDÉ :
     * une demande en attente doit se lire — on déplie ce qu'on approuve.
     * Dès qu'une décision est tombée (clic d'Éric ou accord d'avance),
     * ibox_decided() a replié la boîte et le résultat qui arrive ne doit
     * plus la rouvrir toute seule. La hauteur reste bornée, donc un
     * output de 30 000 lignes ne coûte ni une seconde de layout, ni
     * 600 000 pixels de fil. */
    if (!b->decided)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->out_fold),
                                     TRUE);
}

const char *
ibox_get_output(GtkWidget *box)
{
    Ibox *b = ibox_of(box);

    return b != NULL ? b->output : "";
}

/* Tranche la barre de choix.
 *   animate = TRUE  → le gagnant mange le perdant sous les yeux d'Éric.
 *   animate = FALSE → état final posé direct : c'est le cas d'un ALLOW,
 *     né déjà tranché. L'animer dirait qu'une décision se prend à l'instant,
 *     alors qu'elle a été accordée d'avance. */
void
ibox_set_choice(GtkWidget *box, IboxChoice choice, gboolean animate)
{
    Ibox *b = ibox_of(box);

    if (b != NULL)
        apply_choice(b, choice, animate);
}

/* Libellé de l'issue OUI, à poser AVANT ibox_set_choice(). Deux usages,
 * un seul sens : la demande acceptée d'avance (ALLOW / ALLOW+) écrit
 * « autorisé » — personne n'a cliqué, « exécuté » serait un mensonge —
 * et ASK+ écrit « exécuté + » pour que le clic porte l'effet propre.
 * Aucun des deux n'atteint le rouge : « refused » est écrit plus haut. */
void
ibox_set_choice_label(GtkWidget *box, const char *text)
{
    Ibox *b = ibox_of(box);

    if (b == NULL)
        return;
    g_free(b->choice_text);
    b->choice_text = (text != NULL && text[0] != '\0')
                         ? g_strdup(text) : NULL;
    /* Déjà résolue ? On recharge le libellé du gagnant sur place. */
    if (b->choice != IB_CHOICE_NONE)
        gtk_button_set_label(GTK_BUTTON(b->choice == IB_CHOICE_YES
                                            ? b->ch_yes : b->ch_no),
                             choice_caption(b, b->choice));
}
IboxChoice
ibox_get_choice(GtkWidget *box)
{
    Ibox *b = ibox_of(box);

    return b != NULL ? b->choice : IB_CHOICE_NONE;
}

void
ibox_set_expanded(GtkWidget *box, gboolean input_open, gboolean output_open)
{
    Ibox *b = ibox_of(box);

    if (b == NULL)
        return;
    if (b->in_fold != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->in_fold),
                                     input_open);
    if (b->out_fold != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->out_fold),
                                     output_open);
}

void
ibox_get_expanded(GtkWidget *box, gboolean *input_open, gboolean *output_open)
{
    Ibox *b = ibox_of(box);

    if (b == NULL)
        return;
    if (input_open != NULL)
        *input_open = b->in_fold != NULL
                          ? gtk_toggle_button_get_active(
                                GTK_TOGGLE_BUTTON(b->in_fold))
                          : TRUE;
    if (output_open != NULL)
        *output_open = b->out_fold != NULL
                           ? gtk_toggle_button_get_active(
                                 GTK_TOGGLE_BUTTON(b->out_fold))
                           : FALSE;
}

/* Une décision est tombée — clic d'Éric, refus, annulation, ou accord
 * d'avance (ALLOW / ALLOW+) : peu importe laquelle. La boîte se replie sur
 * sa zone choix, qui elle reste toujours affichée en entier, et le pliage
 * est épinglé : le résultat qui arrivera ensuite ne la rouvrira pas.
 *
 * C'est la règle d'Éric : une fois la décision prise, la demande a cessé
 * d'être l'actualité du fil ; elle devient de l'historique, et le fil doit
 * pouvoir rester lisible. */
void
ibox_decided(GtkWidget *box)
{
    Ibox *b = ibox_of(box);

    if (b == NULL)
        return;
    b->decided = TRUE;
    if (b->in_fold != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->in_fold), FALSE);
    if (b->out_fold != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->out_fold), FALSE);
}
