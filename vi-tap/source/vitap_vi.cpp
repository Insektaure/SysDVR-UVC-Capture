#include "vitap_vi.hpp"
#include "vitap_present.hpp"
#include <cstring>

namespace ams::vitap {

    namespace {

        std::shared_ptr<::Service> MakeForwardService(const ::Service &s) {
            return std::shared_ptr<::Service>(new ::Service(s), [](::Service *p) { serviceClose(p); delete p; });
        }

        void WrapDisplay(::Service disp, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IDisplayMitm>> &out) {
            auto fwd = MakeForwardService(disp);
            const sf::cmif::DomainObjectId id{ serviceGetObjectId(std::addressof(disp)) };
            ::ams::sf::impl::g_vitap_pending_mitm_forward = fwd;
            out.SetValue(sf::CreateSharedObjectEmplaced<IDisplayMitm, DisplayMitm>(std::move(fwd), ci), id);
            g_counters.displays.fetch_add(1, std::memory_order_relaxed);
        }

        Result ForwardGetDisplayService(::Service *fwd, u32 cmd, u32 policy, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IDisplayMitm>> &out) {
            ::Service disp = {};
            R_TRY(serviceDispatchIn(fwd, cmd, policy, .out_num_objects = 1, .out_objects = std::addressof(disp)));
            WrapDisplay(disp, ci, out);
            R_SUCCEED();
        }

        /* msg: the client's request, copied from TLS before any IPC of ours.
         * plain_cmd: the root's GetDisplayService, used when the request
         * cannot be read. */
        Result ForwardProxyNameExchange(::Service *fwd, u32 cmd, const u8 *msg, const sm::MitmProcessInfo &ci, sf::Out<sf::SharedPointer<IDisplayMitm>> &out, u32 plain_cmd) {
            const HipcParsedRequest req = hipcParseRequest(const_cast<u8 *>(msg));
            const uintptr_t words = reinterpret_cast<uintptr_t>(req.data.data_words);
            const uintptr_t raw   = (words + 0xF) & ~static_cast<uintptr_t>(0xF);
            const size_t total    = static_cast<size_t>(req.meta.num_data_words) * 4;
            const CmifInHeader *hdr = reinterpret_cast<const CmifInHeader *>(raw);
            /* libnx cmifMakeRequest sizes the data words as header + 0x10
             * alignment slack + arguments: what follows the header, less the
             * slack actually used, is the arguments with their padding */
            const bool sane = total >= sizeof(CmifInHeader) + (raw - words) && raw + sizeof(CmifInHeader) <= reinterpret_cast<uintptr_t>(msg) + 0x100
                           && hdr->magic == CMIF_IN_HEADER_MAGIC && hdr->command_id == cmd;
            ::Service disp = {};
            ::Result rc = 0;
            if (sane) {
                size_t args = total - (raw - words) - sizeof(CmifInHeader);
                if (args > 0x40) { args = 0x40; }
                SfDispatchParams params = {};
                params.out_num_objects = 1;
                params.out_objects     = std::addressof(disp);
                rc = serviceDispatchImpl(fwd, cmd, reinterpret_cast<const void *>(raw + sizeof(CmifInHeader)), static_cast<u32>(args), nullptr, 0, params);
            } else {
                const u32 policy = 0;
                rc = serviceDispatchIn(fwd, plain_cmd, policy, .out_num_objects = 1, .out_objects = std::addressof(disp));
            }
            R_TRY(::ams::Result(rc));
            WrapDisplay(disp, ci, out);
            R_SUCCEED();
        }

    }

    Result BinderMitm::TransactParcelAuto(s32 session_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out) {
        AMS_UNUSED(session_id, flags, parcel_out);
        /* record, then let libstratosphere forward the request untouched */
        OnBinderTransaction(m_client_info, code, parcel_in.GetPointer(), parcel_in.GetSize());
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    Result DisplayMitm::GetRelayService(sf::Out<sf::SharedPointer<IBinderMitm>> out) {
        ::Service binder = {};
        R_TRY(serviceDispatch(m_forward_service.get(), 100, .out_num_objects = 1, .out_objects = std::addressof(binder)));
        auto fwd = MakeForwardService(binder);
        const sf::cmif::DomainObjectId id{ serviceGetObjectId(std::addressof(binder)) };
        ::ams::sf::impl::g_vitap_pending_mitm_forward = fwd;
        out.SetValue(sf::CreateSharedObjectEmplaced<IBinderMitm, BinderMitm>(std::move(fwd), m_client_info), id);
        g_counters.binders.fetch_add(1, std::memory_order_relaxed);
        R_SUCCEED();
    }

    Result ViuRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IDisplayMitm>> out, u32 policy) {
        R_RETURN(ForwardGetDisplayService(m_forward_service.get(), 0, policy, m_client_info, out));
    }

    Result ViuRootMitm::GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IDisplayMitm>> out) {
        /* FIRST, before any IPC of ours reuses the TLS message buffer */
        alignas(0x10) u8 msg[0x100];
        std::memcpy(msg, armGetTls(), sizeof(msg));
        R_RETURN(ForwardProxyNameExchange(m_forward_service.get(), 1, msg, m_client_info, out, 0));
    }

    bool VimRootMitm::ShouldMitm(const sm::MitmProcessInfo &ci) {
        const u64 tid = ci.program_id.value;
        const bool app_range = tid >= UINT64_C(0x0100000000010000) && (tid >> 56) != 0x42;
        return app_range || IsRunningApplication(ci.process_id);
    }

    Result VimRootMitm::GetDisplayService(sf::Out<sf::SharedPointer<IDisplayMitm>> out, u32 policy) {
        R_RETURN(ForwardGetDisplayService(m_forward_service.get(), 2, policy, m_client_info, out));
    }

    Result VimRootMitm::GetDisplayServiceWithProxyNameExchange(sf::Out<sf::SharedPointer<IDisplayMitm>> out) {
        alignas(0x10) u8 msg[0x100];
        std::memcpy(msg, armGetTls(), sizeof(msg));
        R_RETURN(ForwardProxyNameExchange(m_forward_service.get(), 3, msg, m_client_info, out, 2));
    }

}
