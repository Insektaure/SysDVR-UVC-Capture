#include "vitap_tap_service.hpp"
#include "vitap_present.hpp"

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

    void TapService::GetStats(sf::Out<UvcTapStats> out) {
        UvcTapStats s;
        vitap::GetStats(std::addressof(s));
        out.SetValue(s);
    }

}
