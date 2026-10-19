#include "vitap_tap_service.hpp"
#include "vitap_present.hpp"
#include <cstring>

namespace ams::vitap {

    void TapService::GetVersion(sf::Out<u32> out) {
        out.SetValue(UVCTAP_VERSION);
    }

    void TapService::GetPresentEvent(sf::OutCopyHandle out) {
        out.SetValue(GetPresentEventHandle(), false);
    }

    void TapService::GetLatestPresent(sf::Out<UvcTapPresent> out) {
        UvcTapPresent r;
        vitap::GetLatestPresent(std::addressof(r));
        out.SetValue(r);
    }

    void TapService::GetSurface(sf::Out<UvcTapSurface> out) {
        UvcTapSurface s;
        vitap::GetSurface(std::addressof(s));
        out.SetValue(s);
    }

    void TapService::GetRecentPresents(const sf::OutBuffer &out, sf::Out<u32> count) {
        /* the caller's buffer may be any size: copy whole records only */
        UvcTapPresent r[UVCTAP_RECENT_MAX];
        const u32 n = vitap::GetRecentPresents(r, UVCTAP_RECENT_MAX);
        const u32 fit = static_cast<u32>(out.GetSize() / sizeof(UvcTapPresent));
        const u32 give = n < fit ? n : fit;
        std::memcpy(out.GetPointer(), r, give * sizeof(UvcTapPresent));
        count.SetValue(give);
    }

    void TapService::GetStats(sf::Out<UvcTapStats> out) {
        UvcTapStats s;
        vitap::GetStats(std::addressof(s));
        out.SetValue(s);
    }

}
