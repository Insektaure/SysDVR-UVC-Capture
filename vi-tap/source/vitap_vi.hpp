/*
 * The vi mitm: just enough of the display service to see the running
 * application's binder transactions, everything else forwarded untouched.
 *
 *   vi:u  GetDisplayService (0) / ...WithProxyNameExchange (1)   games
 *   vi:m  GetDisplayService (2) / ...WithProxyNameExchange (3)   homebrew (libnx tries vi:m first)
 *     -> IApplicationDisplayService.GetRelayService (100)
 *       -> IHOSBinderDriver.TransactParcelAuto (3)               queueBuffer, setPreallocatedBuffer
 *                           TransactParcel (0)                   the same, from games built before
 *                                                                firmware 3.0.0 (map-alias buffers)
 *
 * Layout and rules from Switch Frame Tap's applet_mitm_service.hpp (GPL-2.0):
 *  - OpenLayer is NOT declared: declaring it made games fail to launch.
 *  - The proxy-name variants take arguments that differ between sources, so
 *    they are declared with no typed input and the client's raw request is
 *    forwarded unchanged (BOTW opens the display this way).
 *  - Wrapped sub-objects get a forward service through the libstratosphere
 *    patch (tools/patch_libstrat.py): vi:u sessions are not domains.
 *  - Every forward service is closed with its wrapper (SFT v0.7.13: leaking
 *    them made vi abort around the 15th game launch).
 */
#pragma once
#include <stratosphere.hpp>

#define VITAP_BINDER_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, TransactParcel, (s32 session_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out), (session_id, code, flags, parcel_in, parcel_out)) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, TransactParcelAuto, (s32 session_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out), (session_id, code, flags, parcel_in, parcel_out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::vitap, IBinderMitm, VITAP_BINDER_INTERFACE_INFO, 0x56544201)

#define VITAP_DISPLAY_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 100, Result, GetRelayService, (sf::Out<sf::SharedPointer<ams::vitap::IBinderMitm>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::vitap, IDisplayMitm, VITAP_DISPLAY_INTERFACE_INFO, 0x56544202)

#define VITAP_VIU_ROOT_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::vitap::IDisplayMitm>> out, u32 policy), (out, policy)) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, GetDisplayServiceWithProxyNameExchange, (sf::Out<sf::SharedPointer<ams::vitap::IDisplayMitm>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::vitap, IViuRootMitm, VITAP_VIU_ROOT_INTERFACE_INFO, 0x56544203)

#define VITAP_VIM_ROOT_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetDisplayService, (sf::Out<sf::SharedPointer<ams::vitap::IDisplayMitm>> out, u32 policy), (out, policy)) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, GetDisplayServiceWithProxyNameExchange, (sf::Out<sf::SharedPointer<ams::vitap::IDisplayMitm>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::vitap, IVimRootMitm, VITAP_VIM_ROOT_INTERFACE_INFO, 0x56544204)

namespace ams::vitap {

    class BinderMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result TransactParcel(s32 session_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out);
            Result TransactParcelAuto(s32 session_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out);
    };
    static_assert(IsIBinderMitm<BinderMitm>);

    class DisplayMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result GetRelayService(sf::Out<sf::SharedPointer<IBinderMitm>> out);
    };
    static_assert(IsIDisplayMitm<DisplayMitm>);

    class ViuRootMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            /* games only; homebrew presents through vi:m */
            static bool ShouldMitm(const sm::MitmProcessInfo &ci) {
                return ncm::IsApplicationId(ci.program_id) && !ci.override_status.IsHbl();
            }
        public:
            Result GetDisplayService(sf::Out<sf::SharedPointer<IDisplayMitm>> out, u32 policy);
            Result GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IDisplayMitm>> out);
    };
    static_assert(IsIViuRootMitm<ViuRootMitm>);

    class VimRootMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            /* No IPC here (see g_app_pid). Applications and forwarders have
             * program ids from 0x0100000000010000 up; 0x42... is homebrew
             * sysmodules (the Tesla/Ultrahand overlay loader among them).
             * qlaunch, am and every other vi:m user get the real service. */
            static bool ShouldMitm(const sm::MitmProcessInfo &ci);
        public:
            Result GetDisplayService(sf::Out<sf::SharedPointer<IDisplayMitm>> out, u32 policy);
            Result GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IDisplayMitm>> out);
    };
    static_assert(IsIVimRootMitm<VimRootMitm>);

}
