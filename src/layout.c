/*
 * Layout : modèle récursif du tiling de tuiles, persisté en JSON.
 */

#include "layout.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define SIEB_CONFIG_DIR "siebcodedashboard"
#define SIEB_LAYOUT_FILE "layout.json"

#define DEFAULT_FRACTION 0.25

static char *
layout_config_path(void)
{
    const char *dir = g_get_user_config_dir();

    return g_build_filename(dir, SIEB_CONFIG_DIR, SIEB_LAYOUT_FILE, NULL);
}

/* ------------------------------------------------ */
/* Construction / destruction                        */
/* ------------------------------------------------ */

Layout *
layout_tile(const char *id)
{
    Layout *n = g_new0(Layout, 1);

    n->kind = LAYOUT_TILE;
    /* Une tuile a TOUJOURS un id : NULL serait un crash au rendu
     * (create_piece) et une écriture "id":null dans le JSON. */
    n->id = g_strdup(id != NULL ? id : "empty");
    n->fraction = 0.5;
    return n;
}

static Layout *
layout_split_node(Layout *a, Layout *b, gboolean horizontal)
{
    Layout *n = g_new0(Layout, 1);

    n->kind = horizontal ? LAYOUT_HSPLIT : LAYOUT_VSPLIT;
    /* Split utilisateur : deux moitiés (le boot à ~1/4 est réglé à part
     * dans layout_load). Les fractions s'ajustent ensuite au drag. */
    n->fraction = 0.5;
    n->a = a;
    n->b = b;
    a->parent = n;
    b->parent = n;
    return n;
}

void
layout_free(Layout *node)
{
    if (node == NULL)
        return;
    layout_free(node->a);
    layout_free(node->b);
    g_free(node->id);
    g_free(node);
}

/* ------------------------------------------------ */
/* Sérialisation                                     */
/* ------------------------------------------------ */

static JsonNode *
layout_to_json(Layout *node)
{
    JsonBuilder *builder;
    JsonNode    *out;
    gchar       *kind;

    builder = json_builder_new();
    json_builder_begin_object(builder);

    if (node->kind == LAYOUT_TILE) {
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "tile");
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, node->id);
    } else {
        kind = node->kind == LAYOUT_HSPLIT ? "h" : "v";
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, kind);
        json_builder_set_member_name(builder, "frac");
        json_builder_add_double_value(builder, node->fraction);

        json_builder_set_member_name(builder, "a");
        json_builder_add_value(builder, layout_to_json(node->a));
        json_builder_set_member_name(builder, "b");
        json_builder_add_value(builder, layout_to_json(node->b));
    }

    json_builder_end_object(builder);
    out = json_builder_get_root(builder);
    g_object_unref(builder);
    return out;
}

void
layout_save(Layout *root)
{
    JsonBuilder *builder;
    JsonNode    *root_node;
    gchar       *text;
    GError      *error = NULL;

    if (root == NULL)
        return;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "root");
    json_builder_add_value(builder, layout_to_json(root));
    json_builder_end_object(builder);

    root_node = json_builder_get_root(builder);
    text = json_to_string(root_node, TRUE);
    if (!g_file_set_contents(layout_config_path(), text, -1, &error)) {
        g_printerr("SIEB - CodeDashBoard: écriture layout.json : %s\n",
                   error->message);
        g_error_free(error);
    }
    g_free(text);
    json_node_unref(root_node);
    g_object_unref(builder);
}

/* ------------------------------------------------ */
/* Désérialisation                                   */
/* ------------------------------------------------ */

static Layout *
layout_from_json(JsonObject *obj)
{
    const char *type = json_object_get_string_member(obj, "type");
    Layout     *n;

    if (type == NULL)
        return NULL;

    if (strcmp(type, "tile") == 0) {
        const char *id = json_object_get_string_member(obj, "id");

        if (id == NULL)
            return NULL;
        return layout_tile(id);
    }

    /* Split. */
    n = g_new0(Layout, 1);
    n->kind = strcmp(type, "h") == 0 ? LAYOUT_HSPLIT : LAYOUT_VSPLIT;
    if (json_object_has_member(obj, "frac"))
        n->fraction = json_object_get_double_member(obj, "frac");
    else
        n->fraction = DEFAULT_FRACTION;

    if (json_object_has_member(obj, "a"))
        n->a = layout_from_json(json_object_get_object_member(obj, "a"));
    if (json_object_has_member(obj, "b"))
        n->b = layout_from_json(json_object_get_object_member(obj, "b"));
    if (n->a != NULL)
        n->a->parent = n;
    if (n->b != NULL)
        n->b->parent = n;

    /* Split sans les deux fils : invalide. */
    if (n->a == NULL || n->b == NULL) {
        layout_free(n);
        return NULL;
    }
    return n;
}

Layout *
layout_load(void)
{
    JsonParser *parser;
    JsonNode   *root;
    JsonObject *obj;
    Layout     *out = NULL;
    GError     *error = NULL;
    char       *path = layout_config_path();

    parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_printerr("SIEB - CodeDashBoard: layout.json : %s\n", error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_free(path);
        goto default_layout;
    }
    g_free(path);

    root = json_parser_get_root(parser);
    if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
        obj = json_object_get_object_member(json_node_get_object(root), "root");
        if (obj != NULL)
            out = layout_from_json(obj);
    }
    g_object_unref(parser);

    if (out == NULL)
        goto default_layout;
    return out;

default_layout:
    /* Explorateur | Éditeur, fraction ~1/4. */
    {
        Layout *l = layout_split_node(layout_tile("explorer"),
                                      layout_tile("editor"), TRUE);

        l->fraction = 0.25;
        return l;
    }
}

/* ------------------------------------------------ */
/* Opérations                                        */
/* ------------------------------------------------ */

static void
replace_child(Layout *parent, Layout *old, Layout *new)
{
    if (parent->a == old)
        parent->a = new;
    else if (parent->b == old)
        parent->b = new;
    new->parent = parent;
}

Layout *
layout_split(Layout *root, Layout *node, gboolean horizontal, const char *new_id)
{
    Layout *new_tile = layout_tile(new_id);
    Layout *split;
    Layout *old_parent;

    if (node == NULL || new_tile == NULL)
        return root;

    /* Récupère l'ancien parent AVANT layout_split_node : celui-ci réécrit
     * node->parent (le nœud devient le fils a du nouveau split). */
    old_parent = node->parent;

    split = layout_split_node(node, new_tile, horizontal);

    if (old_parent == NULL) {
        /* node était le root : le split devient la nouvelle racine. */
        return split;
    }
    replace_child(old_parent, node, split);
    return root;
}

Layout *
layout_remove(Layout *root, Layout *node)
{
    Layout *parent;
    Layout *sibling;

    if (node == NULL || node->parent == NULL)
        return root; /* seul tile : on ne retire pas la dernière tuile */

    parent = node->parent;
    sibling = (parent->a == node) ? parent->b : parent->a;
    sibling->parent = NULL;

    /* Détache la soeur du parent : layout_free(parent) ne doit pas la
     * libérer (elle est conservée). */
    if (parent->a == node)
        parent->b = NULL;
    else
        parent->a = NULL;

    if (parent->parent == NULL) {
        /* Le parent est le root : la soeur devient la nouvelle racine. */
        layout_free(parent); /* libère parent + node uniquement */
        return sibling;
    }

    replace_child(parent->parent, parent, sibling);
    layout_free(parent);
    return root;
}

void
layout_retile(Layout *node, const char *new_id)
{
    if (node == NULL || new_id == NULL)
        return;
    /* Un bloc (sous-arbre) devient une tuile unique : sa structure est
     * libérée (le « Changer » du menu d'un bloc le réduit en pièce). */
    layout_free(node->a);
    layout_free(node->b);
    node->a = NULL;
    node->b = NULL;
    node->kind = LAYOUT_TILE;
    g_free(node->id);
    node->id = g_strdup(new_id);
}

/* ------------------------------------------------ */
/* Noms                                              */
/* ------------------------------------------------ */

const char *
layout_name(const char *id)
{
    if (id == NULL)
        return "?";
    if (strcmp(id, "editor") == 0)
        return "Éditeur";
    if (strcmp(id, "explorer") == 0)
        return "Explorateur";
    if (strcmp(id, "bash") == 0)
        return "Bash";
    if (strcmp(id, "empty") == 0)
        return "Vide";
    return id;
}