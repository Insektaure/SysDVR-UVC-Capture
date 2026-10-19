#!/usr/bin/env python3
"""
libstratosphere patch for vi-tap: non-domain mitm sub-object forwarding.

Upstream libstratosphere only gives a forward service to sub-objects returned
from a mitm command on DOMAIN sessions. A game's vi:u session is not a domain:
SetOutObjectImpl calls plain RegisterSession, so the wrapped sub-object
(IApplicationDisplayService, IHOSBinderDriver) has no forward service and any
command the wrapper does not declare aborts in ForwardRequest().

The fix is a thread-local side channel: the mitm handler stores the forward
::Service in ams::sf::impl::g_vitap_pending_mitm_forward right before
out.SetValue(); SetOutObjectImpl consumes it on the same call stack and calls
RegisterMitmSession instead.

Same technique as Switch Frame Tap's tier4/applet-mitm/patch_libstrat.py
(GPL-2.0, Tomas Uribe). Only the forwarding part is needed here: vi-tap runs
in the System resource-limit group, so the Applet group's 6-session limit
(their session hook) does not apply.

Usage: patch_libstrat.py <Atmosphere tree>. Idempotent; the header is only
rewritten when it changes, so make does not rebuild libstratosphere for
nothing.
"""
import pathlib
import sys

F = pathlib.Path(sys.argv[1]) / "libraries/libstratosphere/include/stratosphere/sf/impl/sf_impl_command_serialization.hpp"
src = F.read_text()
MARK = "g_vitap_pending_mitm_forward"

if MARK in src:
    print("patch_libstrat: already applied")
    sys.exit(0)

anchor = "#include <stratosphere/sf/hipc/sf_hipc_server_session_manager.hpp>\n"
assert anchor in src, "anchor include not found - libstratosphere changed, review the patch"
src = src.replace(anchor, anchor + (
    "\n/* --- vi-tap patch: non-domain mitm sub-object forwarding side channel --- */\n"
    "#if AMS_SF_MITM_SUPPORTED\n"
    "#include <memory>\n"
    "namespace ams::sf::impl { inline thread_local ::std::shared_ptr<::Service> g_vitap_pending_mitm_forward{}; }\n"
    "#endif\n"
), 1)

old = "                R_ABORT_UNLESS(manager->RegisterSession(server_handle, std::move(object)));\n"
new = (
    "#if AMS_SF_MITM_SUPPORTED\n"
    "                if (auto _fwd = std::move(::ams::sf::impl::g_vitap_pending_mitm_forward); _fwd != nullptr) {\n"
    "                    R_ABORT_UNLESS(manager->RegisterMitmSession(server_handle, std::move(object), std::move(_fwd)));\n"
    "                } else\n"
    "#endif\n"
    "                {\n"
    "                    R_ABORT_UNLESS(manager->RegisterSession(server_handle, std::move(object)));\n"
    "                }\n"
)
assert old in src, "RegisterSession line not found - libstratosphere changed, review the patch"
src = src.replace(old, new, 1)

F.write_text(src)
print("patch_libstrat: forwarding side channel applied")
