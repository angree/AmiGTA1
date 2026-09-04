/* THE ORIGINAL'S TEXTS - english.fxt (french, german, italian).
 *
 * The file is a run of "[key]text\0" records under a light cipher
 * (gta.mendelsohn.de, Carnage3D's FXTReader): the first byte says which
 * key - 0xBF: subtract 0x63 shifted left once per byte from the first
 * eight bytes, then subtract 1 from every byte; 0xA6: key 0x67 and add 28
 * - and a byte that decodes to 195 is dropped with 64 added to the next.
 * The keys are numbers: 1001.. are the briefs, 2501.. the messages, 4003
 * BUSTED, 4004 WASTED; the mission script names them by number.
 *
 * Decoded once into one buffer, indexed by numeric key. Portable C89.
 * Licence: MIT (ours).
 */
#ifndef GTA_TEXT_H
#define GTA_TEXT_H

#define GTA_TEXT_MAX 2048

typedef struct {
    char *buf;                  /* every string, NUL-terminated, in order */
    unsigned long size;
    int  n;
    int  keys[GTA_TEXT_MAX];    /* numeric key of string i, -1 for none */
    unsigned long offs[GTA_TEXT_MAX];
} gta_text;

/* Returns the number of strings, 0 when the file is absent. */
int  gta_text_load(gta_text *t, const char *path);
void gta_text_free(gta_text *t);

/* The text for numeric key `key`, or NULL. */
const char *gta_text_get(const gta_text *t, int key);

#endif /* GTA_TEXT_H */
