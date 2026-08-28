/* Paula audio for OpenTTD on AmigaOS 68k - OS layer, plain C API.
 *
 * ALL Amiga OS calls live in amiga_audio.c: Amiga system headers cannot be
 * included from OpenTTD's C++ (name collisions break the build), so this
 * header exposes no Amiga types at all - only plain ints and void pointers.
 * Same split as amiga_gfx.c / amiga_gfx.h for video.
 *
 * Model (proven standalone in native/paulatest.c):
 *  - audio.device, all four channels allocated at once (ADCMD_ALLOCATE,
 *    mask 15, ADIOF_NOWAIT: if anything else holds Paula we fail fast and
 *    the game runs silent).
 *  - One CMD_WRITE per sound effect, one Paula channel per effect, played by
 *    DMA with ioa_Cycles = 1; the CPU does nothing after starting it.
 *  - Requests are sent with BeginIO() ONLY - SendIO()/DoIO() clear io_Flags,
 *    wiping ADIOF_PERVOL, which silently leaves the channel at volume 0.
 *  - Completion is polled with CheckIO(), never Wait(): the game is built
 *    thread_none and must never block.
 */

#ifndef AMIGA_AUDIO_H
#define AMIGA_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Number of Paula channels. Channels 0 and 3 route to the LEFT jack,
 * 1 and 2 to the RIGHT jack. */
#define AMIGA_AUDIO_CHANNELS 4

/* Open audio.device and allocate all four channels.
 * Returns 1 on success, 0 on failure (caller must then stay silent;
 * every other call below is safe but a no-op / failure in that state). */
int AmigaAudio_Open(void);

/* Abort any playing sound, free the channels, close the device.
 * Safe to call at any time, including after a failed/absent Open. */
void AmigaAudio_Close(void);

/* Allocate/free a sample buffer Paula DMA can read: Chip RAM, word-aligned.
 * Returns NULL when Chip RAM is exhausted. FreeSample(NULL) is a no-op. */
void *AmigaAudio_AllocSample(unsigned long bytes);
void AmigaAudio_FreeSample(void *p);

/* 1 if channel ch (0..3) is idle. Reaps a completed request (CheckIO +
 * WaitIO) as a side effect; never blocks. 0 while still playing. */
int AmigaAudio_ChannelIdle(int ch);

/* Start playing on channel ch: 8-bit signed samples in Chip RAM (from
 * AllocSample), even byte count (max 131070 - the Paula length register),
 * Paula period (>= 124 on PAL, i.e. <= ~28.6 kHz), hardware volume 0..64.
 * Plays once (ioa_Cycles = 1), entirely by DMA. Returns 1 if started,
 * 0 if not open / channel busy / bad arguments. */
int AmigaAudio_Play(int ch, void *chipdata, unsigned long bytes,
                    int period, int volume);

/* ---- streaming music on Paula channels 2 (right) + 3 (left) -------------
 *
 * The same mono stream is played on both a left and a right channel so music
 * is never stuck in one ear. While music streams, the SFX side (amiga_s.cpp)
 * confines itself to channels 0 and 1; with music off, SFX uses all four.
 *
 * Gapless double buffering: two Chip buffers are kept queued on each music
 * channel, so audio.device chains the next before the current one ends. The
 * refill callback is pulled from AmigaAudio_MusicService(), called once per
 * frame from the video driver's main loop - never from an interrupt, so the
 * thread_none contract holds.
 */

/* Start streaming. period = PAL_CLOCK/rate (>=124). chunk_samples = 8-bit
 * samples per buffer (even; clamped internally). refill(ud, dst, max) writes
 * up to max 8-bit signed samples to dst and returns the count (0 = end of
 * stream). Returns 1 on success, 0 if audio is unavailable or Chip RAM is
 * exhausted. */
int  AmigaAudio_MusicStart(int period, int chunk_samples,
                           int (*refill)(void *ud, signed char *dst, int max),
                           void *ud);

/* Stop streaming: abort the music writes, free the Chip buffers, hand
 * channels 2 and 3 back to the SFX pool. Safe to call when not streaming. */
void AmigaAudio_MusicStop(void);

/* Refill any music buffer both channels have finished with; call once per
 * frame. Cheap and non-blocking when nothing needs refilling. */
void AmigaAudio_MusicService(void);

/* Hardware volume 0..64 for the music channels; applied from the next
 * buffer (so within one chunk). */
void AmigaAudio_MusicSetVolume(int volume);

/* 1 while a track is loaded/streaming - the SFX side then avoids channels
 * 2 and 3. */
int  AmigaAudio_MusicActive(void);

/* 1 once the stream has drained AND both channels have played the last
 * buffer out (so the caller can loop or advance to the next track). */
int  AmigaAudio_MusicFinished(void);

#ifdef __cplusplus
}
#endif

#endif /* AMIGA_AUDIO_H */
