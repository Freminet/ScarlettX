#include <iostream>

#include "scarlettx/gdk/xgameruntime_abi.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "[xgameinvite] FAILED: " #cond                          \
                      << " at line " << __LINE__ << "\n";                        \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int main() {
        // XGameInvite: the registration is accepted, never fires, and the token is
        // good exactly once.
        {
            XTaskQueueRegistrationToken token{};
            CHECK(XGameInviteRegisterForEvent(nullptr, nullptr, nullptr, &token) == XGR_E_INVALIDARG);
            CHECK(XGameInviteRegisterForEvent(nullptr, nullptr,
                                              reinterpret_cast<XGameInviteEventCallback*>(0x1),
                                              &token) == XGR_S_OK);
            CHECK(token.token != 0);
            CHECK(XGameInviteUnregisterForEvent(token, true));
            CHECK(XGameInviteUnregisterForEvent(token, true) == false);

            XTaskQueueRegistrationToken second{};
            CHECK(XGameInviteRegisterForEvent(nullptr, nullptr,
                                              reinterpret_cast<XGameInviteEventCallback*>(0x1),
                                              &second) == XGR_S_OK);
            CHECK(second.token != token.token);
            CHECK(XGameInviteUnregisterForEvent(second, false));
            CHECK(XGameInviteRegisterForEvent(nullptr, nullptr,
                                              reinterpret_cast<XGameInviteEventCallback*>(0x1),
                                              nullptr) == XGR_E_INVALIDARG);
        }

    std::cout << "[xgameinvite] all checks passed\n";
    return 0;
}
