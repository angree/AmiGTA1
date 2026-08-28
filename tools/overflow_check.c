/* Proof that the sprite placement multiply overflowed 32 bits on the Amiga.
 *
 * The renderer's `long` is 64 bits on the host and 32 on m68k-amigaos, so the
 * bug was invisible in every host picture this project ever took. This models
 * the Amiga's word size explicitly with int32_t and prints both the old
 * expression and the new one for a sprite at each distance from the camera.
 *
 *   gcc -O2 -o /tmp/ovf tools/overflow_check.c && /tmp/ovf
 *
 * Licence: MIT (ours).
 */
#include <stdint.h>
#include <stdio.h>

#define FP 16

/* zoom 32 gives a step of about 34 screen pixels per block at the reference
 * grid level, and the levels above it are larger still. 2 228 224 is 34 << 16. */
#define STEP 2228224L

static int32_t old_way(int32_t dxb, int32_t step)
{
    return (int32_t)(((dxb >> 8) * step) >> 8);
}

static int32_t new_way(int32_t db, int32_t step)
{
    int32_t bi = db >> FP;
    int32_t bf = db - (bi << FP);
    return bi * step + ((bf >> 8) * (step >> 8));
}

int main(void)
{
    int b;
    printf("step %ld (%.1f px per block), screen 320x200 = +-5.0 blocks "
           "across, +-3.1 down\n\n", (long)STEP, STEP / 65536.0);
    printf("%8s  %14s  %14s  %10s\n",
           "blocks", "old (32-bit)", "new (32-bit)", "correct px");
    for (b = 1; b <= 8; b++) {
        int32_t dxb = (int32_t)b << FP;
        int32_t o = old_way(dxb, STEP);
        int32_t n = new_way(dxb, STEP);
        double correct = (double)b * STEP;
        printf("%8d  %14ld  %14ld  %10.0f%s\n", b, (long)o, (long)n, correct,
               (o < 0) ? "   <-- OLD WRAPPED NEGATIVE" : "");
    }
    printf("\nA negative x is off the left of the screen, so the sprite was\n"
           "culled. 320 px is 4.7 blocks at this step: that is why cars\n"
           "vanished at the left and right edges and never at the top.\n");
    return 0;
}
