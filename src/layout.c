/*
 * Layout : modèle récursif du tiling de tuiles, persisté en JSON.
 */

#include "layout.h"
#include "session.h"
#include "i18n.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define CDB_LAYOUT_FILE "layout.json"

#define DEFAULT_FRACTION 0.25
static char *
layout_config_path(void)
{
    return session_config_path(CDB_LAYOUT_FILE);
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

/* Écriture « read-modify-write ». layout.json ne porte plus seulement
 * l'arbre : il porte aussi l'état de l'interface (langue, et demain thème,
 * police…). Une sauvegarde du tiling ne doit JAMAIS perdre un membre qu'elle
 * ne comprend pas — on relit le fichier, on remplace « root » seulement, on
 * réécrit. Le propriétaire d'un fichier préserve ce qu'il ne connaît pas. */
void
layout_save(Layout *root)
{
    JsonNode  *file_node = NULL;
    JsonNode  *tree;
    JsonObject *obj;
    gchar     *text;
    GError    *error = NULL;
    char      *path = layout_config_path();

    if (root == NULL) {
        g_free(path);
        return;
    }
    tree = layout_to_json(root);

    {
        JsonParser *parser = json_parser_new();

        if (json_parser_load_from_file(parser, path, NULL) &&
            json_parser_get_root(parser) != NULL &&
            JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
            file_node = json_node_copy(json_parser_get_root(parser));
        g_object_unref(parser);
    }
    if (file_node == NULL) {
        file_node = json_node_new(JSON_NODE_OBJECT);
        json_node_init_object(file_node, json_object_new());
    }
    obj = json_node_get_object(file_node);
    json_object_set_member(obj, "root", tree);   /* consomme tree */

    text = json_to_string(file_node, TRUE);
    if (!g_file_set_contents(path, text, -1, &error)) {
        g_printerr(_("CDB: failed to write layout.json: %s\n"),
                   error->message);
        g_error_free(error);
    }
    g_free(text);
    json_node_unref(file_node);
    g_free(path);
}

/* ------------------------------------------------ */
/* Désérialisation                                   */
/* ------------------------------------------------ */

static Layout *
layout_from_json(JsonObject *obj)
{
    /* has_member avant tout get (convention de CLAUDE.md) : "type" et "id"
     * manquant dans un fichier édité à la main écriraient une plainte sur le
     * stderr au lieu de retomber silencieusement sur le layout par défaut —
     * le piégement du "root" absent, juste en dessous, vient d'être corrigé
     * pour la même raison. */
    const char *type = json_object_has_member(obj, "type")
                           ? json_object_get_string_member(obj, "type")
                           : NULL;
    Layout     *n;

    if (type == NULL)
        return NULL;

    if (strcmp(type, "tile") == 0) {
        const char *id = json_object_has_member(obj, "id")
                             ? json_object_get_string_member(obj, "id")
                             : NULL;

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
            g_printerr(_("CDB: failed to read layout.json: %s\n"), error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_free(path);
        goto default_layout;
    }
    g_free(path);

    root = json_parser_get_root(parser);
    if (root != NULL && JSON_NODE_HOLDS_OBJECT(root) &&
        json_object_has_member(json_node_get_object(root), "root")) {
        /* Le garde-fou n'est pas décoratif : json_object_get_object_member
         * termine sur g_return_val_if_fail(node != NULL), donc un layout.json
         * SANS membre "root" crachait une Json-CRITICAL sur le stderr de
         * l'utilisateur. Cas réel depuis le Jalon G : choisir sa langue sur une
         * session neuve écrit un layout.json qui ne porte QUE "language", et le
         * démarrage suivant retombait ici même. (Convention de CLAUDE.md :
         * has_member avant tout get — les fils "a"/"b" la respectent déjà.) */
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
        return _("Editor");
    if (strcmp(id, "explorer") == 0)
        return _("Explorer");
    if (strcmp(id, "bash") == 0)
        return "Bash";
    if (strcmp(id, "llm") == 0)
        return "LLM";
    if (strcmp(id, "settings") == 0)
        return _("Settings");
    if (strcmp(id, "empty") == 0)
        return _("Empty");
    return id;
}
/* ------------------------------------------------ */
/* Préférences d'interface (mêmes membres que l'arbre) */
/* ------------------------------------------------ */

/* Chaîne à libérer ; NULL si absent, non-chaîne ou fichier illisible. */
char *
layout_pref_get(const char *key)
{
    char       *path = layout_config_path();
    JsonParser *parser = json_parser_new();
    char       *out = NULL;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
        JsonNode   *m = json_object_has_member(obj, key)
                            ? json_object_get_member(obj, key) : NULL;

        if (m != NULL && JSON_NODE_HOLDS_VALUE(m) &&
            json_node_get_value_type(m) == G_TYPE_STRING) {
            const char *v = json_object_get_string_member(obj, key);

            if (v != NULL && v[0] != '\0')
                out = g_strdup(v);
        }
    }
    g_object_unref(parser);
    g_free(path);
    return out;
}

/* Lit le fichier, remplace seulement « key », réécrit. value NULL retire la
 * clé. Ne connaît pas les autres membres et n'y touche pas — même règle que
 * layout_save. */
void
layout_pref_set(const char *key, const char *value)
{
    char       *path = layout_config_path();
    JsonParser *parser = json_parser_new();
    JsonNode   *file_node = NULL;
    JsonObject *obj;
    GError     *error = NULL;
    gchar      *text;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        file_node = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser);
    if (file_node == NULL) {
        file_node = json_node_new(JSON_NODE_OBJECT);
        json_node_init_object(file_node, json_object_new());
    }
    obj = json_node_get_object(file_node);
    if (value != NULL)
        json_object_set_string_member(obj, key, value);
    else
        json_object_remove_member(obj, key);

    text = json_to_string(file_node, TRUE);
    if (!g_file_set_contents(path, text, -1, &error)) {
        g_printerr(_("CDB: failed to write layout.json: %s\n"), error->message);
        g_error_free(error);
    }
    g_free(text);
    json_node_unref(file_node);
    g_free(path);
}

/* Copie du membre (à libérer par l'appelant) ; NULL si absent. La copie est
 * ce qui rend l'API sûre : le nœud du parser meurt avec lui. */
JsonNode *
layout_pref_get_node(const char *key)
{
    char       *path = layout_config_path();
    JsonParser *parser = json_parser_new();
    JsonNode   *out = NULL;

    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *obj = json_node_get_object(json_parser_get_root(parser));

        if (json_object_has_member(obj, key))
            out = json_node_copy(json_object_get_member(obj, key));
    }
    g_object_unref(parser);
    g_free(path);
    return out;
}

/* Fusionne des membres au sommet de layout.json sans toucher aux autres —
 * y compris l'arbre "root" et la clé "language". C'est par là que window.json
 * est rapatrié ici sans qu'un module tiers n'ait à apprendre le chemin.
 * `members` n'est pas consommé. */
void
layout_merge_members(JsonObject *members)
{
    char       *path = layout_config_path();
    JsonParser *parser = json_parser_new();
    JsonNode   *file_node = NULL;
    JsonObject *obj;
    GError     *error = NULL;
    gchar      *text;
    GList      *keys;

    if (members == NULL) {
        g_object_unref(parser);
        g_free(path);
        return;
    }
    if (json_parser_load_from_file(parser, path, NULL) &&
        json_parser_get_root(parser) != NULL &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        file_node = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser);
    if (file_node == NULL) {
        file_node = json_node_new(JSON_NODE_OBJECT);
        json_node_init_object(file_node, json_object_new());
    }
    obj = json_node_get_object(file_node);

    keys = json_object_get_members(members);
    for (GList *l = keys; l != NULL; l = l->next) {
        const char *k = l->data;

        json_object_set_member(obj, k,
                               json_node_copy(json_object_get_member(members,
                                                                     k)));
    }
    g_list_free(keys);

    text = json_to_string(file_node, TRUE);
    if (!g_file_set_contents(path, text, -1, &error)) {
        g_printerr(_("CDB: failed to write layout.json: %s\n"), error->message);
        g_error_free(error);
    }
    g_free(text);
    json_node_unref(file_node);
    g_free(path);
}
