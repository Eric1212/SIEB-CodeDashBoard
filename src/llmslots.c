/*
 * llmslots.c : persistance des slots JSON de la tuile LLM — voir llmslots.h.
 *
 * Contenu d'un slot : le body chat/completions BRUT, tel qu'envoyé au
 * réseau (json_to_string(root, FALSE)). Aucune enveloppe — le fichier
 * EST la requête.
 */

#include "llmslots.h"
#include "session.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

/* Répertoire des slots d'une session quelconque (g_strdup). */
static char *
slots_dir_of(int session)
{
    char *base = session_dir();
    char *out;
    char  num[8];

    g_snprintf(num, sizeof(num), "%03d", session);
    out = g_build_filename(base, num, "llm_slots", NULL);
    g_free(base);
    return out;
}

/* Répertoire des slots de la session courante, créé au besoin. */
char *
llm_slots_dir(void)
{
    char *dir = slots_dir_of(sieb_session);

    g_mkdir_with_parents(dir, 0700);
    return dir;
}

gboolean
llm_slots_dir_exists(int session)
{
    char *dir = slots_dir_of(session);
    gboolean ok = g_file_test(dir, G_FILE_TEST_IS_DIR);

    g_free(dir);
    return ok;
}

/* Chemin complet d'un slot dans un répertoire donné. */
static char *
slot_path_in(const char *dir, int slot)
{
    char *name = g_strdup_printf("%d.json", slot);
    char *path = g_build_filename(dir, name, NULL);

    g_free(name);
    return path;
}

/* Écriture atomique simple : contenu → fichier (O_TRUNC). */
static gboolean
write_json_file(const char *path, const char *json)
{
    GError *error = NULL;

    if (!g_file_set_contents(path, json, -1, &error)) {
        g_printerr("CDB: écriture %s : %s\n", path, error->message);
        g_error_free(error);
        return FALSE;
    }
    return TRUE;
}

/* Lecture complète ; NULL si absent ou illisible. */
static char *
read_json_file(const char *path)
{
    GError *error = NULL;
    char   *text = NULL;

    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return NULL;
    if (!g_file_get_contents(path, &text, NULL, &error)) {
        g_printerr("CDB: lecture %s : %s\n", path, error->message);
        g_error_free(error);
        return NULL;
    }
    return text;
}

gboolean
llm_slots_last_save(const char *json)
{
    char     *dir = llm_slots_dir();
    char     *path = g_build_filename(dir, "last.json", NULL);
    gboolean  ok = write_json_file(path, json);

    g_free(path);
    g_free(dir);
    return ok;
}

char *
llm_slots_last_load(void)
{
    char *dir = slots_dir_of(sieb_session);
    char *path = g_build_filename(dir, "last.json", NULL);
    char *text = read_json_file(path);

    g_free(path);
    g_free(dir);
    return text;
}

gboolean
llm_slots_save(int slot, const char *json)
{
    char     *dir = llm_slots_dir();
    char     *path = slot_path_in(dir, slot);
    gboolean  ok = write_json_file(path, json);

    g_free(path);
    g_free(dir);
    return ok;
}

char *
llm_slots_load(int slot)
{
    char *dir = slots_dir_of(sieb_session);
    char *path = slot_path_in(dir, slot);
    char *text = read_json_file(path);

    g_free(path);
    g_free(dir);
    return text;
}

gboolean
llm_slots_exists(int slot)
{
    char     *dir = slots_dir_of(sieb_session);
    char     *path = slot_path_in(dir, slot);
    gboolean  ok = g_file_test(path, G_FILE_TEST_IS_REGULAR);

    g_free(path);
    g_free(dir);
    return ok;
}

/* TRUE si <slot>.json existe DANS la session `session` (import). */
gboolean
llm_slots_exists_in(int session, int slot)
{
    char     *dir = slots_dir_of(session);
    char     *path = slot_path_in(dir, slot);
    gboolean  ok = g_file_test(path, G_FILE_TEST_IS_REGULAR);

    g_free(path);
    g_free(dir);
    return ok;
}

void
llm_slots_clear(int slot)
{
    char *dir = slots_dir_of(sieb_session);
    char *path = slot_path_in(dir, slot);

    if (g_unlink(path) != 0 && errno != ENOENT)
        g_printerr("CDB: suppression %s : %s\n", path, g_strerror(errno));
    g_free(path);
    g_free(dir);
}

gboolean
llm_slots_import(int src_session, int src_slot, int dst_slot)
{
    char     *src_dir = slots_dir_of(src_session);
    char     *src_path = slot_path_in(src_dir, src_slot);
    char     *dst_dir;
    char     *dst_path;
    char     *text;
    gboolean  ok;

    text = read_json_file(src_path);
    g_free(src_path);
    g_free(src_dir);
    if (text == NULL)
        return FALSE;

    dst_dir = llm_slots_dir(); /* session courante */
    dst_path = slot_path_in(dst_dir, dst_slot);
    ok = write_json_file(dst_path, text);
    g_free(dst_path);
    g_free(dst_dir);
    g_free(text);
    return ok;
}
