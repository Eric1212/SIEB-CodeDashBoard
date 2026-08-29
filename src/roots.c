/*
 * Roots : modèle des "Roots de structure" et "Roots de projet".
 *
 * Un Root de structure est un dossier qui contient des projets
 * (ex: /home/eric/dev) — on n'y travaille jamais directement.
 * Un Root de projet est un dossier de travail ouvert dans l'IDE
 * (ex: /home/eric/dev/alvalllm), comme dans Zed.
 *
 * Persistance : membres "roots" et "last_file" de llm.json, par
 * llm_config_merge_members() / llm_config_get_member() — le fichier est
 * écrit par llmcore.c et llmtoolpref.c, qui préservent ce qu'ils ne
 * connaissent pas. L'ancien roots.json est migré une fois, relu, puis
 * supprimé.
 */

#include "roots.h"
#include "llm.h"
#include "session.h"
#include "i18n.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <stdio.h>

/* Ancien fichier des racines : nom conservé pour la migration une fois. */
#define CDB_ROOTS_FILE  "roots.json"

/* Prototypes internes (définies plus bas dans le fichier). */
static gboolean root_in_any_children(GListStore *roots, const char *path);
static void scan_project_dirs(GListStore *roots, RootEntry *structure,
                              const char *path);

G_DEFINE_TYPE(RootEntry, root_entry, G_TYPE_OBJECT)

static void
root_entry_init(RootEntry *e)
{
    e->children = NULL;
}

static void
root_entry_finalize(GObject *object)
{
    RootEntry *e = ROOT_ENTRY(object);

    if (e->children != NULL) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(e->children));
        for (guint i = 0; i < n; i++)
            g_object_unref(g_list_model_get_item(G_LIST_MODEL(e->children), i));
        g_object_unref(e->children);
    }
    if (e->contents != NULL) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(e->contents));
        for (guint i = 0; i < n; i++)
            g_object_unref(g_list_model_get_item(G_LIST_MODEL(e->contents), i));
        g_object_unref(e->contents);
    }
    g_free(e->path);
    g_free(e->basename);

    G_OBJECT_CLASS(root_entry_parent_class)->finalize(object);
}

static void
root_entry_class_init(RootEntryClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = root_entry_finalize;
}

static RootEntry *
root_entry_new(RootKind kind, const char *path)
{
    RootEntry *e = g_object_new(ROOT_TYPE_ENTRY, NULL);

    e->path = g_strdup(path);
    e->kind = kind;
    e->basename = g_path_get_basename(path);
    if (e->basename == NULL || *e->basename == '\0')
        e->basename = g_strdup(path);

    if (kind == ROOT_STRUCTURE)
        e->children = g_list_store_new(ROOT_TYPE_ENTRY);
    e->contents_dirty = FALSE;
    return e;
}

/* Chemin du fichier LEGACY (roots.json). Plus qu'un point de passage : depuis
 * le Jalon G les racines vivent dans llm.json, et cette fonction ne sert
 * qu'à la migration une fois. */
static char *
roots_legacy_path(void)
{
    return session_config_path(CDB_ROOTS_FILE);
}

/* ------------------------------------------------------------------ */
/* JSON : sauvegarde                                                   */
/* ------------------------------------------------------------------ */

static JsonNode *
entry_to_json(RootEntry *e)
{
    JsonBuilder *b = json_builder_new();
    JsonNode    *node;

    /* Seuls les ROOTS sont persistés : pas de children — les projets
     * d'une structure sont reconstruits par scan au chargement. */
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "path");
    json_builder_add_string_value(b, e->path);
    json_builder_set_member_name(b, "kind");
    json_builder_add_string_value(b, e->kind == ROOT_STRUCTURE ? "structure" : "project");
    json_builder_end_object(b);

    node = json_builder_get_root(b);
    g_object_unref(b);
    return node;
}

void
roots_save(GListStore *roots)
{
    JsonBuilder *b = json_builder_new();
    JsonObject  *members;
    JsonNode    *arr;
    gsize        n = g_list_model_get_n_items(G_LIST_MODEL(roots));

    json_builder_begin_array(b);
    for (gsize i = 0; i < n; i++) {
        RootEntry *e = g_list_model_get_item(G_LIST_MODEL(roots), i);

        json_builder_add_value(b, entry_to_json(e));
        g_object_unref(e);
    }
    json_builder_end_array(b);
    arr = json_builder_get_root(b);
    g_object_unref(b);

    /* Fusion, et non réécriture du fichier : les racines ne sont plus seules
     * propriétaires de l'endroit où elles logent. L'ancien roots.json était
     * écrit « en entier » ici même, ce qui effaçait la clé "last_file" à
     * chaque ajout ou retrait de dossier — l'autre écriture du même fichier
     * (roots_write_last_file) la reliait pourtant : deux règles, un fichier.
     * Pas de mkdir : le dossier de session est créé par session_init(). */
    members = json_object_new();
    json_object_set_member(members, "roots", arr);   /* consomme arr */
    llm_config_merge_members(members);
    json_object_unref(members);
}

/* ------------------------------------------------------------------ */
/* JSON : chargement                                                   */
/* ------------------------------------------------------------------ */

/* Construit un root depuis le JSON. Les enfants ne sont PAS lus :
 * ils sont reconstruits par scan (structures) à la racine. */
static RootEntry *
json_to_entry(JsonObject *obj)
{
    const char *path = json_object_get_string_member(obj, "path");
    const char *kind = json_object_get_string_member(obj, "kind");

    if (path == NULL)
        return NULL;

    return root_entry_new(g_strcmp0(kind, "structure") == 0
                          ? ROOT_STRUCTURE : ROOT_PROJECT, path);
}

/* Remplit le modèle depuis un tableau de racines. Partagé par les deux chemins
 * de roots_load : le membre de llm.json, et le fichier legacy migré. */
static void
roots_fill_from_array(GListStore *roots, JsonArray *arr)
{
    guint n;

    if (arr == NULL)
        return;
    n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
        RootEntry *e = json_to_entry(json_array_get_object_element(arr, i));

        if (e == NULL)
            continue;
        if (e->kind == ROOT_STRUCTURE) {
            /* Structure : scanne ses sous-dossiers en projets,
             * et absorbe les projets isolés déjà chargés. */
            g_list_store_append(roots, e);
            scan_project_dirs(roots, e, e->path);
            g_object_unref(e);
        } else {
            /* Projet isolé : ignoré s'il est déjà couvert par une
             * structure chargée précédemment (pas de doublon). */
            if (root_in_any_children(roots, e->path))
                g_object_unref(e);
            else
                g_list_store_append(roots, e);
        }
    }
}

/* Les clés qu'on vient d'écrire sont-elles relues dans llm.json ? C'est la
 * condition de la suppression de l'original : on ne détruit pas un fichier
 * d'utilisateur parce que la copie a échoué. */
static gboolean
llm_members_present(JsonObject *merged)
{
    GList    *keys = json_object_get_members(merged);
    gboolean  ok   = TRUE;

    for (GList *l = keys; l != NULL && ok; l = l->next) {
        JsonNode *n = llm_config_get_member(l->data);

        if (n == NULL)
            ok = FALSE;
        else
            json_node_unref(n);
    }
    g_list_free(keys);
    return ok;
}

/* Migration, une seule fois : roots.json rejoignait llm.json sous les membres
 * "roots" et "last_file". Un legacy qui porterait une clé que je ne connais
 * pas est CONSERVÉ : je ne supprime pas un fichier dont je n'ai pas tout lu. */
static void
roots_migrate_legacy(GListStore *roots)
{
    static const char *MIGRER[] = { "roots", "last_file" };
    char       *path   = roots_legacy_path();
    JsonParser *parser = json_parser_new();
    GError     *error  = NULL;

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_object_unref(parser);
        g_free(path);
        return;
    }
    if (!json_parser_load_from_file(parser, path, &error)) {
        g_printerr(_("CDB: failed to read roots.json: %s\n"), error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_free(path);
        return;                      /* illisible : l'original ne bouge pas */
    }
    {
        JsonObject *legacy = json_node_get_object(json_parser_get_root(parser));
        JsonObject *merge  = json_object_new();
        GList      *toutes;
        guint       total;
        int         connues = 0;

        for (gsize i = 0; i < G_N_ELEMENTS(MIGRER); i++) {
            const char *k = MIGRER[i];

            if (json_object_has_member(legacy, k)) {
                json_object_set_member(merge, k, json_node_copy(
                                    json_object_get_member(legacy, k)));
                connues++;
            }
        }
        toutes = json_object_get_members(legacy);
        total  = g_list_length(toutes);
        g_list_free(toutes);

        if (connues > 0 && (guint)connues == total) {
            llm_config_merge_members(merge);
            if (llm_members_present(merge)) {
                JsonNode *arr = llm_config_get_member("roots");

                if (arr != NULL) {
                    if (JSON_NODE_HOLDS_ARRAY(arr))
                        roots_fill_from_array(roots, json_node_get_array(arr));
                    json_node_unref(arr);
                }
                if (g_unlink(path) == 0)
                    g_printerr(_("CDB: roots.json merged into llm.json\n"));
            }
        }
        json_object_unref(merge);
    }
    g_object_unref(parser);
    g_free(path);
}

GListStore *
roots_load(void)
{
    GListStore *roots  = g_list_store_new(ROOT_TYPE_ENTRY);
    JsonNode   *member = llm_config_get_member("roots");

    if (member != NULL) {
        if (JSON_NODE_HOLDS_ARRAY(member))
            roots_fill_from_array(roots, json_node_get_array(member));
        json_node_unref(member);
        return roots;
    }
    /* Aucune clé "roots" dans llm.json : soit c'est le premier lancement après
     * le déplacement (le legacy existe encore), soit la session est vierge.
     * roots_migrate_legacy() distingue les deux et ne touche à rien si le
     * legacy est absent. */
    roots_migrate_legacy(roots);
    return roots;
}

/* Dernier fichier ouvert : membre "last_file" de llm.json. Appelé après
 * roots_load() dans on_activate(), donc toujours après la migration. */
char *
roots_read_last_file(void)
{
    JsonNode *n    = llm_config_get_member("last_file");
    char     *last = NULL;

    if (n != NULL) {
        if (JSON_NODE_HOLDS_VALUE(n) &&
            json_node_get_value_type(n) == G_TYPE_STRING) {
            const char *s = json_node_get_string(n);

            if (s != NULL && s[0] != '\0')
                last = g_strdup(s);
        }
        json_node_unref(n);
    }
    return last;
}

/* Écrire ici ne peut plus effacer les racines : la fusion ne remplace que la
 * clé qu'on lui donne. L'ancien code, lui, n'écrivait RIEN quand le fichier
 * manquait — un dernier fichier ouvert avant toute racine n'était jamais
 * enregistré. */
void
roots_write_last_file(const char *last_path)
{
    JsonObject *members = json_object_new();

    json_object_set_string_member(members, "last_file",
                                  last_path != NULL ? last_path : "");
    llm_config_merge_members(members);
    json_object_unref(members);
}

/* ------------------------------------------------------------------ */
/* Ajout / suppression                                                 */
/* ------------------------------------------------------------------ */

/* Index d'un root à la racine (niveau 0) par chemin, -1 sinon. */
static gint
root_index_at_top(GListStore *roots, const char *path)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(roots));

    for (guint i = 0; i < n; i++) {
        RootEntry *e = g_list_model_get_item(G_LIST_MODEL(roots), i);
        gboolean eq = g_strcmp0(e->path, path) == 0;

        g_object_unref(e);
        if (eq)
            return (gint)i;
    }
    return -1;
}

/* Le chemin existe-t-il déjà comme enfant d'une structure ? */
static gboolean
root_in_any_children(GListStore *roots, const char *path)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(roots));

    for (guint i = 0; i < n; i++) {
        RootEntry *e = g_list_model_get_item(G_LIST_MODEL(roots), i);

        if (e->children != NULL) {
            guint m = g_list_model_get_n_items(G_LIST_MODEL(e->children));
            for (guint j = 0; j < m; j++) {
                RootEntry *c = g_list_model_get_item(G_LIST_MODEL(e->children), j);
                gboolean eq = g_strcmp0(c->path, path) == 0;

                g_object_unref(c);
                if (eq) {
                    g_object_unref(e);
                    return TRUE;
                }
            }
        }
        g_object_unref(e);
    }
    return FALSE;
}

/* Scan d'une structure : chaque sous-dossier direct devient un root
 * projet. Exclus : fichiers, dossiers cachés (.*).
 * Un projet isolé à la racine situé DANS ce dossier est absorbé :
 * il quitte la racine et devient enfant de la structure (il disparaîtra
 * avec elle si on la supprime). */
static void
scan_project_dirs(GListStore *roots, RootEntry *structure, const char *path)
{
    GDir       *dir;
    const char *name;
    GPtrArray  *found = g_ptr_array_new_with_free_func(g_free);

    dir = g_dir_open(path, 0, NULL);
    if (dir == NULL)
        return;

    while ((name = g_dir_read_name(dir)) != NULL) {
        char *full;

        if (name[0] == '.')
            continue; /* dossier caché : pas un projet */
        full = g_build_filename(path, name, NULL);
        if (!g_file_test(full, G_FILE_TEST_IS_DIR)) {
            g_free(full);
            continue;
        }

        gint idx = root_index_at_top(roots, full);
        if (idx >= 0) {
            /* Le chemin existe déjà à la racine. */
            RootEntry *e = g_list_model_get_item(G_LIST_MODEL(roots), idx);

            if (e->kind == ROOT_PROJECT) {
                /* Projet isolé dans ce dossier → absorbé par la structure. */
                g_list_store_remove(roots, idx);
                e->parent = structure;
                g_list_store_append(structure->children, e);
            }
            g_object_unref(e); /* la structure (ou la racine) garde la ref */
            g_free(full);
            continue;
        }
        if (root_in_any_children(roots, full)) {
            g_free(full);
            continue; /* déjà enfant d'une autre structure : pas de doublon */
        }
        g_ptr_array_add(found, full);
    }
    g_dir_close(dir);

    g_ptr_array_sort(found, (GCompareFunc)g_ascii_strcasecmp);
    for (guint i = 0; i < found->len; i++) {
        RootEntry *p = root_entry_new(ROOT_PROJECT, found->pdata[i]);
        p->parent = structure;
        g_list_store_append(structure->children, p);
    }
    g_ptr_array_free(found, TRUE);
}

/* Re-scan des structures déjà déclarées, à la demande : chaque
 * sous-dossier direct non encore connu devient un root projet. C'est
 * exactement ce que fait le chargement (entry_to_json ne persiste que les
 * racines), simplement en cours de session — sans lui, un « mkdir
 * projet_neuf » dans le répertoire d'accueil restait invisible jusqu'au
 * redémarrage.
 *
 * Idempotent par construction : scan_project_dirs écarte déjà un chemin
 * présent à la racine (il l'absorbe) ou enfant d'une autre structure.
 *
 * Les structures sont RAMASSÉES AVANT le scan. scan_project_dirs peut
 * absorber un projet isolé, donc RETIRER une entrée du store qu'on serait
 * en train de parcourir : un parcours indexé sauterait silencieusement la
 * structure suivante — le genre de bug muet qui ne se remarque jamais.
 *
 * Ce que ça ne fait pas : retirer un projet dont le dossier a disparu du
 * disque. Le faire détruirait un RootEntry, et son cache, sous un tree
 * model encore vivant. Ça se traitera avec la purge des fantômes, pas ici. */
void
roots_rescan_structures(GListStore *roots)
{
    GPtrArray *structures;
    guint      n;

    if (roots == NULL)
        return;
    structures = g_ptr_array_new_with_free_func(g_object_unref);
    n = g_list_model_get_n_items(G_LIST_MODEL(roots));
    for (guint i = 0; i < n; i++) {
        RootEntry *e = g_list_model_get_item(G_LIST_MODEL(roots), i);

        if (e->kind == ROOT_STRUCTURE && e->children != NULL)
            g_ptr_array_add(structures, e); /* la ref part au tableau */
        else
            g_object_unref(e);
    }
    for (guint i = 0; i < structures->len; i++) {
        RootEntry *s = g_ptr_array_index(structures, i);

        scan_project_dirs(roots, s, s->path);
    }
    g_ptr_array_free(structures, TRUE);
}

RootEntry *
roots_add(GListStore *roots, RootEntry *parent, RootKind kind, const char *path)
{
    RootEntry *e = root_entry_new(kind, path);

    e->parent = parent;
    if (parent != NULL && parent->children != NULL)
        g_list_store_append(parent->children, e);
    else
        g_list_store_append(roots, e);
    return e;
}

/* Ajoute un root de structure puis scanne ses sous-dossiers directs :
 * chacun devient un root projet enfant. */
RootEntry *
roots_add_structure(GListStore *roots, const char *path)
{
    RootEntry *e = root_entry_new(ROOT_STRUCTURE, path);

    g_list_store_append(roots, e);
    scan_project_dirs(roots, e, path);
    return e;
}

/* Un chemin est-il déjà un root (racine) ou un projet d'une structure ?
 * Empêche les doublons à l'ajout. */
gboolean
roots_conflict(GListStore *roots, const char *path)
{
    if (root_index_at_top(roots, path) >= 0)
        return TRUE;
    return root_in_any_children(roots, path);
}

void
roots_remove(GListStore *roots, RootEntry *entry)
{
    guint i;

    for (i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(roots)); i++) {
        RootEntry *e = g_list_model_get_item(G_LIST_MODEL(roots), i);
        if (e == entry) {
            g_list_store_remove(roots, i);
            g_object_unref(e); /* libère entry et ses enfants */
            return;
        }
        if (e->children != NULL) {
            guint j;
            for (j = 0; j < g_list_model_get_n_items(G_LIST_MODEL(e->children)); j++) {
                RootEntry *c = g_list_model_get_item(G_LIST_MODEL(e->children), j);
                if (c == entry) {
                    g_list_store_remove(e->children, j);
                    g_object_unref(c);
                    g_object_unref(e);
                    return;
                }
            }
        }
        g_object_unref(e);
    }
    g_printerr(_("CDB: root not found: %s\n"), entry->path);
}

/* Suppression récursive d'un dossier : ne suit pas les liens symboliques
 * (le lien est supprimé, pas sa cible). Retourne FALSE en cas d'échec. */
gboolean
roots_delete_recursive(const char *path)
{
    GDir     *dir;
    gboolean  ok = TRUE;

    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK))
        return g_remove(path) == 0;

    dir = g_dir_open(path, 0, NULL);
    if (dir == NULL)
        return g_remove(path) == 0; /* fichier simple */

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *child = g_build_filename(path, name, NULL);

        ok = roots_delete_recursive(child) && ok;
        g_free(child);
    }
    g_dir_close(dir);
    return g_rmdir(path) == 0 && ok;
}
/* ------------------------------------------------ */
/* Projet courant (pour les panneaux bash)           */
/* ------------------------------------------------ */

/* TRUE si path est le projet lui-même ou un chemin dedans. */
static gboolean
path_in_project(const char *path, const char *project)
{
    size_t len = strlen(project);

    if (strncmp(path, project, len) != 0)
        return FALSE;
    return path[len] == '\0' || path[len] == '/';
}

/* Cherche le projet (RootEntry PROJECT) contenant path : projet racine
 * direct, ou projet enfant d'une structure. Ref +1, ou NULL. */
static RootEntry *
find_project_for_path(GListStore *roots, const char *path)
{
    guint n;

    if (roots == NULL || path == NULL)
        return NULL;
    n = g_list_model_get_n_items(G_LIST_MODEL(roots));
    for (guint i = 0; i < n; i++) {
        RootEntry *e = g_list_model_get_item(G_LIST_MODEL(roots), i);

        if (e->kind == ROOT_PROJECT) {
            gboolean hit = path_in_project(path, e->path);

            if (hit)
                return e; /* ref transférée à l'appelant */
            g_object_unref(e);
            continue;
        }
        {
            guint m = e->children != NULL
                          ? g_list_model_get_n_items(G_LIST_MODEL(e->children))
                          : 0;
            RootEntry *found = NULL;

            for (guint j = 0; j < m && found == NULL; j++) {
                RootEntry *pr =
                    g_list_model_get_item(G_LIST_MODEL(e->children), j);

                if (path_in_project(path, pr->path))
                    found = pr; /* ref conservée */
                else
                    g_object_unref(pr);
            }
            g_object_unref(e);
            if (found != NULL)
                return found;
        }
    }
    return NULL;
}

char *
roots_current_project(GListStore *roots, GHashTable *multi_paths)
{
    GHashTableIter iter;
    gpointer       key;
    char          *inside = NULL;
    char          *exact  = NULL;

    if (roots == NULL || multi_paths == NULL ||
        g_hash_table_size(multi_paths) == 0)
        return NULL;
    g_hash_table_iter_init(&iter, multi_paths);
    while (g_hash_table_iter_next(&iter, &key, NULL) && exact == NULL) {
        const char *path = key;
        RootEntry  *pr = find_project_for_path(roots, path);

        if (pr != NULL) {
            if (strcmp(path, pr->path) == 0) {
                g_free(exact);
                exact = g_strdup(pr->path); /* prioritaire, on sort */
            } else if (inside == NULL) {
                inside = g_strdup(pr->path);
            }
            g_object_unref(pr);
        }
    }
    if (exact != NULL) {
        g_free(inside);
        return exact;
    }
    return inside; /* NULL si rien */
}
