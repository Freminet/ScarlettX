#include <iostream>

#include "scarlettx/gdk/xgameruntime_abi.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "[xspeechsynthesizer] FAILED: " #cond                          \
                      << " at line " << __LINE__ << "\n";                        \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

bool SCARLETTX_GDK_CALLBACK CountVoice(const XSpeechSynthesizerVoiceInformation* /*information*/,
                                       void* context) {
    ++*static_cast<int*>(context);
    return true;
}

}  // namespace

int main() {
        // XSpeechSynthesizer: no voice is installed and every stream is empty, so a
        // title's setup succeeds and reads back zero bytes of speech.
        {
            XSpeechSynthesizerHandle synthesizer = nullptr;
            CHECK(XSpeechSynthesizerCreate(&synthesizer) == XGR_S_OK);
            CHECK(synthesizer != nullptr);
            CHECK(XSpeechSynthesizerCreate(nullptr) == XGR_E_INVALIDARG);

            XSpeechSynthesizerStreamHandle stream = nullptr;
            CHECK(XSpeechSynthesizerCreateStreamFromText(synthesizer, "hello", &stream) == XGR_S_OK);
            CHECK(stream != nullptr);
            size_t size = 123;
            CHECK(XSpeechSynthesizerGetStreamDataSize(stream, &size) == XGR_S_OK && size == 0);
            size_t used = 7;
            CHECK(XSpeechSynthesizerGetStreamData(stream, 0, nullptr, &used) == XGR_S_OK && used == 0);
            CHECK(XSpeechSynthesizerGetStreamData(stream, 8, nullptr, &used) == XGR_E_INVALIDARG);
            // An invalid stream handle is documented as HRESULT_FROM_WIN32(ERROR_NOT_FOUND).
            CHECK(XSpeechSynthesizerGetStreamData(nullptr, 0, nullptr, &used) ==
                  static_cast<XGameResult>(0x80070490u));
            CHECK(XSpeechSynthesizerGetStreamDataSize(stream, nullptr) == XGR_E_INVALIDARG);

            XSpeechSynthesizerStreamHandle ssml = nullptr;
            CHECK(XSpeechSynthesizerCreateStreamFromSsml(synthesizer, "<speak/>", &ssml) == XGR_S_OK);
            CHECK(XSpeechSynthesizerCloseStreamHandle(ssml) == XGR_S_OK);
            CHECK(XSpeechSynthesizerCloseStreamHandle(nullptr) == XGR_S_OK);

            CHECK(XSpeechSynthesizerSetDefaultVoice(synthesizer) == XGR_S_OK);
            CHECK(XSpeechSynthesizerSetCustomVoice(synthesizer, "voice") == XGR_E_INVALIDARG);
            CHECK(XSpeechSynthesizerSetCustomVoice(synthesizer, nullptr) == XGR_E_INVALIDARG);

            int voices = 0;
            CHECK(XSpeechSynthesizerEnumerateInstalledVoices(&voices, CountVoice) == XGR_S_OK);
            CHECK(voices == 0);  // the callback must not fire
            CHECK(XSpeechSynthesizerEnumerateInstalledVoices(&voices, nullptr) == XGR_E_INVALIDARG);

            CHECK(XSpeechSynthesizerCloseStreamHandle(stream) == XGR_S_OK);
            CHECK(XSpeechSynthesizerCloseHandle(synthesizer) == XGR_S_OK);
            CHECK(XSpeechSynthesizerCloseHandle(nullptr) == XGR_S_OK);

            // A pointer that does not carry our magic is not ours: closing it is
            // refused instead of freeing memory the allocator does not own.
            uint64_t foreign[4] = {0, 0, 0, 0};
            CHECK(XSpeechSynthesizerCloseHandle(
                      reinterpret_cast<XSpeechSynthesizerHandle>(foreign)) == XGR_E_INVALIDARG);
            CHECK(XSpeechSynthesizerCloseStreamHandle(
                      reinterpret_cast<XSpeechSynthesizerStreamHandle>(foreign)) ==
                  XGR_E_INVALIDARG);
        }

    std::cout << "[xspeechsynthesizer] all checks passed\n";
    return 0;
}
