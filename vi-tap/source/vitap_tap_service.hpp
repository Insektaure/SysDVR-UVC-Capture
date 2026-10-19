/*
 * "uvctap": what SysDVR-UVC asks vi-tap. Protocol in include/uvctap.h.
 * Holds no per-client state, so a client killed by ovlSysmodules only
 * closes its session.
 */
#pragma once
#include <stratosphere.hpp>
#include "uvctap.h"

#define VITAP_TAP_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, void, GetVersion,       (sf::Out<u32> out),                  (out))       \
    AMS_SF_METHOD_INFO(C, H, 1, void, GetPresentEvent,  (sf::OutCopyHandle out),             (out))       \
    AMS_SF_METHOD_INFO(C, H, 2, void, GetLatestPresent, (sf::Out<UvcTapPresent> out),        (out))       \
    AMS_SF_METHOD_INFO(C, H, 3, void, GetStats,         (sf::Out<UvcTapStats> out),          (out))

AMS_SF_DEFINE_INTERFACE(ams::vitap, ITapService, VITAP_TAP_INTERFACE_INFO, 0x56544205)

namespace ams::vitap {

    class TapService {
        public:
            void GetVersion(sf::Out<u32> out);
            void GetPresentEvent(sf::OutCopyHandle out);
            void GetLatestPresent(sf::Out<UvcTapPresent> out);
            void GetStats(sf::Out<UvcTapStats> out);
    };
    static_assert(IsITapService<TapService>);

}
