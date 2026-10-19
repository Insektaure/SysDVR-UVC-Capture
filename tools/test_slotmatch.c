/*
 * Host test for source/native/slotmatch.c - C port of Switch Frame Tap's
 * tier4/applet-mitm/test/slotmatch_test.cpp (GPL-2.0, Tomas Uribe), same
 * scenarios and bounds; a xorshift PRNG replaces std::mt19937.
 *
 * A game with one buffer per swapchain slot at shuffled addresses, among
 * other buffers the same size (a render target drawn every frame, one never
 * touched, texture memory that changes now and then), presents frames the
 * way a GPU finishes them; the matcher has to find which buffer is which
 * slot - and must never settle on a wrong match.
 *
 *   gcc -std=c11 -O2 -Wall -Wextra -Wno-missing-field-initializers -Isource/native \
 *       tools/test_slotmatch.c source/native/slotmatch.c -o test_slotmatch && ./test_slotmatch
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "slotmatch.h"

#define MAXC SLOTMATCH_MAX_CAND
#define MAXS SLOTMATCH_MAX_SLOTS

static int g_fail = 0;
static uint64_t g_rng;

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static double urand(void) { return (rnd() & 0xFFFFFF) / (double)0x1000000; }

typedef struct {
    uint32_t nslots, ncand;
    bool rt, still_buf;
    double early;      // chance the next slot is already partly drawn when we sample
    double skip;       // chance the capture misses a present
    double still;      // chance a frame repeats the previous picture
    uint32_t frames;
    uint32_t ntex;     // texture buffers among the candidates
    double tex_rate;   // chance per frame that one of them changes
    uint32_t nrts;     // extra render targets drawn every frame
    uint32_t npp;      // ping-pong pairs
} Opts;

// frames until a match (0 = none); *wrong set if the match is not the truth
static uint32_t Run(const Opts* o, uint32_t seed, bool* wrong)
{
    g_rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull);
    if (!g_rng)
        g_rng = 1;

    uint32_t order[MAXC], perm[MAXS], content[MAXC] = {0};
    int rt = -1, rts[MAXC], pp[MAXC][2], tex[MAXC];
    uint32_t nrts = 0, npp = 0, ntex = 0, picture = 0, next_value = 1;

    for (uint32_t c = 0; c < o->ncand; c++)
        order[c] = c;
    for (uint32_t c = o->ncand - 1; c > 0; c--) {
        uint32_t j = rnd() % (c + 1), t = order[c];
        order[c] = order[j];
        order[j] = t;
    }
    for (uint32_t s = 0; s < o->nslots; s++)
        perm[s] = order[s];
    uint32_t k = o->nslots;
    if (o->rt)
        rt = (int)order[k++];
    if (o->still_buf)
        k++;  // a buffer never drawn
    for (uint32_t t = 0; t < o->ntex && k < o->ncand; t++)
        tex[ntex++] = (int)order[k++];
    for (uint32_t t = 0; t < o->nrts && k < o->ncand; t++)
        rts[nrts++] = (int)order[k++];
    for (uint32_t t = 0; t < o->npp && k + 1 < o->ncand; t++) {
        pp[npp][0] = (int)order[k++];
        pp[npp++][1] = (int)order[k++];
    }

    static SlotMatcher m;
    SlotMatcherReset(&m, o->nslots, o->ncand);
    *wrong = false;
    uint32_t fed = 0;
    for (uint32_t n = 1; n <= o->frames; n++) {
        const uint32_t slot = n % o->nslots;
        if (!(urand() < o->still))
            picture = next_value++;
        content[perm[slot]] = picture;                       // the GPU finished present n
        if (rt >= 0)
            content[rt] = 0x80000000u | n;                   // drawn every frame regardless
        for (uint32_t t = 0; t < nrts; t++)
            content[rts[t]] = 0x10000000u | (n * 7 + t);
        for (uint32_t t = 0; t < npp; t++)
            content[pp[t][n & 1]] = 0x08000000u | (n * 5 + t);
        if (ntex && urand() < o->tex_rate)
            content[tex[rnd() % ntex]] ^= 0x20000000u | n;
        if (urand() < o->early)                              // the next frame already started
            content[perm[(slot + 1) % o->nslots]] ^= 0x40000000u | n;
        if (urand() < o->skip)
            continue;                                        // the capture never looked at present n
        uint32_t sig[MAXC];
        for (uint32_t c = 0; c < o->ncand; c++)
            sig[c] = content[c] * 2654435761u + c;
        uint32_t drawn = 0;
        if (fed != 0 && n - fed <= 6) {
            for (uint32_t c = fed + 1; c <= n; c++)
                drawn |= 1u << (c % o->nslots);
        }
        SlotMatcherAddSpan(&m, n, drawn, sig);
        fed = n;
        uint8_t out[MAXS];
        int32_t weakest = 0;
        if (SlotMatcherMatch(&m, out, &weakest)) {
            for (uint32_t s = 0; s < o->nslots; s++) {
                if (out[s] != perm[s])
                    *wrong = true;
            }
            return n;
        }
    }
    return 0;
}

static void Expect(const char* what, Opts o, uint32_t max_frames)
{
    uint32_t worst = 0, failed = 0, wrong = 0;
    for (uint32_t seed = 1; seed <= 200; seed++) {
        bool w = false;
        const uint32_t n = Run(&o, seed, &w);
        wrong += w;
        if (n == 0 || n > max_frames)
            failed++;
        if (n > worst)
            worst = n;
    }
    if (wrong || failed) {
        printf("FAIL %s: %u wrong match(es), %u run(s) not matched within %u frames (of 200)\n", what, wrong, failed, max_frames);
        g_fail++;
    } else {
        printf("ok   %s: matched within %u frames\n", what, worst);
    }
}

static void ExpectNoMatch(const char* what, Opts o)
{
    for (uint32_t seed = 1; seed <= 50; seed++) {
        bool w = false;
        if (Run(&o, seed, &w) != 0) {
            printf("FAIL %s: matched (seed %u)\n", what, seed);
            g_fail++;
            return;
        }
    }
    printf("ok   %s: no match\n", what);
}

int main(void)
{
    // Smash Ultimate: three slots, three buffers, nothing else
    Expect("3 slots, 3 buffers",                   (Opts){ 3, 3,  false, false, 0.0,  0.0,  0.0,  600 }, 60);
    Expect("3 slots among 6 buffers",              (Opts){ 3, 6,  true,  true,  0.0,  0.0,  0.0,  600 }, 60);
    Expect("+ next slot drawn early 30%",          (Opts){ 3, 6,  true,  true,  0.30, 0.0,  0.0,  1200 }, 300);
    Expect("+ 20% presents missed",                (Opts){ 3, 6,  true,  true,  0.10, 0.20, 0.0,  1200 }, 300);
    Expect("+ 40% still frames",                   (Opts){ 3, 6,  true,  true,  0.10, 0.10, 0.40, 2000 }, 600);
    Expect("double-buffered game",                 (Opts){ 2, 4,  true,  true,  0.10, 0.10, 0.0,  1200 }, 300);
    Expect("12 candidates",                        (Opts){ 3, 12, true,  true,  0.10, 0.10, 0.10, 1200 }, 300);
    // Moonlight: three 4 MB blocks among its texture memory, box art loading
    Expect("homebrew: 3 slots among 24, textures", (Opts){ 3, 24, true,  true,  0.10, 0.10, 0.10, 2000, 18, 0.30 }, 400);
    // DuckStation: two slots of one object, emulated VRAM uploaded every frame
    Expect("homebrew: 2 slots among 16, textures", (Opts){ 2, 16, true,  true,  0.10, 0.10, 0.20, 2000, 12, 0.30 }, 400);
    // Minecraft Vibrant Visuals: deferred passes, TAA ping-pong, stream taking ~2 in 5 presents
    Expect("VV: 3 slots among 43, 60% missed",     (Opts){ 3, 43, true,  true,  0.10, 0.60, 0.05, 3000, 0, 0.0, 28, 4 }, 900);
    Expect("VV: + 80% missed",                     (Opts){ 3, 43, true,  true,  0.10, 0.80, 0.05, 6000, 0, 0.0, 28, 4 }, 3000);
    // a picture that never moves says nothing about which buffer is which
    ExpectNoMatch("still picture",                 (Opts){ 3, 6,  true,  true,  0.0,  0.0,  1.0,  2000 });
    {
        static SlotMatcher m;
        SlotMatcherReset(&m, 3, 6);
        uint32_t sig[MAXC] = {0};
        for (uint32_t n = 1; n <= SLOTMATCH_MAX_SAMPLES; n++)
            SlotMatcherAdd(&m, n, n % 3, sig);
        if (!SlotMatcherGaveUp(&m)) {
            printf("FAIL still picture: still sampling after %u samples\n", SLOTMATCH_MAX_SAMPLES);
            g_fail++;
        } else {
            printf("ok   still picture: gives up after %u samples\n", SLOTMATCH_MAX_SAMPLES);
        }
    }
    // the next slot drawn early EVERY time: ambiguous, so no match rather than a wrong one
    {
        uint32_t wrong = 0;
        Opts o = { 3, 6, true, true, 1.0, 0.0, 0.0, 2000 };
        for (uint32_t seed = 1; seed <= 200; seed++) {
            bool w = false;
            Run(&o, seed, &w);
            wrong += w;
        }
        if (wrong) {
            printf("FAIL always-early: %u wrong match(es)\n", wrong);
            g_fail++;
        } else {
            printf("ok   always-early: never a wrong match\n");
        }
    }
    // StillGuess: the swapchain's slots hold the same still picture
    {
        const uint32_t sigs[]  = { 7, 9, 5, 5, 4, 4, 3, 8 };
        const bool varied[]    = { 1, 1, 1, 1, 0, 0, 1, 1 };
        uint8_t out[MAXS] = {0};
        const bool g1 = SlotMatcherStillGuess(sigs, varied, 8, 2, true, out) && out[0] == 2 && out[1] == 3;
        const bool g2 = !SlotMatcherStillGuess(sigs, varied, 8, 3, false, out);
        const uint32_t sigs3[] = { 1, 6, 2, 6, 6, 9 };
        const bool varied3[]   = { 1, 1, 1, 1, 1, 1 };
        const bool g3 = SlotMatcherStillGuess(sigs3, varied3, 6, 3, false, out) && out[0] == 1 && out[1] == 3 && out[2] == 4;
        const uint32_t sigs4[] = { 1, 2, 2, 3 };
        const bool g4 = !SlotMatcherStillGuess(sigs4, varied3, 4, 2, true, out);
        if (g1 && g2 && g3 && g4) {
            printf("ok   still-picture guess: equal slots, cleared buffers skipped\n");
        } else {
            printf("FAIL still-picture guess: %d %d %d %d\n", g1, g2, g3, g4);
            g_fail++;
        }
    }
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
