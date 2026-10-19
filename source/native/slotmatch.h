#pragma once
// Which candidate buffer is which swapchain slot.
//
// The binder parcels give each slot's offset in its nvmap object, never an
// address, so the buffers are found in the game's memory and matched by
// behaviour: between two presents only the slot that was drawn changes, so
// slot s's buffer changes in every interval that drew s and (almost) never
// in the others. Each candidate scores, per slot, its change rate in the
// intervals that drew the slot minus its rate in the intervals that did not:
// the slot's own buffer scores ~1000, a render target that changes every
// frame ~0.
//
// C port of Switch Frame Tap's applet_mitm_slotmatch.hpp (GPL-2.0, Tomas
// Uribe), thresholds unchanged. Pure logic: host test in
// tools/test_slotmatch.c.
#include <stdbool.h>
#include <stdint.h>

#define SLOTMATCH_MAX_SLOTS   8
#define SLOTMATCH_MAX_CAND    48   // Minecraft back in handheld: > 24 regions of a 720p buffer's size
#define SLOTMATCH_MIN_PER_SLOT 8   // counted intervals per slot before any match
#define SLOTMATCH_MIN_SCORE   600  // of 1000
#define SLOTMATCH_MIN_MARGIN  300  // over the slot's second-best candidate
#define SLOTMATCH_GIVE_UP     900  // counted intervals (~15 s of moving picture at 60 fps)
#define SLOTMATCH_MAX_SAMPLES 3600 // samples of any kind (~1 min): a still picture stops it too

typedef struct {
    uint32_t nslots, ncand;
    uint32_t sig[SLOTMATCH_MAX_CAND];
    uint32_t last;      // the present count sig[] was taken at
    bool     have;
    uint16_t hit_in[SLOTMATCH_MAX_SLOTS][SLOTMATCH_MAX_CAND];   // changed, in intervals that drew the slot
    uint16_t hit_out[SLOTMATCH_MAX_SLOTS][SLOTMATCH_MAX_CAND];  // changed, in intervals that did not
    uint16_t tot_in[SLOTMATCH_MAX_SLOTS];
    uint16_t tot_out[SLOTMATCH_MAX_SLOTS];
    uint32_t intervals;
    uint32_t samples;
} SlotMatcher;

void SlotMatcherReset(SlotMatcher* m, uint32_t slots, uint32_t cands);

// The candidates' signatures once present `count` is finished. `drawn`: a bit
// per slot drawn by the presents since the last sample, 0 when unknown.
void SlotMatcherAddSpan(SlotMatcher* m, uint32_t count, uint32_t drawn, const uint32_t* sigs);

// One present after the last sample: it drew `slot`.
void SlotMatcherAdd(SlotMatcher* m, uint32_t count, uint32_t slot, const uint32_t* sigs);

// True once every slot has a candidate that stands out, each a different one:
// out[slot] = candidate, *weakest = the lowest score.
bool SlotMatcherMatch(const SlotMatcher* m, uint8_t* out, int32_t* weakest);

bool SlotMatcherGaveUp(const SlotMatcher* m);

// A first guess while the picture stands still: a still picture is the same
// in every slot. `grouped`: the slots are offsets of one object, so a set is
// nslots consecutive candidates.
bool SlotMatcherStillGuess(const uint32_t* sigs, const bool* varied, uint32_t ncand,
                           uint32_t nslots, bool grouped, uint8_t* out);
