/* The original's texts - see gta_text.h.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gta_text.h"

int gta_text_load(gta_text *t, const char *path)
{
    FILE *f;
    long len;
    unsigned char *raw;
    unsigned long i, o = 0, n = 0;
    int key, off, in_key = 0, keyval = 0, at_start = 1;

    memset(t, 0, sizeof *t);
    f = fopen(path, "rb");
    if (!f) {
        printf("gta_text: no %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 8) { fclose(f); return 0; }
    raw = (unsigned char *)malloc((unsigned long)len);
    t->buf = (char *)malloc((unsigned long)len + 1);
    if (!raw || !t->buf) { free(raw); gta_text_free(t); fclose(f); return 0; }
    if (fread(raw, 1, (unsigned long)len, f) != (unsigned long)len) {
        free(raw); gta_text_free(t); fclose(f); return 0;
    }
    fclose(f);

    if (raw[0] == 0xBF) { key = 0x63; off = -1; }
    else if (raw[0] == 0xA6) { key = 0x67; off = 28; }
    else { free(raw); gta_text_free(t); printf("gta_text: %s is not an FXT\n", path); return 0; }

    for (i = 0; i < (unsigned long)len; i++) {
        int c = raw[i];
        if (n < 8) { c = (c - key) & 0xff; key = (key << 1) & 0xff; }
        n++;
        if (((c + off) & 0xff) == 195 && i + 1 < (unsigned long)len) {
            c = (raw[++i] + 64) & 0xff;
            n++;
        }
        c = (c + off) & 0xff;

        /* "[key]" opens a string; the number is what the script uses. */
        if (at_start && c == '[') { in_key = 1; keyval = 0; at_start = 0; continue; }
        if (in_key) {
            if (c == ']') {
                in_key = 0;
                if (t->n < GTA_TEXT_MAX) {
                    t->keys[t->n] = keyval;
                    t->offs[t->n] = o;
                    t->n++;
                }
            } else if (c >= '0' && c <= '9') {
                keyval = keyval * 10 + (c - '0');
            } else {
                keyval = -1;                /* a named key: not indexed */
            }
            continue;
        }
        t->buf[o++] = (char)c;
        if (c == 0) at_start = 1;
    }
    t->buf[o] = 0;
    t->size = o;
    free(raw);
    printf("gta_text: %d strings from %s\n", t->n, path);
    return t->n;
}

void gta_text_free(gta_text *t)
{
    free(t->buf);
    t->buf = NULL;
    t->n = 0;
}

const char *gta_text_get(const gta_text *t, int key)
{
    int i;
    if (!t->buf) return NULL;
    for (i = 0; i < t->n; i++)
        if (t->keys[i] == key)
            return t->buf + t->offs[i];
    return NULL;
}
