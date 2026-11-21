#include "scarlettx/gdk/xgameruntime_abi.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "xgr_task_queue.h"

// XLauncher and XGameUI HLE. Nothing is ever drawn: the system windows complete
// their XAsyncBlock immediately, as if the player had dismissed them. Signatures
// and semantics follow the public XLauncher.h / XGameUI.h reference pages
// (learn.microsoft.com, gdk-2610).

namespace {

bool TraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

using XAsyncState = xgr::HleAsyncState;

XGameResult BeginAsyncOperation(XAsyncBlock* async,
                                std::function<void(XAsyncState*)> work_fn) {
    return xgr::BeginHleAsync(async, std::move(work_fn));
}

void CompleteAsyncState(XAsyncState* state, XGameResult hr, size_t result_size = 0) {
    xgr::CompleteHleAsync(state, hr, result_size);
}

std::shared_ptr<XAsyncState> GetAsyncState(XAsyncBlock* async) {
    return xgr::GetHleAsyncState(async);
}

}  // namespace

// ---------------------------------------------------------------------------
// XLauncher HLE
// ---------------------------------------------------------------------------
XGameResult XLaunchUri(XUserHandle /*requestingUser*/, const char* uri) {
    if (!uri || uri[0] == '\0') {
        return XGR_E_INVALIDARG;
    }
    if (TraceEnabled()) {
        fprintf(stderr, "[ScarlettX][xgameruntime] XLaunchUri(%s) ignored\n", uri);
    }
    return XGR_S_OK;
}

// ---------------------------------------------------------------------------
// XGameUI HLE
// ---------------------------------------------------------------------------
XGameResult XGameUiShowMessageDialogAsync(
    XAsyncBlock* async,
    const char* titleText,
    const char* contentText,
    const char* /*firstButtonText*/,
    const char* /*secondButtonText*/,
    const char* /*thirdButtonText*/,
    XGameUiMessageDialogButton defaultButton,
    XGameUiMessageDialogButton cancelButton) {
    if (!async || !titleText || !contentText) {
        return XGR_E_INVALIDARG;
    }
    if (static_cast<uint32_t>(defaultButton) > 2 || static_cast<uint32_t>(cancelButton) > 2) {
        return XGR_E_INVALIDARG;
    }
    if (TraceEnabled()) {
        fprintf(stderr, "[ScarlettX][xgameruntime] XGameUiShowMessageDialogAsync(\"%s\", \"%s\")\n",
                titleText, contentText);
    }
    uint32_t chosen = static_cast<uint32_t>(defaultButton);
    return BeginAsyncOperation(async, [chosen](XAsyncState* st) {
        st->result_payload.resize(sizeof(uint32_t));
        memcpy(st->result_payload.data(), &chosen, sizeof(uint32_t));
        CompleteAsyncState(st, XGR_S_OK, sizeof(uint32_t));
    });
}

XGameResult XGameUiShowMessageDialogResult(XAsyncBlock* async, XGameUiMessageDialogButton* resultButton) {
    if (!async || !resultButton) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state || state->result_payload.size() < 4) {
        return XGR_E_FAIL;
    }
    uint32_t val;
    memcpy(&val, state->result_payload.data(), sizeof(uint32_t));
    *resultButton = static_cast<XGameUiMessageDialogButton>(val);
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

namespace {
    std::atomic<uint32_t> g_ui_notification_pos{0};

    XGameResult CompleteUiImmediately(XAsyncBlock* async) {
        return BeginAsyncOperation(async, [](XAsyncState* st) {
            CompleteAsyncState(st, XGR_S_OK, 0);
        });
    }

    XGameResult UiResult(XAsyncBlock* async) {
        if (!async) return XGR_E_INVALIDARG;
        return XAsyncGetStatus(async, true);
    }
} // namespace

XGameResult XGameUiSetNotificationPositionHint(XGameUiNotificationPositionHint position) {
    if (static_cast<uint32_t>(position) > 5) return XGR_E_INVALIDARG;
    g_ui_notification_pos.store(static_cast<uint32_t>(position));
    return XGR_S_OK;
}

XGameResult XGameUiShowAchievementsAsync(XAsyncBlock* async, XUserHandle requestingUser, uint32_t /*titleId*/) {
    if (!async || !requestingUser) return XGR_E_INVALIDARG;
    return CompleteUiImmediately(async);
}

XGameResult XGameUiShowAchievementsResult(XAsyncBlock* async) {
    return UiResult(async);
}

XGameResult XGameUiShowPlayerProfileCardAsync(XAsyncBlock* async, XUserHandle requestingUser, uint64_t /*targetPlayer*/) {
    if (!async || !requestingUser) return XGR_E_INVALIDARG;
    return CompleteUiImmediately(async);
}

XGameResult XGameUiShowPlayerProfileCardResult(XAsyncBlock* async) {
    return UiResult(async);
}

XGameResult XGameUiShowSendGameInviteAsync(
    XAsyncBlock* async, XUserHandle requestingUser,
    const char* sessionConfigurationId, const char* sessionTemplateName,
    const char* sessionId, const char* /*invitationText*/, const char* /*customActivationContext*/) {
    if (!async || !requestingUser || !sessionConfigurationId || !sessionTemplateName || !sessionId)
        return XGR_E_INVALIDARG;
    return CompleteUiImmediately(async);
}

XGameResult XGameUiShowSendGameInviteResult(XAsyncBlock* async) {
    return UiResult(async);
}

XGameResult XGameUiShowMultiplayerActivityGameInviteAsync(XAsyncBlock* async, XUserHandle requestingUser) {
    if (!async || !requestingUser) return XGR_E_INVALIDARG;
    return CompleteUiImmediately(async);
}

XGameResult XGameUiShowMultiplayerActivityGameInviteResult(XAsyncBlock* async) {
    return UiResult(async);
}

XGameResult XGameUiShowStateShareAsync(XAsyncBlock* async, XUserHandle requestingUser, const char* linkToken) {
    if (!async || !requestingUser || !linkToken) return XGR_E_INVALIDARG;
    return CompleteUiImmediately(async);
}

XGameResult XGameUiShowStateShareResult(XAsyncBlock* async) {
    return UiResult(async);
}

XGameResult XGameUiShowErrorDialogAsync(
    XAsyncBlock* async,
    int32_t errorCode,
    const char* context) {
    if (!async) return XGR_E_INVALIDARG;
    if (TraceEnabled()) {
        fprintf(stderr, "[ScarlettX][xgameruntime] XGameUiShowErrorDialogAsync(0x%08x, \"%s\")\n",
                static_cast<uint32_t>(errorCode), context ? context : "nullptr");
    }
    return BeginAsyncOperation(async, [](XAsyncState* st) {
        CompleteAsyncState(st, XGR_S_OK, 0);
    });
}

XGameResult XGameUiShowErrorDialogResult(XAsyncBlock* async) {
    if (!async) return XGR_E_INVALIDARG;
    return XAsyncGetStatus(async, true);
}

XGameResult XGameUiShowPlayerPickerAsync(
    XAsyncBlock* async,
    XUserHandle requestingUser,
    const char* promptText,
    uint32_t selectFromPlayersCount,
    const uint64_t* selectFromPlayers,
    uint32_t preSelectedPlayersCount,
    const uint64_t* preSelectedPlayers,
    uint32_t minSelectionCount,
    uint32_t maxSelectionCount) {
    if (!async || !requestingUser || !promptText) return XGR_E_INVALIDARG;
    if (selectFromPlayersCount > 0 && !selectFromPlayers) return XGR_E_INVALIDARG;
    if (preSelectedPlayersCount > 0 && !preSelectedPlayers) return XGR_E_INVALIDARG;
    if (minSelectionCount > maxSelectionCount) return XGR_E_INVALIDARG;

    uint32_t count = preSelectedPlayersCount < maxSelectionCount ? preSelectedPlayersCount : maxSelectionCount;
    std::vector<uint64_t> selected(preSelectedPlayers, preSelectedPlayers + count);
    return BeginAsyncOperation(async, [selected](XAsyncState* st) {
        st->result_payload.resize(selected.size() * 8);
        for (size_t i = 0; i < selected.size(); ++i) {
            memcpy(st->result_payload.data() + i * 8, &selected[i], 8);
        }
        CompleteAsyncState(st, XGR_S_OK, st->result_payload.size());
    });
}

XGameResult XGameUiShowPlayerPickerResultCount(XAsyncBlock* async, uint32_t* resultPlayersCount) {
    if (!async || !resultPlayersCount) return XGR_E_INVALIDARG;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) return hr;
    auto state = GetAsyncState(async);
    *resultPlayersCount = state ? static_cast<uint32_t>(state->result_payload.size() / 8) : 0;
    return XGR_S_OK;
}

XGameResult XGameUiShowPlayerPickerResult(
    XAsyncBlock* async,
    uint32_t resultPlayersCount,
    uint64_t* resultPlayers,
    uint32_t* resultPlayersUsed) {
    if (!async) return XGR_E_INVALIDARG;
    if (resultPlayersCount > 0 && !resultPlayers) return XGR_E_INVALIDARG;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) return hr;
    auto state = GetAsyncState(async);
    uint32_t n = state ? static_cast<uint32_t>(state->result_payload.size() / 8) : 0;
    if (resultPlayersCount < n) return XGR_E_NOT_SUFFICIENT_BUFFER;
    for (uint32_t i = 0; i < n; ++i) {
        memcpy(&resultPlayers[i], state->result_payload.data() + i * 8, 8);
    }
    if (resultPlayersUsed) *resultPlayersUsed = n;
    if (state) xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult XGameUiShowTextEntryAsync(
    XAsyncBlock* async,
    const char* titleText,
    const char* descriptionText,
    const char* defaultText,
    XGameUiTextEntryInputScope inputScope,
    uint32_t maxTextLength) {
    if (!async) return XGR_E_INVALIDARG;
    uint32_t limit = (maxTextLength == 0 || inputScope == XGameUiTextEntryInputScope_Password) ? 1024 : maxTextLength;
    std::string text;
    if (defaultText) {
        size_t len = strlen(defaultText);
        if (len > limit) len = limit;
        text.assign(defaultText, len);
    }
    return BeginAsyncOperation(async, [text](XAsyncState* st) {
        st->result_payload.resize(text.size() + 1);
        memcpy(st->result_payload.data(), text.c_str(), text.size());
        st->result_payload[text.size()] = '\0';
        CompleteAsyncState(st, XGR_S_OK, st->result_payload.size());
    });
}

XGameResult XGameUiShowTextEntryResultSize(XAsyncBlock* async, uint32_t* resultTextBufferSize) {
    if (!async || !resultTextBufferSize) return XGR_E_INVALIDARG;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) return hr;
    auto state = GetAsyncState(async);
    *resultTextBufferSize = state ? static_cast<uint32_t>(state->result_payload.size()) : 0;
    return XGR_S_OK;
}

XGameResult XGameUiShowTextEntryResult(
    XAsyncBlock* async,
    uint32_t resultTextBufferSize,
    char* resultTextBuffer,
    uint32_t* resultTextBufferUsed) {
    if (!async) return XGR_E_INVALIDARG;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) return hr;
    auto state = GetAsyncState(async);
    if (!state) return XGR_E_FAIL;
    uint32_t n = static_cast<uint32_t>(state->result_payload.size());
    if (!resultTextBuffer || resultTextBufferSize < n) return XGR_E_NOT_SUFFICIENT_BUFFER;
    memcpy(resultTextBuffer, state->result_payload.data(), n);
    if (resultTextBufferUsed) *resultTextBufferUsed = n;
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

// ---------------------------------------------------------------------------
// XGameUI in-game virtual keyboard (HLE: no window, the text never changes)
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kTextEntryMagic = 0x584B4559;  // 'XKEY'
constexpr uint32_t kTextEntryMaxLength = 32 * 1024;

}  // namespace

struct XGameUiTextEntry {
    uint32_t magic = kTextEntryMagic;
    std::string text;
    uint32_t cursor = 0;
    XGameUiTextEntryPositionHint position = XGameUiTextEntryPositionHint_Bottom;
    XGameUiTextEntryVisibilityFlags visibility = XGameUiTextEntryVisibilityFlags_Default;
    XGameUiTextEntryInputScope inputScope = XGameUiTextEntryInputScope_Default;
};

namespace {

bool IsTextEntryHandle(XGameUiTextEntryHandle handle) {
    return handle != nullptr && handle->magic == kTextEntryMagic;
}

}  // namespace

XGameResult XGameUiTextEntryOpen(
    const XGameUiTextEntryOptions* options,
    uint32_t maxLength,
    const char* initialText,
    uint32_t initialCursorIndex,
    XGameUiTextEntryHandle* handle) {
    if (!options || !handle) {
        return XGR_E_INVALIDARG;
    }
    // The page for XGameUiTextEntryOpen states that maxLength must not exceed 32K.
    if (maxLength > kTextEntryMaxLength) {
        return XGR_E_INVALIDARG;
    }
    auto* entry = new (std::nothrow) XGameUiTextEntry();
    if (!entry) {
        return XGR_E_OUTOFMEMORY;
    }
    if (initialText) {
        entry->text.assign(initialText, strnlen(initialText, maxLength));
    }
    entry->cursor = initialCursorIndex <= entry->text.size()
                        ? initialCursorIndex
                        : static_cast<uint32_t>(entry->text.size());
    entry->position = options->positionHint;
    entry->visibility = options->visibilityflags;
    entry->inputScope = options->inputScope;
    *handle = entry;
    return XGR_S_OK;
}

XGameResult XGameUiTextEntryClose(XGameUiTextEntryHandle handle) {
    if (!IsTextEntryHandle(handle)) {
        // The public pages document no error for a stale handle; a null one is
        // treated as already closed so a title's cleanup path stays simple.
        return handle == nullptr ? XGR_S_OK : XGR_E_INVALIDARG;
    }
    handle->magic = 0;
    delete handle;
    return XGR_S_OK;
}

XGameResult XGameUiTextEntryGetState(
    XGameUiTextEntryHandle handle,
    XGameUiTextEntryChangeTypeFlags* changeType,
    uint32_t* cursorIndex,
    uint32_t* imeClauseStartIndex,
    uint32_t* imeClauseEndIndex,
    uint32_t bufferSize,
    char* buffer) {
    if (!changeType || !buffer || bufferSize == 0) {
        return XGR_E_INVALIDARG;
    }
    if (!IsTextEntryHandle(handle)) {
        return XGR_E_INVALIDARG;
    }
    // Nothing can type into the HLE keyboard, so the text never changes and it is
    // never dismissed.
    *changeType = XGameUiTextEntryChangeTypeFlags_None;
    if (cursorIndex) *cursorIndex = handle->cursor;
    if (imeClauseStartIndex) *imeClauseStartIndex = 0;
    if (imeClauseEndIndex) *imeClauseEndIndex = 0;
    const size_t copied = handle->text.size() < bufferSize - 1 ? handle->text.size() : bufferSize - 1;
    memcpy(buffer, handle->text.data(), copied);
    buffer[copied] = '\0';
    return XGR_S_OK;
}

XGameResult XGameUiTextEntryGetExtents(
    XGameUiTextEntryHandle handle,
    XGameUiTextEntryExtents* extents) {
    if (!extents || !IsTextEntryHandle(handle)) {
        return XGR_E_INVALIDARG;
    }
    // The HLE has no keyboard to measure, so the reported rectangle is the
    // conventional bottom or top third of the screen, in normalised coordinates.
    XGameUiTextEntryExtents out{};
    out.left = 0.0f;
    out.right = 1.0f;
    if (handle->position == XGameUiTextEntryPositionHint_Top) {
        out.top = 0.0f;
        out.bottom = 1.0f / 3.0f;
    } else {
        out.top = 2.0f / 3.0f;
        out.bottom = 1.0f;
    }
    *extents = out;
    return XGR_S_OK;
}

XGameResult XGameUiTextEntryUpdatePositionHint(
    XGameUiTextEntryHandle handle,
    XGameUiTextEntryPositionHint positionHint) {
    if (!IsTextEntryHandle(handle)) {
        return XGR_E_INVALIDARG;
    }
    if (positionHint != XGameUiTextEntryPositionHint_Bottom &&
        positionHint != XGameUiTextEntryPositionHint_Top) {
        return XGR_E_INVALIDARG;
    }
    handle->position = positionHint;
    return XGR_S_OK;
}

XGameResult XGameUiTextEntryUpdateVisibility(
    XGameUiTextEntryHandle handle,
    XGameUiTextEntryVisibilityFlags visibilityFlags) {
    if (!IsTextEntryHandle(handle)) {
        return XGR_E_INVALIDARG;
    }
    if (visibilityFlags != XGameUiTextEntryVisibilityFlags_Default &&
        visibilityFlags != XGameUiTextEntryVisibilityFlags_OnlyShowCandidates) {
        return XGR_E_INVALIDARG;
    }
    handle->visibility = visibilityFlags;
    return XGR_S_OK;
}
