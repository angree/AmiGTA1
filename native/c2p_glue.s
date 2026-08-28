; C-callable wrappers around Mikael Kalms' chunky-to-planar routines.
;
; TWO routines live here, one per screen depth the AGA path can open:
;
;   c2p_rect              8 bitplanes, special/c2p_rect.s. The only Kalms
;                         routine that takes a chunky ROWMOD, so it converts a
;                         sub-rectangle straight out of our full-width chunky
;                         buffer with no intermediate copy.
;
;   c2p1x1_6_c5_bm_040    6 bitplanes (EHB), bitmap/c2p1x1_6_c5_bm_040.s. This
;                         one has NO rowmod: it insists the chunky input be
;                         contiguous and exactly chunkyx wide. amiga_gfx.c
;                         therefore feeds it either the chunky buffer directly
;                         (full-width rectangles, the common case) or a small
;                         scratch band it has just copied the rectangle into.
;                         That copy is deliberate: this is stock, released,
;                         already-validated assembly, and paying one memcpy is
;                         far cheaper than the risk of a hand-written 6-plane
;                         rect routine. Both source files are verbatim upstream
;                         copies - only these wrappers are ours.
;
; Both take all their arguments in registers, including d2-d7 which the C ABI
; treats as callee-saved. Each routine does save them itself - but only AFTER we
; have already loaded our arguments into them, so it would restore OUR values
; and destroy the caller's. Hence these wrappers save them first.
;
; Assemble with the vasm that ships with bebbo amiga-gcc. The -I is needed by
; c2p1x1_6_c5_bm_040.s, which includes graphics/gfx.i for the BitMap offsets:
;   vasmm68k_mot -Fhunk -m68040 -no-opt \
;                -I/opt/amiga/m68k-amigaos/ndk-include \
;                -o c2p_glue.o c2p_glue.s
; (this one command assembles all three .s files - the two below are included).

	section	code,code

	xdef	_c2p_rect_asm
	xdef	_c2p6_bm_asm

; void c2p_rect_asm(struct C2PArgs *a);
;   0  UWORD x        (multiple of 32)
;   2  UWORD y
;   4  UWORD w        (multiple of 32)
;   6  UWORD h
;   8  UWORD cmod     chunky buffer stride in bytes
;  10  UWORD bmod     bitplane BytesPerRow
;  12  ULONG bplsize  bytes per bitplane (distance between planes)
;  16  APTR  chunky   BASE of the chunky buffer (routine adds x/y itself)
;  20  APTR  bpl      BASE of plane 0 (planes must be contiguous, bplsize apart)

_c2p_rect_asm:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a1		; 11 saved longs = 44, +4 return address
	move.w	(a1),d0
	move.w	2(a1),d1
	move.w	4(a1),d2
	move.w	6(a1),d3
	move.w	8(a1),d4
	move.w	10(a1),d5
	move.l	12(a1),d6
	move.l	16(a1),a0
	move.l	20(a1),a1
	jsr	c2p_rect
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; void c2p6_bm_asm(struct C2P6Args *a);
;   0  UWORD chunkyx  width  in pixels, MUST be a multiple of 32
;   2  UWORD chunkyy  height in rows
;   4  UWORD offsx    destination x in the BitMap, MUST be a multiple of 8
;   6  UWORD offsy    destination y in the BitMap
;   8  APTR  chunky   contiguous chunkyx * chunkyy chunky pixels
;  12  APTR  bitmap   struct BitMap * with at least 6 planes
;
; Unlike c2p_rect this takes no rowmod, which is exactly why the caller must
; hand it contiguous input - see the header comment at the top of this file.

_c2p6_bm_asm:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a1		; 11 saved longs = 44, +4 return address
	move.w	(a1),d0
	move.w	2(a1),d1
	move.w	4(a1),d2
	move.w	6(a1),d3
	move.l	8(a1),a0
	move.l	12(a1),a1
	jsr	c2p1x1_6_c5_bm_040
	movem.l	(sp)+,d2-d7/a2-a6
	rts

	include	"c2p_rect.s"
	include	"c2p1x1_6_c5_bm_040.s"
