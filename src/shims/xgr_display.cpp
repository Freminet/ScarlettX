#include "scarlettx/gdk/xgameruntime_abi.h"

#include <new>

// XDisplay HLE: the emulated display is SDR-only, so HDR is never enabled, and
// screen-timeout deferrals are plain handles because nothing ever times out.
// Signatures and semantics follow the public XDisplay.h reference pages
// (learn.microsoft.com, gdk-2610).

namespace {
constexpr uint32_t kDeferralMagic = 0x58445444;  // 'XDTD'
}  // namespace

struct XDisplayTimeoutDeferral {
    uint32_t magic = kDeferralMagic;
};

XDisplayHdrModeResult XDisplayTryEnableHdrMode(
    XDisplayHdrModePreference /*displayModePreference*/,
    XDisplayHdrModeInfo* displayHdrModeInfo) {
    if (displayHdrModeInfo) {
        displayHdrModeInfo->minToneMapLuminance = 0.f;
        displayHdrModeInfo->maxToneMapLuminance = 0.f;
        displayHdrModeInfo->maxFullFrameToneMapLuminance = 0.f;
    }
    return XDisplayHdrModeResult_Disabled;
}

XGameResult XDisplayAcquireTimeoutDeferral(XDisplayTimeoutDeferralHandle* handle) {
    if (!handle) {
        return XGR_E_INVALIDARG;
    }
    auto* deferral = new (std::nothrow) XDisplayTimeoutDeferral();
    if (!deferral) {
        return XGR_E_OUTOFMEMORY;
    }
    *handle = deferral;
    return XGR_S_OK;
}

void XDisplayCloseTimeoutDeferralHandle(XDisplayTimeoutDeferralHandle handle) {
    if (handle && handle->magic == kDeferralMagic) {
        handle->magic = 0;
        delete handle;
    }
}
