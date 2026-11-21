#include "scarlettx/gdk/xgameruntime_abi.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

// XGameInvite HLE: no invite can arrive, because the emulator never joins a
// multiplayer session. Registering hands out a valid task-queue token, the
// callback is never invoked, and unregistering accepts the token exactly once —
// the same contract as the XGameSave package-installed registrations.
// Signatures from the public pages under
// learn.microsoft.com/.../reference/system/xgameinvite/functions (gdk-2610).
//
// Both entry points are marked deprecated on their public pages ("Use
// XGameActivationRegisterForEvent instead"); XGameActivation is not part of our
// surface.

namespace {

bool TraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

std::mutex g_tokens_mutex;
std::unordered_set<uint64_t> g_tokens;
uint64_t g_next_token = 0;

uint64_t NextTokenLocked() { return ++g_next_token; }

}  // namespace

XGameResult XGameInviteRegisterForEvent(
    XTaskQueueHandle queue,
    void* context,
    XGameInviteEventCallback* callback,
    XTaskQueueRegistrationToken* token) {
    (void)queue;
    (void)context;
    if (!callback || !token) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(g_tokens_mutex);
    token->token = NextTokenLocked();
    g_tokens.insert(token->token);
    if (TraceEnabled()) {
        std::fprintf(stderr, "[ScarlettX][xgameinvite] RegisterForEvent -> token %llu\n",
                     static_cast<unsigned long long>(token->token));
    }
    return XGR_S_OK;
}

bool XGameInviteUnregisterForEvent(XTaskQueueRegistrationToken token, bool wait) {
    (void)wait;  // No callback is ever pending, so there is never anything to wait for.
    std::lock_guard<std::mutex> lock(g_tokens_mutex);
    return g_tokens.erase(token.token) > 0;
}
