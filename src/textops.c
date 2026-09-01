/*
 * textops.c — voir textops.h. Couche pure, sans vue ni disque.
 *
 * Le CRC32, le hash4 et la regle des offsets sont deplaces ici mot pour
 * mot depuis llmcore.c : aucun changement d'algorithme, sinon les hashes
 * dejà vus par les modeles cesseraient de correspondre.
 */

#include "textops.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Hash                                                                */
/* ------------------------------------------------------------------ */

static guint32 textops_crc_table[256];
static gboolean textops_crc_ready = FALSE;

static void
textops_crc32_init(void)
{
    for (guint32 i = 0; i < 256; i++) {
        guint32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        textops_crc_table[i] = c;
    }
    textops_crc_ready = TRUE;
}

static guint32
textops_crc32(const void *buf, gsize len)
{
    const guint8 *b = buf;
    guint32 c = 0xFFFFFFFFu;

    if (!textops_crc_ready)
        textops_crc32_init();
    for (gsize i = 0; i < len; i++)
        c = textops_crc_table[(c ^ b[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

char *
textops_hash4(const void *buf, gsize len)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    guint32 v = textops_crc32(buf, len) % 1679616u; /* 36^4 */
    char *s = g_new0(char, 5);

    s[4] = '\0';
    for (int i = 3; i >= 0; i--) {
        s[i] = digits[v % 36];
        v /= 36;
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* Adressage des lignes                                                */
/* ------------------------------------------------------------------ */

void
textops_line_offsets(const char *content, gsize len, GArray *off,
                     guint *line_count)
{
    gsize z = 0;
    guint n_nl = 0;
    gboolean ends_nl;

    g_array_set_size(off, 0);
    g_array_append_val(off, z);
    for (gsize i = 0; i < len; i++) {
        if (content[i] == '\n') {
            gsize p = i + 1;
            g_array_append_val(off, p);
            n_nl++;
        }
    }
    ends_nl = (len > 0 && content[len - 1] == '\n');
    *line_count = n_nl + ((ends_nl || len == 0) ? 0 : 1);
    if (!ends_nl && len > 0) {
        gsize last = len;
        g_array_append_val(off, last);
    }
}

guint
textops_line_at(GArray *off, guint line_count, gsize pos)
{
    for (guint k = 1; k <= line_count; k++)
        if (pos < g_array_index(off, gsize, k))
            return k;
    return line_count;
}

guint
textops_logical_lines(const char *t, gsize len)
{
    guint nl = 0;

    if (len == 0)
        return 0;
    for (gsize i = 0; i < len; i++)
        if (t[i] == '\n')
            nl++;
    return nl + ((t[len - 1] == '\n') ? 0 : 1);
}

/* ------------------------------------------------------------------ */
/* Style de fin de ligne                                               */
/* ------------------------------------------------------------------ */

void
textops_scan_eol(const char *content, gsize len, TextopsEol *out)
{
    guint nl = 0, crlf = 0;

    out->eol = "\n";
    out->eol_len = 1;
    out->final_nl = (len > 0 && content[len - 1] == '\n');

    for (gsize i = 0; i < len; i++) {
        if (content[i] != '\n')
            continue;
        nl++;
        if (i > 0 && content[i - 1] == '\r')
            crlf++;
    }
    /* CRLF seulement si TOUS les sauts le sont : un fichier mele garde
     * LF, et le melange reste visible dans les octets plutot que
     * silencieusement normalise. */
    if (nl > 0 && crlf == nl) {
        out->eol = "\r\n";
        out->eol_len = 2;
    }
}

/* ------------------------------------------------------------------ */
/* Blocs de lignes entieres                                            */
/* ------------------------------------------------------------------ */

gboolean
textops_block_range(gsize len, GArray *off, guint line_count,
                    guint from, guint to, gsize *start, gsize *end)
{
    (void)len;

    if (off == NULL || line_count == 0)
        return FALSE;
    if (from < 1 || to < from || to > line_count)
        return FALSE;
    if (start != NULL)
        *start = g_array_index(off, gsize, from - 1);
    if (end != NULL)
        *end = g_array_index(off, gsize, to);
    return TRUE;
}

gchar *
textops_join_block(const char *const *lines, guint n, const TextopsEol *eol,
                   gboolean at_eof, gsize *out_len, TextopsJoinErr *err)
{
    GString *s;
    gsize    total = 0;

    if (n > 0 && lines == NULL)
        return NULL;
    if (eol == NULL)
        return NULL;

    for (guint i = 0; i < n; i++) {
        if (lines[i] == NULL) {
            if (err != NULL) {
                err->bad_line = i + 1;
                err->bad_char = '\0';
            }
            return NULL;
        }
        /* Un element = une ligne. Un \n ou un \r cache dans un element
         * signerait deux lignes la ou le compte en annonce une : refus,
         * et le numero de l'element fautif remonte tel quel. */
        for (const char *p = lines[i]; *p != '\0'; p++) {
            if (*p == '\n' || *p == '\r') {
                if (err != NULL) {
                    err->bad_line = i + 1;
                    err->bad_char = *p;
                }
                return NULL;
            }
        }
        total += strlen(lines[i]);
    }

    if (n == 0) {
        if (out_len != NULL)
            *out_len = 0;
        return g_strdup("");
    }

    /* n - 1 separateurs toujours ; le terminateur de la derniere ligne
     * seulement si le bloc n'est pas en fin de fichier, ou s'il l'est et
     * que le fichier avait deja un \n final. */
    total += eol->eol_len * (n - 1);
    if (!at_eof || eol->final_nl)
        total += eol->eol_len;

    s = g_string_sized_new(total);
    for (guint i = 0; i < n; i++) {
        if (i > 0)
            g_string_append_len(s, eol->eol, eol->eol_len);
        g_string_append(s, lines[i]);
    }
    if (!at_eof || eol->final_nl)
        g_string_append_len(s, eol->eol, eol->eol_len);

    if (out_len != NULL)
        *out_len = s->len;
    return g_string_free(s, FALSE);
}

void
textops_split_block(const char *bytes, gsize len, GPtrArray *out)
{
    gsize start = 0;

    if (out == NULL)
        return;
    for (gsize i = 0; i < len; i++) {
        if (bytes[i] != '\n')
            continue;
        gsize n = i - start;              /* n exclut le \n */
        if (n > 0 && bytes[start + n - 1] == '\r')
            n--;                          /* et le \r de CRLF */
        g_ptr_array_add(out, g_strndup(bytes + start, n));
        start = i + 1;
    }
    if (start < len)                      /* derniere ligne sans \n */
        g_ptr_array_add(out, g_strdup(bytes + start));
}
