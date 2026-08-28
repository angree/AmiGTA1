/* IMA/DVI ADPCM streaming decoder for the Amiga OpenTTD music driver.
 *
 * Plain C, stdio only - NO Amiga <proto/*> headers (they collide with
 * OpenTTD's C++ #define Point). Compiles unchanged on the host for testing
 * (define ADPCM_HOST_TEST) and on m68k-amigaos with libnix stdio.
 *
 * The source WAVs are IMA ADPCM (wFormatTag 0x11), mono, 22050 Hz, 4-bit,
 * blockAlign 1024, 2041 samples/block. We STREAM from disk: a compressed
 * staging buffer (ADPCM_STAGE_BYTES) is filled from the open file and
 * decoded block-by-block on demand, so a whole track is never loaded.
 * Output is 8-bit signed (predictor >> 8), ready for Paula.
 *
 * Build (Amiga): m68k-amigaos-gcc -O0 -mcpu=68020 -msoft-float -noixemul
 *                -c amiga_adpcm.c -o amiga_adpcm.o
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amiga_adpcm.h"

/* Disk staging buffer for compressed ADPCM. User spec: ~256 KB, min 128 KB.
 * 256 KB of 4-bit mono ADPCM is ~23 s of audio, so disk is touched rarely. */
#define ADPCM_STAGE_BYTES (256 * 1024)

/* Standard IMA ADPCM tables. */
static const int ima_index_table[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8
};
static const int ima_step_table[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
	41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
	190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
	7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
	18500, 20350, 22385, 24623, 27086, 29794, 32767
};

struct AdpcmStream {
	FILE *f;
	long  data_off;        /* file offset of the 'data' chunk payload */
	unsigned long data_len;/* payload byte count */
	unsigned long data_read;/* payload bytes already pulled off disk */
	int   rate;            /* samples per second (e.g. 22050) */
	int   block_align;     /* bytes per ADPCM block (e.g. 1024) */
	int   samples_per_block;

	/* compressed staging buffer, filled from disk */
	unsigned char *stage;
	int   stage_len;       /* valid bytes in stage */
	int   stage_pos;       /* next unconsumed byte */

	/* decoded holding buffer for the current block's leftover samples */
	signed char *dec;      /* samples_per_block bytes */
	int   dec_len;
	int   dec_pos;
};

/* little-endian readers */
static unsigned int rd_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long rd_u32(const unsigned char *p)
{ return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24); }

/* Refill the staging buffer: keep the unconsumed tail, append fresh disk bytes
 * up to the remaining data payload. Returns bytes now available (stage_len). */
static int StageRefill(AdpcmStream *s)
{
	int keep = s->stage_len - s->stage_pos;
	if (keep > 0 && s->stage_pos > 0) memmove(s->stage, s->stage + s->stage_pos, keep);
	if (keep < 0) keep = 0;
	s->stage_pos = 0;
	s->stage_len = keep;

	unsigned long remain = s->data_len - s->data_read;
	int room = ADPCM_STAGE_BYTES - s->stage_len;
	if (room > (int)remain) room = (int)remain;
	if (room > 0) {
		int got = (int)fread(s->stage + s->stage_len, 1, room, s->f);
		if (got < 0) got = 0;
		s->stage_len += got;
		s->data_read += got;
	}
	return s->stage_len;
}

/* Decode exactly one ADPCM block from the staging buffer into s->dec.
 * Returns sample count, or 0 if no full block is available (EOF). */
static int DecodeBlock(AdpcmStream *s)
{
	if (s->stage_len - s->stage_pos < s->block_align) {
		if (StageRefill(s) < s->block_align) {
			/* Tail shorter than a full block: nothing more to decode. */
			return 0;
		}
	}
	const unsigned char *b = s->stage + s->stage_pos;
	s->stage_pos += s->block_align;

	/* Block header (mono): predictor int16 LE, step index byte, reserved. */
	int predictor = (int)(short)rd_u16(b);
	int index = b[2];
	if (index < 0) index = 0;
	if (index > 88) index = 88;

	signed char *out = s->dec;
	int n = 0;
	out[n++] = (signed char)(predictor >> 8);   /* the header sample itself */

	int i;
	for (i = 4; i < s->block_align && n < s->samples_per_block; i++) {
		int byte = b[i];
		int half;
		for (half = 0; half < 2 && n < s->samples_per_block; half++) {
			int nibble = half ? (byte >> 4) : (byte & 0x0f);
			int step = ima_step_table[index];
			int diff = step >> 3;
			if (nibble & 1) diff += step >> 2;
			if (nibble & 2) diff += step >> 1;
			if (nibble & 4) diff += step;
			if (nibble & 8) predictor -= diff; else predictor += diff;
			if (predictor > 32767) predictor = 32767;
			if (predictor < -32768) predictor = -32768;
			index += ima_index_table[nibble];
			if (index < 0) index = 0;
			if (index > 88) index = 88;
			out[n++] = (signed char)(predictor >> 8);
		}
	}
	return n;
}

AdpcmStream *Adpcm_Open(const char *path)
{
	AdpcmStream *s = (AdpcmStream *)calloc(1, sizeof(AdpcmStream));
	if (s == NULL) return NULL;

	s->f = fopen(path, "rb");
	if (s->f == NULL) { free(s); return NULL; }

	/* Walk the RIFF chunks to find 'fmt ' and 'data'. */
	unsigned char hdr[12];
	if (fread(hdr, 1, 12, s->f) != 12 ||
			memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
		Adpcm_Close(s); return NULL;
	}

	int have_fmt = 0, have_data = 0;
	unsigned char ch[8];
	while (fread(ch, 1, 8, s->f) == 8) {
		unsigned long clen = rd_u32(ch + 4);
		if (memcmp(ch, "fmt ", 4) == 0) {
			unsigned char fmt[40];
			unsigned long want = clen < sizeof(fmt) ? clen : sizeof(fmt);
			if (fread(fmt, 1, want, s->f) != want) { Adpcm_Close(s); return NULL; }
			unsigned int tag = rd_u16(fmt);
			unsigned int chans = rd_u16(fmt + 2);
			if (tag != 0x0011 || chans != 1) { Adpcm_Close(s); return NULL; } /* IMA ADPCM mono only */
			s->rate = (int)rd_u32(fmt + 4);
			s->block_align = (int)rd_u16(fmt + 12);
			s->samples_per_block = (want >= 20) ? (int)rd_u16(fmt + 18)
			                                    : (s->block_align - 4) * 2 + 1;
			/* skip any padding of this chunk */
			if (clen > want) fseek(s->f, (long)(clen - want), SEEK_CUR);
			if (clen & 1) fseek(s->f, 1, SEEK_CUR);
			have_fmt = 1;
		} else if (memcmp(ch, "data", 4) == 0) {
			s->data_off = ftell(s->f);
			s->data_len = clen;
			have_data = 1;
			break;   /* stream from here on */
		} else {
			fseek(s->f, (long)(clen + (clen & 1)), SEEK_CUR);
		}
	}
	if (!have_fmt || !have_data || s->block_align <= 4 || s->samples_per_block <= 0) {
		Adpcm_Close(s); return NULL;
	}

	s->stage = (unsigned char *)malloc(ADPCM_STAGE_BYTES);
	s->dec   = (signed char *)malloc(s->samples_per_block);
	if (s->stage == NULL || s->dec == NULL) { Adpcm_Close(s); return NULL; }
	s->data_read = 0;
	s->stage_len = s->stage_pos = 0;
	s->dec_len = s->dec_pos = 0;
	return s;
}

void Adpcm_Close(AdpcmStream *s)
{
	if (s == NULL) return;
	if (s->f != NULL) fclose(s->f);
	if (s->stage != NULL) free(s->stage);
	if (s->dec != NULL) free(s->dec);
	free(s);
}

int Adpcm_Rate(AdpcmStream *s) { return s ? s->rate : 0; }

void Adpcm_Rewind(AdpcmStream *s)
{
	if (s == NULL) return;
	fseek(s->f, s->data_off, SEEK_SET);
	s->data_read = 0;
	s->stage_len = s->stage_pos = 0;
	s->dec_len = s->dec_pos = 0;
}

int Adpcm_Decode(AdpcmStream *s, signed char *out, int max_samples)
{
	int produced = 0;
	while (produced < max_samples) {
		if (s->dec_pos >= s->dec_len) {
			s->dec_len = DecodeBlock(s);
			s->dec_pos = 0;
			if (s->dec_len == 0) break;   /* EOF */
		}
		int avail = s->dec_len - s->dec_pos;
		int want = max_samples - produced;
		int n = avail < want ? avail : want;
		memcpy(out + produced, s->dec + s->dec_pos, n);
		s->dec_pos += n;
		produced += n;
	}
	return produced;
}

#ifdef ADPCM_HOST_TEST
/* Host test: decode a whole file, write 8-bit raw + a 16-bit PCM WAV for
 * listening, and print stats. Usage: prog in.wav out_prefix */
static void put_u32(FILE *o, unsigned long v){ fputc(v&255,o);fputc((v>>8)&255,o);fputc((v>>16)&255,o);fputc((v>>24)&255,o); }
static void put_u16(FILE *o, unsigned int v){ fputc(v&255,o);fputc((v>>8)&255,o); }

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s in.wav out_prefix\n", argv[0]); return 2; }
	AdpcmStream *s = Adpcm_Open(argv[1]);
	if (s == NULL) { fprintf(stderr, "open/parse failed\n"); return 1; }
	fprintf(stderr, "rate=%d block_align=%d samples_per_block=%d data_len=%lu\n",
			s->rate, s->block_align, s->samples_per_block, s->data_len);

	char raw_path[512], wav_path[512];
	snprintf(raw_path, sizeof(raw_path), "%s.raw8", argv[2]);
	snprintf(wav_path, sizeof(wav_path), "%s.pcm16.wav", argv[2]);
	FILE *raw = fopen(raw_path, "wb");
	FILE *wav = fopen(wav_path, "wb");

	/* reserve WAV header space */
	long hdr_at = 0; unsigned long nsamp = 0;
	fseek(wav, 44, SEEK_SET);

	signed char buf[4096];
	int mn = 127, mx = -128; long total = 0; double energy = 0;
	for (;;) {
		int n = Adpcm_Decode(s, buf, (int)sizeof(buf));
		if (n == 0) break;
		fwrite(buf, 1, n, raw);
		int i;
		for (i = 0; i < n; i++) {
			int v8 = buf[i];
			if (v8 < mn) mn = v8; if (v8 > mx) mx = v8;
			energy += (double)v8 * v8;
			int v16 = v8 << 8;
			put_u16(wav, (unsigned int)(v16 & 0xffff));
		}
		total += n;
		nsamp += n;
	}
	/* backfill WAV header (PCM16 mono @ rate) */
	fseek(wav, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, wav); put_u32(wav, 36 + nsamp * 2);
	fwrite("WAVE", 1, 4, wav); fwrite("fmt ", 1, 4, wav); put_u32(wav, 16);
	put_u16(wav, 1); put_u16(wav, 1); put_u32(wav, s->rate);
	put_u32(wav, s->rate * 2); put_u16(wav, 2); put_u16(wav, 16);
	fwrite("data", 1, 4, wav); put_u32(wav, nsamp * 2);
	fclose(wav); fclose(raw);
	(void)hdr_at;

	fprintf(stderr, "decoded samples=%ld  8bit min=%d max=%d  rms=%.1f  dur=%.1fs\n",
			total, mn, mx, total ? __builtin_sqrt(energy / total) : 0.0, s->rate ? (double)total / s->rate : 0.0);
	fprintf(stderr, "wrote %s (%ld bytes) and %s\n", raw_path, total, wav_path);
	Adpcm_Close(s);
	return 0;
}
#endif
