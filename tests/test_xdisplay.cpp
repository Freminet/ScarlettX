#include <iostream>

#include "scarlettx/gdk/xgameruntime_abi.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "[test_xdisplay] FAILED: " #cond                        \
                      << " at line " << __LINE__ << "\n";                        \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int main() {
    // The HLE output is SDR: HDR is reported as disabled and the info is zeroed.
    XDisplayHdrModeInfo info{1.f, 2.f, 3.f};
    CHECK(XDisplayTryEnableHdrMode(XDisplayHdrModePreference_PreferHdr, &info) ==
          XDisplayHdrModeResult_Disabled);
    CHECK(info.minToneMapLuminance == 0.f);
    CHECK(info.maxToneMapLuminance == 0.f);
    CHECK(info.maxFullFrameToneMapLuminance == 0.f);

    // The info structure is optional.
    CHECK(XDisplayTryEnableHdrMode(XDisplayHdrModePreference_PreferRefreshRate, nullptr) ==
          XDisplayHdrModeResult_Disabled);

    // Timeout deferrals: each acquire hands out its own handle.
    CHECK(XDisplayAcquireTimeoutDeferral(nullptr) == XGR_E_INVALIDARG);

    XDisplayTimeoutDeferralHandle h1 = nullptr;
    XDisplayTimeoutDeferralHandle h2 = nullptr;
    CHECK(XDisplayAcquireTimeoutDeferral(&h1) == XGR_S_OK);
    CHECK(h1 != nullptr);
    CHECK(XDisplayAcquireTimeoutDeferral(&h2) == XGR_S_OK);
    CHECK(h2 != nullptr);
    CHECK(h1 != h2);

    XDisplayCloseTimeoutDeferralHandle(h1);
    XDisplayCloseTimeoutDeferralHandle(h2);
    XDisplayCloseTimeoutDeferralHandle(nullptr);

    std::cout << "[test_xdisplay] all checks passed\n";
    return 0;
}
