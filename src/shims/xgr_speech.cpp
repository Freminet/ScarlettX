#include "scarlettx/gdk/xgameruntime_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// XSpeechSynthesizer HLE: there is no speech engine, so no voice is installed and
// every produced stream is empty. A title that enumerates voices sees none and a
// stream it asks for is a valid handle of size zero, which is the same shape the
// real API gives for silent output. Signatures from the public pages under
// learn.microsoft.com/.../reference/system/xspeechsynthesizer (gdk-2610).
//
// XSpeechSynthesizerVoiceInformation is only ever received by a callback, never
// built here: the type is left incomplete on purpose, because its fields are not
// public and no voice is enumerated.

namespace {

bool TraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

constexpr uint32_t kSynthesizerMagic = 0x58535359;  // 'XSSY'
constexpr uint32_t kStreamMagic = 0x58535354;       // 'XSST'

}  // namespace

struct XSpeechSynthesizer {
    uint32_t magic = kSynthesizerMagic;
};

struct XSpeechSynthesizerStream {
    uint32_t magic = kStreamMagic;
};

namespace {

bool ValidSynthesizer(XSpeechSynthesizerHandle handle) {
    return handle != nullptr && handle->magic == kSynthesizerMagic;
}

bool ValidStream(XSpeechSynthesizerStreamHandle handle) {
    return handle != nullptr && handle->magic == kStreamMagic;
}

}  // namespace

XGameResult XSpeechSynthesizerCreate(XSpeechSynthesizerHandle* speechSynthesizer) {
    if (!speechSynthesizer) {
        return XGR_E_INVALIDARG;
    }
    auto* handle = new (std::nothrow) XSpeechSynthesizer();
    if (!handle) {
        return XGR_E_OUTOFMEMORY;
    }
    *speechSynthesizer = handle;
    if (TraceEnabled()) {
        std::fprintf(stderr, "[ScarlettX][speech] XSpeechSynthesizerCreate -> %p\n",
                     static_cast<void*>(handle));
    }
    return XGR_S_OK;
}

XGameResult XSpeechSynthesizerCloseHandle(XSpeechSynthesizerHandle speechSynthesizer) {
    if (!speechSynthesizer) {
        return XGR_S_OK;  // _In_opt_: a null handle is already closed.
    }
    if (speechSynthesizer->magic != kSynthesizerMagic) {
        return XGR_E_INVALIDARG;  // not one of ours (or already closed): do not free it again
    }
    speechSynthesizer->magic = 0;
    delete speechSynthesizer;
    return XGR_S_OK;
}

XGameResult XSpeechSynthesizerCreateStreamFromText(
    XSpeechSynthesizerHandle speechSynthesizer,
    const char* text,
    XSpeechSynthesizerStreamHandle* speechSynthesisStream) {
    if (!ValidSynthesizer(speechSynthesizer) || !text || !speechSynthesisStream) {
        return XGR_E_INVALIDARG;
    }
    auto* stream = new (std::nothrow) XSpeechSynthesizerStream();
    if (!stream) {
        return XGR_E_OUTOFMEMORY;
    }
    *speechSynthesisStream = stream;
    return XGR_S_OK;
}

XGameResult XSpeechSynthesizerCreateStreamFromSsml(
    XSpeechSynthesizerHandle speechSynthesizer,
    const char* ssml,
    XSpeechSynthesizerStreamHandle* speechSynthesisStream) {
    // SSML is just text to this HLE: the same empty stream comes back.
    return XSpeechSynthesizerCreateStreamFromText(speechSynthesizer, ssml, speechSynthesisStream);
}

XGameResult XSpeechSynthesizerCloseStreamHandle(XSpeechSynthesizerStreamHandle speechSynthesisStream) {
    if (!speechSynthesisStream) {
        return XGR_S_OK;
    }
    if (speechSynthesisStream->magic != kStreamMagic) {
        return XGR_E_INVALIDARG;  // not one of ours (or already closed): do not free it again
    }
    speechSynthesisStream->magic = 0;
    delete speechSynthesisStream;
    return XGR_S_OK;
}

XGameResult XSpeechSynthesizerGetStreamDataSize(
    XSpeechSynthesizerStreamHandle speechSynthesisStream,
    size_t* bufferSize) {
    if (!ValidStream(speechSynthesisStream) || !bufferSize) {
        return XGR_E_INVALIDARG;
    }
    *bufferSize = 0;
    return XGR_S_OK;
}

XGameResult XSpeechSynthesizerGetStreamData(
    XSpeechSynthesizerStreamHandle speechSynthesisStream,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed) {
    if (!ValidStream(speechSynthesisStream)) {
        // Documented: an invalid stream handle is HRESULT_FROM_WIN32(ERROR_NOT_FOUND).
        return static_cast<XGameResult>(0x80070490u);
    }
    if (bufferSize > 0 && !buffer) {
        return XGR_E_INVALIDARG;
    }
    // Nothing to copy: the stream is empty, so any buffer stays untouched.
    if (bufferUsed) {
        *bufferUsed = 0;
    }
    return XGR_S_OK;
}

XGameResult XSpeechSynthesizerSetDefaultVoice(XSpeechSynthesizerHandle speechSynthesizer) {
    if (!ValidSynthesizer(speechSynthesizer)) {
        return XGR_E_INVALIDARG;
    }
    // There is no voice to select; the call is accepted so a title's setup path
    // does not fail just because this machine has no speech engine.
    return XGR_S_OK;
}

XGameResult XSpeechSynthesizerSetCustomVoice(XSpeechSynthesizerHandle speechSynthesizer,
                                             const char* voiceId) {
    if (!ValidSynthesizer(speechSynthesizer) || !voiceId) {
        return XGR_E_INVALIDARG;
    }
    // No voice is ever enumerated, so no voice ID can be valid.
    return XGR_E_INVALIDARG;
}

XGameResult XSpeechSynthesizerEnumerateInstalledVoices(
    void* context,
    XSpeechSynthesizerInstalledVoicesCallback* callback) {
    if (!callback) {
        return XGR_E_INVALIDARG;
    }
    // No voices are installed: the callback is never invoked.
    (void)context;
    return XGR_S_OK;
}
