/* Differential test of the claim the native span rests on:
   the guest's 64-pixel unrolled block + fixup arithmetic + remainder loop
   is exactly equivalent to one merged `while (dst < end)` loop -
   same bytes written, same final %ecx, same final %esi. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t  LUT[65536];
static uint32_t PAL[256];

/* transcribed from the decoded guest code, structure preserved exactly */
static void guest( uint8_t *base, uint32_t dst, uint32_t end, uint32_t src,
                   uint32_t *out_ecx, uint32_t *out_esi )
{
    uint32_t eax, ecx = dst, ebx = end, esi = src, edx;
    #define RD16(a) (*(uint16_t *)(base + (a)))
    #define WR32(a,v) (*(uint32_t *)(base + (a)) = (v))
    if (!(ecx >= ebx - 0x100))                 /* cmp/jae guard at 0x519a4f */
    {
        uint32_t limit = ebx - 0x100, ecx0 = ecx;
        eax = ecx; edx = esi;
        do {
            uint32_t eax0 = eax, edx0 = edx;
            int i;
            eax += 0x100; edx += 0x80;
            for (i = 0; i < 64; i++)           /* 64 unrolled groups */
                WR32( eax0 + 4 * i, PAL[ LUT[ RD16( edx0 + 2 * i ) ] ] );
        } while (eax < limit);                 /* cmp/jb at 0x519f54 */
        {                                      /* the fixup at 0x519f66..0x519f7f */
            uint32_t n = ((0xfffffeffu - ecx0 + ebx) >> 8) + 1;
            ecx = ecx0 + (n << 8);
            esi = esi + (n << 7);
        }
    }
    if (!(ecx >= ebx))                          /* cmp/jae at 0x519f83 */
    {
        eax = ecx;
        do {                                    /* the remainder loop at 0x519f87 */
            edx = RD16( esi );
            eax += 4; esi += 2;
            WR32( eax - 4, PAL[ LUT[ edx ] ] );
        } while (eax < ebx);
        ecx = ecx + (((ebx - 1 - ecx) >> 2) * 4) + 4;   /* 0x519fa2..0x519faa */
    }
    *out_ecx = ecx; *out_esi = esi;
    #undef RD16
    #undef WR32
}

static void merged( uint8_t *base, uint32_t dst, uint32_t end, uint32_t src,
                    uint32_t *out_ecx, uint32_t *out_esi )
{
    while (dst < end)
    {
        *(uint32_t *)(base + dst) = PAL[ LUT[ *(uint16_t *)(base + src) ] ];
        dst += 4; src += 2;
    }
    *out_ecx = dst; *out_esi = src;
}

int main( void )
{
    static uint8_t a[1 << 22], b[1 << 22];
    unsigned t, fails = 0;
    srandom( 12345 );
    for (t = 0; t < 65536; t++) LUT[t] = (uint8_t)random();
    for (t = 0; t < 256; t++)   PAL[t] = (uint32_t)random();

    for (t = 0; t < 20000; t++)
    {
        /* cover: shorter than one unrolled pass, exact multiples, +-1 pixel,
           odd source alignment, and end not a multiple of 4 from dst */
        uint32_t dst = 0x1000 + 4 * (random() % 16);
        uint32_t pixels = (t < 300) ? t : (unsigned)(random() % 20000);
        uint32_t end = dst + 4 * pixels + (random() % 4);
        uint32_t src = 0x200000 + 2 * (random() % 16) + (random() % 2);
        uint32_t ca, sa, cb, sb;

        if (end < 0x100) continue;
        memset( a + 0x1000, 0xa5, 0x120000 ); memcpy( b, a, sizeof a );
        for (uint32_t i = 0; i < 0x80000; i++) a[src + i] = b[src + i] = (uint8_t)random();

        guest ( a, dst, end, src, &ca, &sa );
        merged( b, dst, end, src, &cb, &sb );

        if (ca != cb || sa != sb || memcmp( a, b, sizeof a ))
        {
            printf( "MISMATCH t=%u dst=%x end=%x src=%x pixels=%u: ecx %x/%x esi %x/%x buf %s\n",
                    t, dst, end, src, pixels, ca, cb, sa, sb,
                    memcmp( a, b, sizeof a ) ? "DIFFER" : "same" );
            if (++fails > 5) return 1;
        }
    }
    printf( fails ? "FAILED (%u)\n" : "all %u cases identical: bytes, final ecx, final esi\n",
            fails ? fails : t );
    return fails != 0;
}
