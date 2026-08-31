/*
 * css.c : CSS applicative centralisée de CDB.
 *
 * Toute la partie style non fournie par le thème GTK/libadwaita vit ici.
 * main.c appelle cdb_css_install() au démarrage.
 */

#include "css.h"

static const char *cdb_css =
    /* Tokens des boîtes interactives. Définis ici, AVANT tout usage :
     * GTK résout les noms colorés à la parse, dans l'ordre. Un seul
     * endroit où tunner la palette (decision d'Éric : couleurs absolues,
     * identiques en thème clair et en thème sombre). */
    "@define-color ibox_in_bg     #c8c8c8;\n"
    "@define-color ibox_in_fg     #000000;\n"
    "@define-color ibox_ask_bg    #c8c8c8;\n"
    "@define-color ibox_choice_fg #ffffff;\n"
    "@define-color ibox_yes_bg    #2e7d32;\n"
    "@define-color ibox_yes_hi    #3d9142;\n"
    "@define-color ibox_no_bg     #b3261e;\n"
    "@define-color ibox_no_hi     #cf3a30;\n"
    "@define-color ibox_out_bg    #101010;\n"
    "@define-color ibox_out_fg    #ffffff;\n"
    "@define-color ibox_out_sel   #3a3f4b;\n"
    "@define-color ibox_border    #7a7a7a;\n"
    /* --- Base générale --- */
    ".tile-title { font-size: 10pt; }\n"
    ".initprompt-editor text { font-family: monospace; font-size: 10pt; }\n"
    "button.llm-configure { font-size: 10pt; font-weight: normal; padding: 2px 6px; }\n"
    "menubutton.tile-menu > button { font-size: 9pt; padding: 0 4px; min-height: 0; }\n"
    ".cdb-busy-dot { color: orange; font-size: 8pt; }\n"

    /* --- LLM : barre de composition --- */
    ".llm-status { padding: 2px 8px 0 8px; }\n"
    ".llm-status label { font-size: 10pt; font-weight: normal; "
    "opacity: 0.72; }\n"
    ".llm-status image { opacity: 0.72; }\n"
    ".llm-status-logo { font-size: 10pt; opacity: 0.8; }\n"
    ".llm-credits { font-variant-numeric: tabular-nums; opacity: 0.88; }\n"
    ".llm-compose { background: alpha(shade(@view_bg_color, 0.92), 1); "
    "border: 1px solid @borders; border-radius: 6px; }\n"
    ".llm-compose-entry, .llm-compose-entry text { background: none; }\n"
    ".llm-compose-entry { padding: 4px 8px; }\n"
    ".llm-compose-send { padding: 2px 8px; min-height: 0; }\n"

    /* --- LLM : bouton sélecteur de modèle --- */
    "menubutton.llm-model-btn > button { background: none; border: none; "
    "box-shadow: none; min-height: 0; padding: 0 2px; font-size: 10pt; "
    "font-weight: normal; }\n"
    "menubutton.llm-model-btn > button:hover:not(:checked) { background: none; }\n"
    "menubutton.llm-model-btn > button > box > label { opacity: 0.85; }\n"

    /* --- Titlebar --- */
    "headerbar { font-size: 10pt; font-weight: normal; min-height: 0; padding: 0 8px; }\n"
    "headerbar .title { font-weight: normal; font-size: 10pt; }\n"
    "headerbar > box { min-height: 0; }\n"
    "headerbar button { min-height: 0; min-width: 0; padding: 0 6px; margin: 0; }\n"
    "headerbar button > image { min-height: 0; min-width: 0; -gtk-icon-size: 12px; }\n"
    "headerbar windowcontrols { min-height: 0; }\n"
    "headerbar windowcontrols > button { min-height: 0; min-width: 0; "
    "padding: 0 4px; margin: 0; border: none; border-radius: 0; }\n"
    "headerbar windowcontrols > button > image { min-height: 12px; "
    "min-width: 12px; -gtk-icon-size: 12px; }\n"
    ".titlebar-brand { padding: 0; font-size: 10pt; font-weight: normal; }\n"
    "menubutton.titlebar-brand > button { background: none; box-shadow: none; "
    "min-height: 0; min-width: 0; padding: 0 2px; margin: 0; border: none; }\n"
    "menubutton.titlebar-brand > button:hover:not(:checked) { background: none; }\n"
    ".titlebar-sep { padding: 0 4px; font-size: 10pt; }\n"
    ".titlebar-signature { font-size: 10pt; font-weight: normal; }\n"
    ".titlebar-file { font-weight: normal; font-size: 10pt; }\n"
    ".titlebar-session { font-weight: normal; font-size: 10pt; }\n"
    /* Le numéro de session se met en gras quand la boucle agentique tient —
     * la classe est posée/retirée par llm_busy_set, du même coup d'œil que
     * l'icône play/pause, donc titre et bouton ne peuvent pas diverger. */
    ".titlebar-session.cdb-busy { font-weight: bold; }\n"

    /* --- Boutons icônes vraiment flat --- */
    "button.cdb-flat, menubutton.cdb-flat > button { background: none; "
    "border: none; box-shadow: none; }\n"
    "button.cdb-flat:hover, button.cdb-flat:active, button.cdb-flat:checked, "
    "button.cdb-flat:disabled, menubutton.cdb-flat > button:hover, "
    "menubutton.cdb-flat > button:active, menubutton.cdb-flat > button:checked, "
    "menubutton.cdb-flat > button:disabled { background: none; border: none; "
    "box-shadow: none; }\n"

    /* --- Popovers CDB unifiés --- */
    "popover.cdb-pop > contents { border-radius: 0; background: @view_bg_color; "
    "padding: 4px 0; }\n"
    "popover.cdb-pop button.cdb-pop-item { background: none; border: none; "
    "box-shadow: none; border-radius: 0; padding: 6px 10px; min-height: 0; "
    "font-size: 10pt; font-weight: normal; }\n"
    "popover.cdb-pop modelbutton { background: none; border: none; "
    "box-shadow: none; border-radius: 0; padding: 6px 10px; min-height: 0; "
    "font-size: 10pt; font-weight: normal; }\n"
    "popover.cdb-pop button.cdb-pop-item:hover { background: alpha(@theme_fg_color, 0.06); }\n"
    "popover.cdb-pop modelbutton:hover { background: alpha(@theme_fg_color, 0.06); }\n"
    "popover.cdb-pop button.cdb-pop-item label { font-size: 10pt; "
    "font-weight: normal; opacity: 0.9; }\n"
    "popover.cdb-pop modelbutton label { font-size: 10pt; "
    "font-weight: normal; opacity: 0.9; }\n"
    "popover.cdb-pop label.cdb-pop-title { font-size: 9pt; font-weight: normal; "
    "opacity: 0.65; padding: 4px 10px 2px 10px; }\n"
    "popover.cdb-pop separator.cdb-pop-sep { background: @borders; "
    "margin: 4px 0; min-height: 1px; }\n"
    "popover.cdb-pop listbox { background: none; }\n"
    "popover.cdb-pop listbox > row { background: none; border-radius: 0; "
    "padding: 6px 10px; }\n"
    "popover.cdb-pop listbox > row:hover { background: alpha(@theme_fg_color, 0.06); }\n"

    /* --- Sélecteur de modèle : précisions --- */
    "popover.llm-model-pop searchentry { margin: 4px 8px; }\n"
    "popover.llm-model-pop scrolledwindow { margin: 0; }\n"
    /* --- Boîtes interactives (ibox) ---
     * Couleurs ABSOLUES (@ibox_*, définis en tête de feuille),
     * volontairement hors thème : la boîte est un instrument, pas du
     * texte de chat — elle doit crier la même chose en clair et en
     * sombre.
     * Les zones input et output sont des GtkTextView (non éditables),
     * pas des GtkLabel : mesuré, une GtkLabel de 10 000 lignes coute
     * 15 s de layout, un GtkTextView 0,0 ms. D'ou les sélecteurs
     * `textview text` (le noeud interne qui porte le fond et le texte). */
    ".ibox { margin: 6px 0; border: 1px solid @ibox_border; "
    "border-radius: 6px; }\n"
    ".ibox-tag, .ibox-digest { font-family: monospace; font-size: 8pt; "
    "opacity: 0.55; }\n"
    /* La bande entière d'une zone pliable EST le bouton (modèle
     * GtkExpander). Aplanie de fond en comble ici : sinon Adwaita lui
     * peindrait un fond, un cadre et une hauteur minimale par-dessus la
     * couleur de sa zone. « checked » est le plus traître — nos boutons
     * sont enfoncés quand la zone est dépliée, donc presque tout le temps.
     * « color: inherit » est indispensable : sans lui les étiquettes
     * prendraient la couleur d'un bouton au lieu de celle de leur zone. */
    "button.ibox-head { background: none; border: none; box-shadow: none; "
    "margin: 0; padding: 0; min-height: 0; min-width: 0; "
    "border-radius: 0; color: inherit; cursor: pointer; }\n"
    "button.ibox-head:hover, button.ibox-head:active, "
    "button.ibox-head:checked, button.ibox-head:disabled { "
    "background: none; border: none; box-shadow: none; color: inherit; }\n"
    /* Zone 1 — input : les couleurs MEMES du fil (decision d'Eric : le
     * gris pale, meme pousse, restait trop blanc). @view_bg_color /
     * @view_fg_color sont le fond et le texte par defaut du TextView
     * historique — aucune CSS ne les recouvre, verifie — donc la zone se
     * fond exactement dans la zone « message ». En theme sombre le texte
     * du theme EST blanc ; en theme clair il est noir, et la zone reste
     * lisible au lieu de devenir du blanc sur blanc. */
    ".ibox-in { background-color: @view_bg_color; color: @view_fg_color; "
    "padding: 2px 6px; }\n"
    ".ibox-in textview, .ibox-in textview text { "
    "background-color: @view_bg_color; background-image: none; "
    "color: @view_fg_color; caret-color: @view_fg_color; }\n"
    ".ibox-in textview text selection { background-color: @ibox_border; "
    "color: @view_fg_color; }\n"
    /* Zone 2 — choix : la barre EST les deux options, deux moitiés
     * jointives de 50 %. Plus de fond gris portant des boutons flottants.
     *
     * Le texte NOIR que voyait Éric venait d'une règle que j'avais écrite
     * ici même : « .ibox-ask label { color: @ibox_in_fg } », soit noir.
     * .ibox-ask est la barre, et ce sélecteur visait TOUT label descendant
     * — donc aussi ceux des deux boutons, par-dessus leur blanc. La règle
     * est supprimée ; le blanc est désormais posé sur le bouton et sur son
     * nœud label, dans chaque état. */
    ".ibox-ask { background-color: @ibox_ask_bg; padding: 0; }\n"
    /* ibox-half : le bouton aplati qui fait une moitié de barre. Aucun
     * relief, aucun arrondi — les deux moitiés doivent se joindre sans
     * couture. Le padding vit ici et non sur .ibox-ask : c'est lui qui
     * donne la hauteur de la barre, et cette hauteur ne change jamais
     * (50/50, puis 100/0), donc le fil ne saute pas.
     *
     * Le blanc est posé sur le bouton ET sur son nœud label : un label a
     * son propre nœud CSS, et c'est lui qui affiche les glyphes.
     *
     * Aucun état :disabled ici, et c'est volontaire — le gagnant reste
     * SENSIBLE (voir choice_set_final). Je l'avais rendu insensible, et
     * Adwaita l'assombrissait : c'était la pâleur du verdict. */
    "button.ibox-half { border: none; box-shadow: none; outline: none; "
    "margin: 0; border-radius: 0; background-image: none; "
    "padding: 7px 8px; font-weight: bold; color: @ibox_choice_fg; "
    "opacity: 1; cursor: pointer; }\n"
    "button.ibox-half label { color: @ibox_choice_fg; font-weight: bold; }\n"
    /* ibox-eaten : posée sur le perdant pendant qu'il se rétracte (voir
     * apply_choice). Elle n'annule que l'horizontale — les 7px verticaux
     * survivent, donc la hauteur de la barre est invariable pendant
     * l'animation.
     *
     * Ce sont ces deux propriétés qui décident si le repas va jusqu'au bout.
     * Mesuré dans /tmp/probe3, naturelle du perdant :
     *
     *     Adwaita seul ................. 50 px   = 7  % d'une barre de 700
     *     + padding horizontal à 0 ..... 18 px
     *     + min-width: 0 ............... 11 px   = 1,5 %  <- l'objectif
     *
     * Sans cette règle, le perdant a un plancher que nul size_request ne
     * peut franchir : il s'arrêtait vers 15 % et disparaissait là, comme
     * l'a vu Éric. */
    "button.ibox-half.ibox-eaten { padding-left: 0; padding-right: 0; "
    "min-width: 0; }\n"
    "button.ibox-yes { background-color: @ibox_yes_bg; }\n"
    "button.ibox-yes:hover, button.ibox-yes:active { "
    "background-color: @ibox_yes_hi; }\n"
    "button.ibox-no { background-color: @ibox_no_bg; }\n"
    "button.ibox-no:hover, button.ibox-no:active { "
    "background-color: @ibox_no_hi; }\n"
    /* Le curseur ne dit « clique ici » que quand cliquer ici fait quelque
     * chose : .ibox-resolved n'apparaît qu'une fois la décision prise.
     * Tant que la demande est en attente, la barre reste au curseur
     * normal et seules ses deux moitiés répondent. */
    ".ibox-resolved { cursor: pointer; }\n"
    /* Zone 3 — output : texte BLANC sur NOIR, un terminal dans la boîte.
     * color est porté par le CONTENEUR, pas seulement par le texte :
     * sans lui, l'étiquette « output », le digest et les chevrons
     * hériteraient du thème — donc blanc sur noir ici, et pire, blanc
     * sur ton gris pâle dans la zone input en thème sombre. */
    ".ibox-out { background-color: @ibox_out_bg; color: @ibox_out_fg; "
    "padding: 2px 6px; }\n"
    ".ibox-out textview, .ibox-out textview text { "
    "background-color: @ibox_out_bg; background-image: none; "
    "color: @ibox_out_fg; caret-color: @ibox_out_fg; }\n"
    ".ibox-out textview text selection { background-color: @ibox_out_sel; "
    "color: @ibox_out_fg; }\n"
    "button.ibox-more { color: @ibox_out_fg; border: none; "
    "box-shadow: none; background-image: none; background-color: "
    "transparent; min-height: 0; padding: 0 4px; font-size: 9pt; }\n"
    "popover.llm-model-pop button.llm-configure { margin: 4px 8px; }\n";
void
cdb_css_install(GdkDisplay *display)
{
    GtkCssProvider *css;

    if (display == NULL)
        display = gdk_display_get_default();
    if (display == NULL)
        return;

    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, cdb_css);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}
