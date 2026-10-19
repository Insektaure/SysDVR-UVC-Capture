#include <string.h>
#include "slotmatch.h"

void SlotMatcherReset(SlotMatcher* m, uint32_t slots, uint32_t cands)
{
    memset(m, 0, sizeof(*m));
    m->nslots = slots < SLOTMATCH_MAX_SLOTS ? slots : SLOTMATCH_MAX_SLOTS;
    m->ncand  = cands < SLOTMATCH_MAX_CAND ? cands : SLOTMATCH_MAX_CAND;
}

void SlotMatcherAddSpan(SlotMatcher* m, uint32_t count, uint32_t drawn, const uint32_t* sigs)
{
    m->samples++;
    const uint32_t all = (m->nslots >= 32) ? 0xFFFFFFFFu : ((1u << m->nslots) - 1);
    drawn &= all;
    if (m->have && count > m->last && drawn != 0 && drawn != all) {
        bool any = false;
        for (uint32_t c = 0; c < m->ncand; c++)
            any |= sigs[c] != m->sig[c];
        if (any) {
            m->intervals++;
            for (uint32_t s = 0; s < m->nslots; s++) {
                const bool in = (drawn >> s) & 1;
                uint16_t* tot = in ? &m->tot_in[s] : &m->tot_out[s];
                uint16_t* hit = in ? m->hit_in[s] : m->hit_out[s];
                if (*tot == 0xFFFF)
                    continue;
                (*tot)++;
                for (uint32_t c = 0; c < m->ncand; c++) {
                    if (sigs[c] != m->sig[c])
                        hit[c]++;
                }
            }
        }
    }
    memcpy(m->sig, sigs, m->ncand * sizeof(uint32_t));
    m->last = count;
    m->have = true;
}

void SlotMatcherAdd(SlotMatcher* m, uint32_t count, uint32_t slot, const uint32_t* sigs)
{
    SlotMatcherAddSpan(m, count,
        (m->have && count == m->last + 1 && slot < m->nslots) ? (1u << slot) : 0, sigs);
}

bool SlotMatcherMatch(const SlotMatcher* m, uint8_t* out, int32_t* weakest)
{
    if (m->nslots < 2)
        return false;
    for (uint32_t s = 0; s < m->nslots; s++) {
        if (m->tot_in[s] < SLOTMATCH_MIN_PER_SLOT || m->tot_out[s] < SLOTMATCH_MIN_PER_SLOT)
            return false;
    }
    uint8_t found[SLOTMATCH_MAX_SLOTS] = {0};
    uint64_t used = 0;
    int32_t worst = 1000;
    for (uint32_t s = 0; s < m->nslots; s++) {
        int32_t best = -1, best_score = -100000, second = -100000;
        for (uint32_t c = 0; c < m->ncand; c++) {
            const int32_t score = (int32_t)(m->hit_in[s][c] * 1000u / m->tot_in[s]) -
                                  (int32_t)(m->hit_out[s][c] * 1000u / m->tot_out[s]);
            if (score > best_score) {
                second = best_score;
                best_score = score;
                best = (int32_t)c;
            } else if (score > second) {
                second = score;
            }
        }
        if (best < 0 || best_score < SLOTMATCH_MIN_SCORE || best_score - second < SLOTMATCH_MIN_MARGIN ||
            (used & (UINT64_C(1) << best)) != 0)
            return false;
        found[s] = (uint8_t)best;
        used |= UINT64_C(1) << best;
        if (best_score < worst)
            worst = best_score;
    }
    memcpy(out, found, m->nslots);
    if (weakest)
        *weakest = worst;
    return true;
}

bool SlotMatcherGaveUp(const SlotMatcher* m)
{
    return m->intervals >= SLOTMATCH_GIVE_UP || m->samples >= SLOTMATCH_MAX_SAMPLES;
}

bool SlotMatcherStillGuess(const uint32_t* sigs, const bool* varied, uint32_t ncand,
                           uint32_t nslots, bool grouped, uint8_t* out)
{
    if (nslots < 2 || nslots > SLOTMATCH_MAX_SLOTS || ncand < nslots)
        return false;
    if (grouped) {
        for (uint32_t c = 0; c + nslots <= ncand; c += nslots) {
            bool same = varied[c];
            for (uint32_t k = 1; k < nslots && same; k++)
                same = varied[c + k] && sigs[c + k] == sigs[c];
            if (same) {
                for (uint32_t k = 0; k < nslots; k++)
                    out[k] = (uint8_t)(c + k);
                return true;
            }
        }
        return false;
    }
    for (uint32_t c = 0; c < ncand; c++) {
        if (!varied[c])
            continue;
        uint32_t n = 0;
        for (uint32_t d = c; d < ncand && n < nslots; d++) {
            if (varied[d] && sigs[d] == sigs[c])
                out[n++] = (uint8_t)d;
        }
        if (n == nslots)
            return true;
    }
    return false;
}
