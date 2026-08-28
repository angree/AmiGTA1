/* IMA/DVI ADPCM streaming decoder - public C API (no Amiga headers). */
#ifndef AMIGA_ADPCM_H
#define AMIGA_ADPCM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AdpcmStream AdpcmStream;

/* Open a mono IMA-ADPCM WAV and seek to its data chunk. NULL on failure. */
AdpcmStream *Adpcm_Open(const char *path);
void         Adpcm_Close(AdpcmStream *s);

/* Sample rate (Hz), e.g. 22050. */
int  Adpcm_Rate(AdpcmStream *s);

/* Decode up to max_samples 8-bit signed samples into out[]; streams from
 * disk internally. Returns the count decoded, 0 at end of stream. */
int  Adpcm_Decode(AdpcmStream *s, signed char *out, int max_samples);

/* Seek back to the start of the audio data (for looping the title theme). */
void Adpcm_Rewind(AdpcmStream *s);

#ifdef __cplusplus
}
#endif

#endif /* AMIGA_ADPCM_H */
