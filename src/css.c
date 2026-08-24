/*
 * css.c : CSS applicative centralisée de CDB.
 *
 * Toute la partie style non fournie par le thème GTK/libadwaita vit ici.
 * main.c appelle cdb_css_install() au démarrage.
 */

#include "css.h"

static const char *cdb_css =
    /* --- Base générale --- */
    ".tile-title { font-size: 10pt; }\n"
    ".initprompt-editor text { font-family: monospace; font-size: 10pt; }\n"
    "button.llm-configure { font-size: 10pt; font-weight: normal; padding: 2px 6px; }\n"
    "menubutton.tile-menu > button { font-size: 9pt; padding: 0 4px; min-height: 0; }\n"
    ".cdb-busy-dot { color: orange; font-size: 8pt; }\n"

    /* --- LLM : barre de composition --- */
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
