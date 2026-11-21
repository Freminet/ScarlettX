#include <iostream>

#include "scarlettx/gdk/xgameruntime_abi.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "[xaccessibility] FAILED: " #cond                          \
                      << " at line " << __LINE__ << "\n";                        \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int main() {
        // XAccessibility: there is no TV setting and no speech recogniser, so the
        // state is fixed and the strings are accepted without being drawn.
        {
            XHighContrastMode mode = XHighContrastMode_Dark;
            CHECK(XHighContrastGetMode(&mode) == XGR_S_OK);
            CHECK(mode == XHighContrastMode_Off);
            CHECK(XHighContrastGetMode(nullptr) == XGR_E_POINTER);

            CHECK(XClosedCaptionSetEnabled(true) == XGR_S_OK);
            CHECK(XClosedCaptionSetEnabled(false) == XGR_S_OK);

            uint32_t hypothesis = 0;
            CHECK(XSpeechToTextBeginHypothesisString("player", "helo",
                                                     XSpeechToTextType_Voice, &hypothesis) ==
                  XGR_S_OK);
            CHECK(hypothesis != 0);
            CHECK(XSpeechToTextUpdateHypothesisString(hypothesis, "hello") == XGR_S_OK);
            CHECK(XSpeechToTextFinalizeHypothesisString(hypothesis, "hello") == XGR_S_OK);
            // The id is gone once finalised, and an invalid id is documented as not
            // being an error.
            CHECK(XSpeechToTextUpdateHypothesisString(hypothesis, "x") == XGR_S_OK);
            CHECK(XSpeechToTextFinalizeHypothesisString(hypothesis, "x") == XGR_S_OK);

            uint32_t cancelled = 0;
            CHECK(XSpeechToTextBeginHypothesisString("player", "abc",
                                                     XSpeechToTextType_Text, &cancelled) == XGR_S_OK);
            CHECK(XSpeechToTextCancelHypothesisString(cancelled) == XGR_S_OK);
            CHECK(XSpeechToTextCancelHypothesisString(cancelled) == XGR_S_OK);

            CHECK(XSpeechToTextBeginHypothesisString(nullptr, "x", XSpeechToTextType_Voice,
                                                     &hypothesis) == XGR_E_INVALIDARG);
            CHECK(XSpeechToTextBeginHypothesisString("p", "x", XSpeechToTextType_Voice, nullptr) ==
                  XGR_E_INVALIDARG);
            CHECK(XSpeechToTextBeginHypothesisString("p", "x", static_cast<XSpeechToTextType>(7),
                                                     &hypothesis) == XGR_E_INVALIDARG);
            CHECK(XSpeechToTextSendString("player", "hello", XSpeechToTextType_Voice) == XGR_S_OK);
            CHECK(XSpeechToTextSendString(nullptr, "hello", XSpeechToTextType_Voice) ==
                  XGR_E_INVALIDARG);
            CHECK(XSpeechToTextSetPositionHint(XSpeechToTextPositionHint_TopLeft) == XGR_S_OK);
            CHECK(XSpeechToTextSetPositionHint(static_cast<XSpeechToTextPositionHint>(9)) ==
                  XGR_E_INVALIDARG);
        }

    std::cout << "[xaccessibility] all checks passed\n";
    return 0;
}
