/*
 * fp_conv.c - correct soft-float int->double / int->float conversions for
 * m68k-amigaos (bebbo GCC 6.5, -msoft-float).
 *
 * WHY THIS FILE EXISTS (diagnosed 2026-07-20, map-generation freeze):
 *
 * The toolchain resolves __floatunsidf / __floatsidf from libgcc.a members
 * xfpgnulib__floatunsidf.o / xfpgnulib__floatsidf.o. Those routines build the
 * IEEE bits with integer code but RETURN THE RESULT IN FPU REGISTER %fp0
 * (they end in "fmove.d ...,%fp0"). This program is compiled -msoft-float, so
 * every call site expects the double in d0/d1 (verified against the linked
 * binary: each call site reads d0/d1 right after the jsr).
 * The caller therefore reads stale register garbage:
 *     fp: (double)(uint)123 = -2.46e+269   (on the Amiga)
 * and the garbage propagated through perlin_coast_noise_2D into max_x of
 * HeightMapCoastLines, whose fill loop then never terminated -> clean freeze
 * during map generation.
 *
 * Double->int (__fixdfsi) is NOT affected: an int result lands in d0 under
 * both conventions, which is why "(int)(a*10)" probed correct while
 * "(double)(uint)123" probed garbage.
 *
 * REPLACEMENT SET - exactly the routines that are broken or would break:
 *   __floatunsidf  uint32 -> double   broken today (returns in fp0)
 *   __floatsidf    int32  -> double   broken today (returns in fp0); the
 *                                     probes never exercised it, but it
 *                                     carries the identical defect
 *   __floatunsisf  uint32 -> float    libgcc's version is a wrapper that
 *   __floatsisf    int32  -> float    calls __float(un)sidf and reads %fp0
 *                                     afterwards ("fmoves %fp0,d0"); it works
 *                                     today by accident but would break the
 *                                     moment the didf routines stop setting
 *                                     fp0, so it must be replaced together
 *                                     with them.
 * NOT replaced (verified working or compatible in the linked binary):
 *   __fixdfsi      integer code, int result in d0, probe-verified correct
 *   __extendsfdf2, __truncdfsf2   libnix mathieee stubs that return in BOTH
 *                                 d0/d1 and fp0 - compatible with soft-float
 *   __floatdidf, __floatundidf, __fixunsdfsi   not linked into the binary at
 *                                 all (no call sites); libgcc's versions are
 *                                 C-coded on top of __float(un)sidf and would
 *                                 bind to these corrected routines if a
 *                                 future relink ever pulls them in.
 *
 * The implementations are pure integer code: they construct the IEEE-754 bit
 * pattern directly and return it through a union. They call NO libgcc/libnix
 * float routine, so they cannot recurse, and they use whatever return
 * convention the compiler itself emits for a C function returning
 * double/float under -msoft-float (d0/d1 resp. d0) - matching every call
 * site by construction.
 *
 * BUILD: compile at -O0 (correctness first: this toolchain has a documented
 * history of -O1 miscompilation, see the FORCE_O0 list in CLAUDE.md; these
 * are tiny leaf routines, and map generation calls them only a bounded
 * number of times, so the cost is irrelevant):
 *
 *   m68k-amigaos-gcc -O0 -m68040 -msoft-float -noixemul -c fp_conv.c
 *
 * LINK: the object is added to LIBS in objs/release/Makefile, same pattern
 * as amiga_gfx.o / c2p_glue.o. A plain .o on the link line is linked
 * unconditionally, so these definitions are registered before libgcc.a is
 * scanned and its xfpgnulib members are never pulled in (each broken symbol
 * lives in its own archive member, so no duplicate-definition clash is
 * possible).
 *
 * Mirrored: repo native/openttd/fp_conv.c <-> build tree src/fp_conv.c.
 */

typedef unsigned int u32; /* 32-bit on m68k */

union dconv {
	double d;
	struct { u32 hi; u32 lo; } w; /* m68k is big-endian: high word first */
};

union fconv {
	float f;
	u32 w;
};

/*
 * Build a double from a 32-bit magnitude and a ready-made sign bit.
 * A 32-bit integer always fits in the 53-bit mantissa, so the conversion is
 * exact and no rounding path is needed.
 */
static double make_double(u32 mag, u32 signbit)
{
	union dconv u;
	int n;

	if (mag == 0) {
		u.w.hi = 0;
		u.w.lo = 0;
		return u.d;
	}

	/* Normalise: shift left until bit 31 holds the leading 1.
	 * n ends up as the position of the MSB, i.e. the binary exponent. */
	n = 31;
	while ((mag & 0x80000000u) == 0) {
		mag <<= 1;
		n--;
	}

	/* value = 1.frac * 2^n, frac = the 31 bits below the MSB placed at the
	 * top of the 52-bit fraction field: frac<<21. Top 20 fraction bits go
	 * into the high word, the remaining 11 into the top of the low word. */
	u.w.hi = signbit | ((u32)(1023 + n) << 20) | ((mag >> 11) & 0x000FFFFFu);
	u.w.lo = mag << 21;
	return u.d;
}

/*
 * Build a float from a 32-bit magnitude and a ready-made sign bit.
 * A 32-bit integer can exceed the 24-bit mantissa, so this one must round
 * (round-to-nearest, ties-to-even, like the FPU would).
 */
static float make_float(u32 mag, u32 signbit)
{
	union fconv u;
	u32 word, rest;
	int n;

	if (mag == 0) {
		u.w = 0;
		return u.f;
	}

	n = 31;
	while ((mag & 0x80000000u) == 0) {
		mag <<= 1;
		n--;
	}

	/* 23 fraction bits from below the MSB; the 8 bits shifted out decide
	 * the rounding. */
	word = signbit | ((u32)(127 + n) << 23) | ((mag >> 8) & 0x007FFFFFu);
	rest = mag & 0xFFu;
	if (rest > 0x80u || (rest == 0x80u && (word & 1u) != 0)) {
		/* Adding 1 to the packed word is the classic trick: a mantissa
		 * overflow carries straight into the exponent, which is exactly
		 * the correct IEEE result. Overflow to infinity cannot happen
		 * (max exponent here is 127+31=158, well below 255). */
		word += 1u;
	}
	u.w = word;
	return u.f;
}

double __floatunsidf(u32 a)
{
	return make_double(a, 0);
}

double __floatsidf(int a)
{
	if (a < 0) {
		/* 0u - (u32)a is the magnitude; correct for INT_MIN too. */
		return make_double(0u - (u32)a, 0x80000000u);
	}
	return make_double((u32)a, 0);
}

float __floatunsisf(u32 a)
{
	return make_float(a, 0);
}

float __floatsisf(int a)
{
	if (a < 0) {
		return make_float(0u - (u32)a, 0x80000000u);
	}
	return make_float((u32)a, 0);
}
