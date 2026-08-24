/*
 * css.c : CSS applicative centralisée de CDB.
 *
 * Toute la partie style non fournie par le thème GTK/libadwaita vit ici.
 * main.c appelle cdb_css_install() au démarrage.
 */

#include "css.h"

static const char *cdb_css =
            ".tile-title { font-size: 10pt; }\n"
            /* Bouton « Configurer… » du sélecteur LLM : même fine
             * print que le reste — le style bouton par défaut rend le
             * label en gras et casse la hiérarchie 10 px. */
            ".initprompt-editor text { font-family: monospace; font-size: 10pt; }\n"
            "button.llm-configure { font-size: 10pt; "
            "font-weight: normal; padding: 2px 6px; }\n"
            "menubutton.tile-menu > button { font-size: 9pt; "
            "padding: 0 4px; min-height: 0; }\n"
            /* Barre de composition LLM : bloc plein légèrement plus sombre
             * que la tuile ; la saisie y est transparente (fond du bloc). */
            ".llm-compose { background: alpha(shade(@view_bg_color, 0.92), 1); "
            "border: 1px solid @borders; border-radius: 6px; }\n"
            ".llm-compose-entry, .llm-compose-entry text "
            "{ background: none; }\n"
            ".llm-compose-entry { padding: 4px 8px; }\n"
            ".llm-compose-send { padding: 2px 8px; min-height: 0; }\n"
            /* Point orange d'onglet bash : commande /CDB:: en cours. */
            ".cdb-busy-dot { color: orange; font-size: 8pt; }\n"
            /* Sélecteur de modèle : label phrasique discret — pas de fond,
             * pas de bordure, 10 pt, chevron atténué (style « gracile »). */
            "menubutton.llm-model-btn > button { background: none; "
            "border: none; box-shadow: none; min-height: 0; padding: 0 2px; "
            "font-size: 10pt; font-weight: normal; }\n"
            "menubutton.llm-model-btn > button:hover:not(:checked) "
            "{ background: none; }\n"
            "menubutton.llm-model-btn > button > box > label "
            "{ opacity: 0.85; }\n"
            /* Popover du sélecteur : flat/square comme le thème — fond
             * uni, coins droits, bordure fine ; rangées transparentes. */
            "popover.llm-model-pop > contents "
            "{ border-radius: 0; background: @view_bg_color; }\n"
            "popover.llm-model-pop listbox > row "
            "{ background: none; border-radius: 0; }\n"
            "popover.llm-model-pop listbox > row:hover "
            "{ background: alpha(@theme_fg_color, 0.06); }\n"
            /* Popover du menu slots : même langage visuel que le sélecteur. */
            "popover.llm-slots-pop > contents "
            "{ border-radius: 0; background: @view_bg_color; }\n"
            "popover.llm-slots-pop button "
            "{ background: none; border: none; box-shadow: none; "
            "border-radius: 0; padding: 6px 10px; min-height: 0; "
            "font-size: 10pt; font-weight: normal; }\n"
            "popover.llm-slots-pop button:hover "
            "{ background: alpha(@theme_fg_color, 0.06); }\n"
            /* Titlebar : teintes uniformes, tout en 10 pt non gras. */
            "headerbar { font-size: 10pt; font-weight: normal; }\n"
            "headerbar .title { font-weight: normal; font-size: 10pt; }\n"
            ".titlebar-brand { padding: 0; font-size: 10pt; "
            "font-weight: normal; }\n"
            "menubutton.titlebar-brand > button { background: none; "
            "box-shadow: none; min-height: 0; min-width: 0; padding: 0 2px; "
            "margin: 0; border: none; }\n"
            "menubutton.titlebar-brand > button:hover:not(:checked) "
            "{ background: none; }\n"
            ".titlebar-sep { padding: 0 4px; font-size: 10pt; }\n"
            ".titlebar-signature { font-size: 10pt; font-weight: normal; }\n"
            ".titlebar-file { font-weight: normal; font-size: 10pt; }\n"
            "headerbar { min-height: 0; padding: 0 8px; }\n"
            "headerbar > box { min-height: 0; }\n"
            "headerbar button { min-height: 0; min-width: 0; padding: 0 6px; "
            "margin: 0; }\n"
            "headerbar button > image { min-height: 0; min-width: 0; "
            "-gtk-icon-size: 12px; }\n"
            "headerbar windowcontrols { min-height: 0; }\n"
            "headerbar windowcontrols > button { min-height: 0; "
            "min-width: 0; padding: 0 4px; margin: 0; border: none; "
            "border-radius: 0; }\n"
            "headerbar windowcontrols > button > image { min-height: 12px; "
            "min-width: 12px; -gtk-icon-size: 12px; }\n";

void
cdb_css_install(GdkDisplay *display)
{
    GtkCssProvider *css = gtk_css_provider_new();

    if (display == NULL)
        display = gdk_display_get_default();

    gtk_css_provider_load_from_string(css, cdb_css);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}
