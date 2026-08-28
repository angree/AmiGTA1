; amiga_span_blit.s - the LUT span blit of Surface::blitNShade, by hand.
;
; The C loop (gcc 6.5 -O1) spends 3 instructions per pixel (clr.l + 2 move.b)
; plus a lot of stack-relative reloads between runs; this keeps everything in
; registers and does 2 instructions per pixel, unrolled by 4. Only the
; unclipped-in-X case comes here - the C fallback handles clipped blits.
;
; void amiga_span_blit_lut(const UBYTE *row,    /* source pixels at row ry0   */
;                          UBYTE *drow,         /* dst + dy0*dpitch + dx0-sx0 */
;                          const UBYTE *spans,  /* per row: n, then n*(x,len) */
;                          const UWORD *rowoff, /* [h] offset of row in spans */
;                          LONG ry0, LONG ry1,  /* rows [ry0, ry1)            */
;                          LONG spitch, LONG dpitch,
;                          const UBYTE *lut);   /* 256-byte shade table       */
;
; C ABI: args on the stack, d2-d7/a2-a6 callee-saved.

	section	code,code

	xdef	_amiga_span_blit_lut

_amiga_span_blit_lut:
	movem.l	d2-d7/a2-a6,-(sp)	; 11 longs = 44 bytes; args from 48(sp)

	move.l	64(sp),d0		; ry0
	move.l	68(sp),d5		; ry1
	sub.l	d0,d5			; rows
	ble	.done

	movea.l	48(sp),a0		; source row base
	movea.l	52(sp),a1		; dst row base
	movea.l	60(sp),a4		; rowoff
	add.l	d0,d0
	adda.l	d0,a4			; rowoff += ry0 (UWORD entries)
	move.l	72(sp),d6		; spitch
	move.l	76(sp),d7		; dpitch
	movea.l	80(sp),a2		; lut
	moveq	#0,d0			; pixel register: upper bits stay 0

.rowloop:
	moveq	#0,d1
	move.w	(a4)+,d1		; rowoff[y]
	movea.l	56(sp),a3
	adda.l	d1,a3			; sq = spans + rowoff[y]
	move.b	(a3)+,d3		; n (byte; flags set)
	beq	.rownext

.runloop:
	moveq	#0,d1
	move.b	(a3)+,d1		; x
	moveq	#0,d2
	move.b	(a3)+,d2		; len (>= 1)
	movea.l	a0,a6
	adda.l	d1,a6			; src = row + x
	movea.l	a1,a5
	adda.l	d1,a5			; dst = drow + x

	move.w	d2,d4
	lsr.w	#2,d4			; len / 4
	beq	.tail
	subq.w	#1,d4
.quad:
	move.b	(a6)+,d0
	move.b	(a2,d0.w),(a5)+
	move.b	(a6)+,d0
	move.b	(a2,d0.w),(a5)+
	move.b	(a6)+,d0
	move.b	(a2,d0.w),(a5)+
	move.b	(a6)+,d0
	move.b	(a2,d0.w),(a5)+
	dbra	d4,.quad
.tail:
	and.w	#3,d2			; len % 4
	beq	.rundone
	subq.w	#1,d2
.tailloop:
	move.b	(a6)+,d0
	move.b	(a2,d0.w),(a5)+
	dbra	d2,.tailloop
.rundone:
	subq.b	#1,d3
	bne	.runloop

.rownext:
	adda.l	d6,a0
	adda.l	d7,a1
	subq.l	#1,d5
	bne	.rowloop

.done:
	movem.l	(sp)+,d2-d7/a2-a6
	rts
