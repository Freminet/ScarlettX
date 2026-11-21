#include "scarlettx/gdk/xgameruntime_abi.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

// XAccessibility HLE: the emulator has no television settings and no speech
// recogniser, so closed captions report the fixed "everything off" state and the
// speech-to-text strings a title submits are accepted without ever being drawn.
// Signatures and enum values from the public pages under
// learn.microsoft.com/.../reference/system/xaccessibility (gdk-2610).
//
// Not implemented here, and why: XClosedCaptionGetProperties needs the
// XClosedCaptionProperties structure, which contains XColor — and XColor has no
// public page, so the layout cannot be reproduced without inventing it.

namespace {

bool TraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

std::mutex g_state_mutex;
bool g_captions_enabled = false;
XSpeechToTextPositionHint g_position = XSpeechToTextPositionHint_BottomCenter;
uint32_t g_next_hypothesis = 0;
std::unordered_set<uint32_t> g_hypotheses;

bool ValidPosition(XSpeechToTextPositionHint position) {
    return static_cast<uint32_t>(position) <=
           static_cast<uint32_t>(XSpeechToTextPositionHint_TopRight);
}

bool ValidType(XSpeechToTextType type) {
    return type == XSpeechToTextType_Voice || type == XSpeechToTextType_Text;
}

}  // namespace

XGameResult XClosedCaptionSetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_captions_enabled = enabled;
    if (TraceEnabled()) {
        std::fprintf(stderr, "[ScarlettX][xaccessibility] XClosedCaptionSetEnabled(%d)\n",
                     enabled ? 1 : 0);
    }
    return XGR_S_OK;
}

XGameResult XHighContrastGetMode(XHighContrastMode* mode) {
    if (!mode) {
        return XGR_E_POINTER;
    }
    // The console reads this from the TV/display settings; there are none here.
    *mode = XHighContrastMode_Off;
    return XGR_S_OK;
}

XGameResult XSpeechToTextBeginHypothesisString(
    const char* speakerName,
    const char* content,
    XSpeechToTextType type,
    uint32_t* hypothesisId) {
    if (!speakerName || !content || !hypothesisId) {
        return XGR_E_INVALIDARG;
    }
    if (!ValidType(type)) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const uint32_t id = ++g_next_hypothesis;
    g_hypotheses.insert(id);
    *hypothesisId = id;
    return XGR_S_OK;
}

XGameResult XSpeechToTextUpdateHypothesisString(uint32_t hypothesisId, const char* content) {
    if (!content) {
        return XGR_E_INVALIDARG;
    }
    // "Passing an invalid hypothesisId value does not return an error": an expired
    // or unknown id is simply ignored.
    return XGR_S_OK;
}

XGameResult XSpeechToTextFinalizeHypothesisString(uint32_t hypothesisId, const char* content) {
    if (!content) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_hypotheses.erase(hypothesisId);  // an invalid id is documented as not an error
    return XGR_S_OK;
}

XGameResult XSpeechToTextCancelHypothesisString(uint32_t hypothesisId) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_hypotheses.erase(hypothesisId);  // an invalid id is documented as not an error
    return XGR_S_OK;
}

XGameResult XSpeechToTextSendString(const char* speakerName, const char* content,
                                    XSpeechToTextType type) {
    if (!speakerName || !content) {
        return XGR_E_INVALIDARG;
    }
    if (!ValidType(type)) {
        return XGR_E_INVALIDARG;
    }
    // The string would be drawn by the console's speech-to-text window.
    if (TraceEnabled()) {
        std::fprintf(stderr, "[ScarlettX][xaccessibility] XSpeechToTextSendString(%s: %s)\n",
                     speakerName, content);
    }
    return XGR_S_OK;
}

XGameResult XSpeechToTextSetPositionHint(XSpeechToTextPositionHint position) {
    if (!ValidPosition(position)) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_position = position;
    return XGR_S_OK;
}
