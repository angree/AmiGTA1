/*
 * fp_single.c - IEEE single-precision multiply and divide in integer code.
 *
 * WHY THIS FILE EXISTS (diagnosed 2026-08-16, first-frame Guru #8000000B):
 *
 * With -msoft-float this toolchain does not put float arithmetic in libgcc;
 * libnix's libm.a supplies __mulsf3/__divsf3 as stubs that call the ROM
 * mathieeesingbas.library (IEEESPMul, LVO -78 / IEEESPDiv, LVO -84).
 * In Kickstart 3.1 (mathieeesingbas 40.4, checked in the A4000 40.70 image,
 * checksum valid) the function table the library installs on a machine
 * WITHOUT an FPU has its Mul and Div entries pointing back into the table
 * itself: calling either executes the table bytes -
 *
 *     FCA0BA:  001a 001c   ori.b #$1c,(a2)+       <- IEEESPMul lands here
 *     FCA0BC:  001c fe5e   ori.b #$5e,(a4)+       <- IEEESPDiv lands here
 *     FCA0BE:  fe5e        line-F  -> Guru 8000000B
 *
 * - a stray byte write through a2/a4 and then a line-F trap. Verified on the
 * emulated 68020 (AttnFlags 0x0003) with a trap handler: TRAP 11 at
 * PC 00FCA0C0 for Div, 00FCA0BE for Mul, opcode words fe3c / fe5e. Every
 * other IEEE entry point tested (SP Add/Sub/Cmp, DP Add/Mul/Div, DP
 * Sqrt/Sin/Pow) works. So exactly these two are replaced.
 *
 * The routines below never touch libgcc/libnix float code (no recursion is
 * possible) and return through the same convention the compiler uses for a
 * C function returning float under -msoft-float (d0), matching every call
 * site by construction. Round-to-nearest-even, subnormals, infinities and
 * NaNs are handled the way the FPU would; the host-side test
 * (build/test_fp_single.c) checks a few million random and edge cases bit
 * for bit against hardware IEEE arithmetic.
 *
 * (a*b) uses the 68020's 32x32->64 mulu.l via a plain u64 product; (a/b)
 * uses divu.l 64/32->32 through inline asm on the target, portable C on the
 * host.
 *
 * LINK: as an object on the link line, before -lm, so the linker binds these
 * definitions and never pulls libm.a's __mulsf3.o / __divsf3.o members.
 */

typedef unsigned int u32;
typedef unsigned long long u64;

union fbits { float f; u32 u; };

#define SIGN_BIT 0x80000000u
#define QNAN     0x7fc00000u

/*
 * Assemble a float from sign, a biased exponent and a 48-bit mantissa whose
 * leading 1 sits in bit 47 (value = mant/2^47 * 2^(exp-127)); mant may carry
 * a sticky bit in bit 0. Rounds to nearest even; produces subnormals, zero
 * and infinity as required.
 */
static float fp_pack(u32 sign, int exp, u64 mant)
{
	union fbits r;
	int shift;
	u64 rem, half;
	u32 m;

	if (exp >= 255) {
		r.u = sign | 0x7f800000u;
		return r.f;
	}
	if (exp <= 0) {
		/* Subnormal (or underflow to zero): the field holds 23 bits with
		 * no implicit 1, and the value must be shifted down a further
		 * (1 - exp) places. */
		shift = 25 - exp;
		if (shift > 49) {
			r.u = sign;              /* below half of the smallest subnormal */
			return r.f;
		}
		m = (u32)(mant >> shift);
		half = 1ull << (shift - 1);
		rem = mant & ((1ull << shift) - 1);
		if (rem > half || (rem == half && (m & 1)))
			m++;                     /* may carry into bit 23 = smallest normal */
		r.u = sign | m;
		return r.f;
	}
	m = (u32)(mant >> 24);           /* 24 bits, implicit 1 in bit 23 */
	rem = mant & 0xffffffull;
	half = 0x800000ull;
	if (rem > half || (rem == half && (m & 1)))
		m++;
	if (m == 0x1000000u) {           /* rounding carried out of the mantissa */
		m >>= 1;
		exp++;
		if (exp >= 255) {
			r.u = sign | 0x7f800000u;
			return r.f;
		}
	}
	r.u = sign | ((u32)exp << 23) | (m & 0x7fffffu);
	return r.f;
}

/* Split a float into biased exponent and 24-bit mantissa with the implicit
 * bit made explicit; subnormals are normalised (exponent may go below 1). */
static void fp_unpack(u32 u, int *exp, u32 *mant)
{
	int e = (int)((u >> 23) & 0xff);
	u32 m = u & 0x7fffffu;
	if (e == 0) {
		e = 1;
		while (!(m & 0x800000u)) {
			m <<= 1;
			e--;
		}
	} else {
		m |= 0x800000u;
	}
	*exp = e;
	*mant = m;
}

float __mulsf3(float a, float b)
{
	union fbits ua, ub, r;
	u32 sign, ma, mb;
	int ea, eb, exp;
	u64 p;

	ua.f = a;
	ub.f = b;
	sign = (ua.u ^ ub.u) & SIGN_BIT;
	ea = (int)((ua.u >> 23) & 0xff);
	eb = (int)((ub.u >> 23) & 0xff);

	if (ea == 255 || eb == 255) {
		int nan_a = ea == 255 && (ua.u & 0x7fffffu);
		int nan_b = eb == 255 && (ub.u & 0x7fffffu);
		int zero_a = (ua.u & 0x7fffffffu) == 0;
		int zero_b = (ub.u & 0x7fffffffu) == 0;
		if (nan_a || nan_b || zero_a || zero_b) {  /* NaN, or inf * 0 */
			r.u = QNAN;
			return r.f;
		}
		r.u = sign | 0x7f800000u;
		return r.f;
	}
	if ((ua.u & 0x7fffffffu) == 0 || (ub.u & 0x7fffffffu) == 0) {
		r.u = sign;
		return r.f;
	}

	fp_unpack(ua.u, &ea, &ma);
	fp_unpack(ub.u, &eb, &mb);

	p = (u64)ma * mb;                /* 24x24 -> 46..48 bits (mulu.l on 020) */
	if (p & (1ull << 47)) {
		exp = ea + eb - 126;
	} else {
		p <<= 1;
		exp = ea + eb - 127;
	}
	return fp_pack(sign, exp, p);
}

float __divsf3(float a, float b)
{
	union fbits ua, ub, r;
	u32 sign, ma, mb, q, rem;
	int ea, eb, exp;
	u64 mant;

	ua.f = a;
	ub.f = b;
	sign = (ua.u ^ ub.u) & SIGN_BIT;
	ea = (int)((ua.u >> 23) & 0xff);
	eb = (int)((ub.u >> 23) & 0xff);

	if (ea == 255 || eb == 255) {
		int nan_a = ea == 255 && (ua.u & 0x7fffffu);
		int nan_b = eb == 255 && (ub.u & 0x7fffffu);
		if (nan_a || nan_b || (ea == 255 && eb == 255)) {  /* NaN, inf/inf */
			r.u = QNAN;
			return r.f;
		}
		if (ea == 255) {             /* inf / finite */
			r.u = sign | 0x7f800000u;
			return r.f;
		}
		r.u = sign;                  /* finite / inf */
		return r.f;
	}
	if ((ub.u & 0x7fffffffu) == 0) {
		if ((ua.u & 0x7fffffffu) == 0) {
			r.u = QNAN;              /* 0 / 0 */
			return r.f;
		}
		r.u = sign | 0x7f800000u;    /* x / 0 */
		return r.f;
	}
	if ((ua.u & 0x7fffffffu) == 0) {
		r.u = sign;                  /* 0 / x */
		return r.f;
	}

	fp_unpack(ua.u, &ea, &ma);
	fp_unpack(ub.u, &eb, &mb);

	/*
	 * q = (ma << 31) / mb. ma and mb are both in [2^23, 2^24), so the
	 * quotient is in [2^30, 2^32): it always fits 32 bits, and carries
	 * 24 mantissa bits plus 7 or 8 extra bits; the remainder supplies the
	 * sticky bit. That is exactly what divu.l 64/32->32 computes.
	 */
#if defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || defined(__mc68060__)
	{
		u32 hi = ma >> 1;            /* ma << 31 as a 64-bit hi:lo pair */
		u32 lo = ma << 31;
		__asm__("divul %2,%1:%0" : "+d"(lo), "+d"(hi) : "d"(mb));
		q = lo;
		rem = hi;
	}
#else
	{
		u64 num = (u64)ma << 31;
		q = (u32)(num / mb);
		rem = (u32)(num % mb);
	}
#endif
	if (q & 0x80000000u) {           /* ma >= mb: quotient in [1, 2) */
		mant = (u64)q << 16;
		exp = ea - eb + 127;
	} else {                         /* quotient in [0.5, 1): renormalise */
		mant = (u64)q << 17;
		exp = ea - eb + 126;
	}
	if (rem)
		mant |= 1;                   /* sticky: below every rounding position */
	return fp_pack(sign, exp, mant);
}
