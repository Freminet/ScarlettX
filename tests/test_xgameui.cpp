#include <cstring>
#include <iostream>

#include "scarlettx/gdk/xgameruntime_abi.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "[test_xgameui] FAILED: " #cond                         \
                      << " at line " << __LINE__ << "\n";                        \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int main() {
    CHECK(XGameRuntimeInitialize() == XGR_S_OK);

    XTaskQueueHandle tp_q = nullptr;
    CHECK(XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool,
                           XTaskQueueDispatchMode_Manual,
                           &tp_q) == XGR_S_OK);

    XAsyncBlock user_async{};
    user_async.queue = tp_q;
    CHECK(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_async) == XGR_S_OK);
    XUserHandle user = nullptr;
    CHECK(XUserAddResult(&user_async, &user) == XGR_S_OK && user != nullptr);

    // XLauncher + XGameUI HLE
    {
        CHECK(XLaunchUri(nullptr, "ms-settings:") == XGR_S_OK);
        CHECK(XLaunchUri(nullptr, nullptr) == XGR_E_INVALIDARG);
        CHECK(XLaunchUri(nullptr, "") == XGR_E_INVALIDARG);

        XAsyncBlock dlg{};
        dlg.queue = tp_q;
        CHECK(XGameUiShowMessageDialogAsync(&dlg, "Title", "Body", "Yes", "No", nullptr,
                                            XGameUiMessageDialogButton_Second,
                                            XGameUiMessageDialogButton_Second) == XGR_S_OK);
        XGameUiMessageDialogButton pressed = static_cast<XGameUiMessageDialogButton>(99);
        CHECK(XGameUiShowMessageDialogResult(&dlg, &pressed) == XGR_S_OK);
        CHECK(pressed == XGameUiMessageDialogButton_Second);

        // Invalid arguments
        XAsyncBlock dlg2{};
        dlg2.queue = tp_q;
        CHECK(XGameUiShowMessageDialogAsync(nullptr, "T", "C", nullptr, nullptr, nullptr,
                                            XGameUiMessageDialogButton_First,
                                            XGameUiMessageDialogButton_First) == XGR_E_INVALIDARG);
        CHECK(XGameUiShowMessageDialogAsync(&dlg2, nullptr, "C", nullptr, nullptr, nullptr,
                                            XGameUiMessageDialogButton_First,
                                            XGameUiMessageDialogButton_First) == XGR_E_INVALIDARG);
        CHECK(XGameUiShowMessageDialogAsync(&dlg2, "T", "C", nullptr, nullptr, nullptr,
                                            static_cast<XGameUiMessageDialogButton>(3),
                                            XGameUiMessageDialogButton_First) == XGR_E_INVALIDARG);
    }

    // XGameUI extended (achievements, profile card, invites, state share)
    {
        CHECK(XGameUiSetNotificationPositionHint(XGameUiNotificationPositionHint_TopRight) == XGR_S_OK);
        CHECK(XGameUiSetNotificationPositionHint(static_cast<XGameUiNotificationPositionHint>(6)) == XGR_E_INVALIDARG);

        XAsyncBlock b{}; b.queue = tp_q;
        CHECK(XGameUiShowAchievementsAsync(&b, user, 0) == XGR_S_OK);
        CHECK(XGameUiShowAchievementsResult(&b) == XGR_S_OK);

        b = {}; b.queue = tp_q;
        CHECK(XGameUiShowPlayerProfileCardAsync(&b, user, 12345) == XGR_S_OK);
        CHECK(XGameUiShowPlayerProfileCardResult(&b) == XGR_S_OK);

        b = {}; b.queue = tp_q;
        CHECK(XGameUiShowSendGameInviteAsync(&b, user, "cfg", "tmpl", "sess", nullptr, nullptr) == XGR_S_OK);
        CHECK(XGameUiShowSendGameInviteResult(&b) == XGR_S_OK);

        b = {}; b.queue = tp_q;
        CHECK(XGameUiShowMultiplayerActivityGameInviteAsync(&b, user) == XGR_S_OK);
        CHECK(XGameUiShowMultiplayerActivityGameInviteResult(&b) == XGR_S_OK);

        b = {}; b.queue = tp_q;
        CHECK(XGameUiShowStateShareAsync(&b, user, "token123") == XGR_S_OK);
        CHECK(XGameUiShowStateShareResult(&b) == XGR_S_OK);

        // Null user -> INVALIDARG
        XAsyncBlock b2{}; b2.queue = tp_q;
        CHECK(XGameUiShowAchievementsAsync(&b2, nullptr, 0) == XGR_E_INVALIDARG);
        CHECK(XGameUiShowPlayerProfileCardAsync(&b2, nullptr, 0) == XGR_E_INVALIDARG);
        CHECK(XGameUiShowSendGameInviteAsync(&b2, nullptr, "c", "t", "s", nullptr, nullptr) == XGR_E_INVALIDARG);
        CHECK(XGameUiShowMultiplayerActivityGameInviteAsync(&b2, nullptr) == XGR_E_INVALIDARG);
        CHECK(XGameUiShowStateShareAsync(&b2, nullptr, "tok") == XGR_E_INVALIDARG);

        // Null required strings -> INVALIDARG
        XAsyncBlock b3{}; b3.queue = tp_q;
        CHECK(XGameUiShowSendGameInviteAsync(&b3, user, nullptr, "t", "s", nullptr, nullptr) == XGR_E_INVALIDARG);
        CHECK(XGameUiShowSendGameInviteAsync(&b3, user, "c", nullptr, "s", nullptr, nullptr) == XGR_E_INVALIDARG);
        CHECK(XGameUiShowSendGameInviteAsync(&b3, user, "c", "t", nullptr, nullptr, nullptr) == XGR_E_INVALIDARG);
        CHECK(XGameUiShowStateShareAsync(&b3, user, nullptr) == XGR_E_INVALIDARG);
    }

    // XGameUI error dialog and player picker
    {
        // ErrorDialog with context
        XAsyncBlock ed1{}; ed1.queue = tp_q;
        CHECK(XGameUiShowErrorDialogAsync(&ed1, XGR_E_FAIL, "ctx") == XGR_S_OK);
        CHECK(XGameUiShowErrorDialogResult(&ed1) == XGR_S_OK);

        // ErrorDialog with null context
        XAsyncBlock ed2{}; ed2.queue = tp_q;
        CHECK(XGameUiShowErrorDialogAsync(&ed2, 0x80070000, nullptr) == XGR_S_OK);
        CHECK(XGameUiShowErrorDialogResult(&ed2) == XGR_S_OK);

        // ErrorDialog null async -> INVALIDARG
        CHECK(XGameUiShowErrorDialogAsync(nullptr, XGR_S_OK, "c") == XGR_E_INVALIDARG);
        CHECK(XGameUiShowErrorDialogResult(nullptr) == XGR_E_INVALIDARG);

        // PlayerPicker: selectFrom={1,2,3}, preSelected={2,3}, min=0, max=1
        uint64_t selFrom[] = {1, 2, 3};
        uint64_t preSel[] = {2, 3};
        XAsyncBlock pp1{}; pp1.queue = tp_q;
        CHECK(XGameUiShowPlayerPickerAsync(&pp1, user, "Pick", 3, selFrom, 2, preSel, 0, 1) == XGR_S_OK);
        uint32_t cnt = 0;
        CHECK(XGameUiShowPlayerPickerResultCount(&pp1, &cnt) == XGR_S_OK);
        CHECK(cnt == 1);
        uint64_t buf[4]{};
        uint32_t used = 0;
        CHECK(XGameUiShowPlayerPickerResult(&pp1, 4, buf, &used) == XGR_S_OK);
        CHECK(used == 1);
        CHECK(buf[0] == 2);

        // Second picker: preSelected={2,3}, max=2 -> ResultCount==2
        XAsyncBlock pp2{}; pp2.queue = tp_q;
        CHECK(XGameUiShowPlayerPickerAsync(&pp2, user, "Pick", 0, nullptr, 2, preSel, 0, 2) == XGR_S_OK);
        uint32_t cnt2 = 0;
        CHECK(XGameUiShowPlayerPickerResultCount(&pp2, &cnt2) == XGR_S_OK);
        CHECK(cnt2 == 2);

        // Result with capacity 1 -> NOT_SUFFICIENT_BUFFER
        uint64_t buf2[1]{};
        CHECK(XGameUiShowPlayerPickerResult(&pp2, 1, buf2, nullptr) == XGR_E_NOT_SUFFICIENT_BUFFER);

        // Result with capacity 2 -> S_OK
        uint64_t buf3[2]{};
        uint32_t used2 = 0;
        CHECK(XGameUiShowPlayerPickerResult(&pp2, 2, buf3, &used2) == XGR_S_OK);
        CHECK(used2 == 2);

        // PlayerPicker: null promptText -> INVALIDARG
        XAsyncBlock pp3{}; pp3.queue = tp_q;
        CHECK(XGameUiShowPlayerPickerAsync(&pp3, user, nullptr, 0, nullptr, 0, nullptr, 0, 1) == XGR_E_INVALIDARG);

        // TextEntry: defaultText "hello", scope Default, max 0 -> S_OK, size 6
        {
            XAsyncBlock te1{}; te1.queue = tp_q;
            CHECK(XGameUiShowTextEntryAsync(&te1, nullptr, nullptr, "hello", XGameUiTextEntryInputScope_Default, 0) == XGR_S_OK);
            uint32_t sz = 0;
            CHECK(XGameUiShowTextEntryResultSize(&te1, &sz) == XGR_S_OK);
            CHECK(sz == 6);
            // buffer too small
            char buf3[3]{};
            CHECK(XGameUiShowTextEntryResult(&te1, 3, buf3, nullptr) == XGR_E_NOT_SUFFICIENT_BUFFER);
            // correct size
            char buf16[16]{};
            uint32_t used = 0;
            CHECK(XGameUiShowTextEntryResult(&te1, 16, buf16, &used) == XGR_S_OK);
            CHECK(used == 6);
            CHECK(strcmp(buf16, "hello") == 0);
        }
        // TextEntry: defaultText "abcdef", max 3 -> "abc", size 4
        {
            XAsyncBlock te2{}; te2.queue = tp_q;
            CHECK(XGameUiShowTextEntryAsync(&te2, nullptr, nullptr, "abcdef", XGameUiTextEntryInputScope_Default, 3) == XGR_S_OK);
            uint32_t sz = 0;
            CHECK(XGameUiShowTextEntryResultSize(&te2, &sz) == XGR_S_OK);
            CHECK(sz == 4);
        }
        // TextEntry: defaultText null -> "", size 1
        {
            XAsyncBlock te3{}; te3.queue = tp_q;
            CHECK(XGameUiShowTextEntryAsync(&te3, nullptr, nullptr, nullptr, XGameUiTextEntryInputScope_Default, 0) == XGR_S_OK);
            uint32_t sz = 0;
            CHECK(XGameUiShowTextEntryResultSize(&te3, &sz) == XGR_S_OK);
            CHECK(sz == 1);
        }
        // TextEntry: scope Password, max 3, defaultText "secret" -> "secret" (limit stays 1024)
        {
            XAsyncBlock te4{}; te4.queue = tp_q;
            CHECK(XGameUiShowTextEntryAsync(&te4, nullptr, nullptr, "secret", XGameUiTextEntryInputScope_Password, 3) == XGR_S_OK);
            uint32_t sz = 0;
            CHECK(XGameUiShowTextEntryResultSize(&te4, &sz) == XGR_S_OK);
            CHECK(sz == 7);
        }
        // TextEntry: null async -> INVALIDARG
        CHECK(XGameUiShowTextEntryAsync(nullptr, nullptr, nullptr, "x", XGameUiTextEntryInputScope_Default, 0) == XGR_E_INVALIDARG);

        // In-game keyboard: open, query, move, hide, close. Nothing types into the
        // HLE keyboard, so the text and the change type never move.
        {
            XGameUiTextEntryOptions options{};
            options.inputScope = XGameUiTextEntryInputScope_Alphanumeric;
            options.positionHint = XGameUiTextEntryPositionHint_Bottom;
            options.visibilityflags = XGameUiTextEntryVisibilityFlags_Default;

            XGameUiTextEntryHandle keyboard = nullptr;
            CHECK(XGameUiTextEntryOpen(&options, 64, "player one", 10, &keyboard) == XGR_S_OK);
            CHECK(keyboard != nullptr);

            CHECK(XGameUiTextEntryOpen(nullptr, 64, "x", 0, &keyboard) == XGR_E_INVALIDARG);
            CHECK(XGameUiTextEntryOpen(&options, 33 * 1024, "x", 0, &keyboard) == XGR_E_INVALIDARG);
            CHECK(XGameUiTextEntryOpen(&options, 64, "x", 0, nullptr) == XGR_E_INVALIDARG);

            XGameUiTextEntryChangeTypeFlags change = XGameUiTextEntryChangeTypeFlags_TextChanged;
            uint32_t cursor = 99;
            uint32_t ime_start = 7;
            uint32_t ime_end = 7;
            char text[32] = {};
            CHECK(XGameUiTextEntryGetState(keyboard, &change, &cursor, &ime_start, &ime_end,
                                           sizeof(text), text) == XGR_S_OK);
            CHECK(change == XGameUiTextEntryChangeTypeFlags_None);
            CHECK(cursor == 10 && ime_start == 0 && ime_end == 0);
            CHECK(std::strcmp(text, "player one") == 0);

            // The buffer is truncated and always terminated.
            char tiny[5] = {};
            CHECK(XGameUiTextEntryGetState(keyboard, &change, nullptr, nullptr, nullptr,
                                           sizeof(tiny), tiny) == XGR_S_OK);
            CHECK(std::strcmp(tiny, "play") == 0);

            XGameUiTextEntryExtents extents{};
            CHECK(XGameUiTextEntryGetExtents(keyboard, &extents) == XGR_S_OK);
            CHECK(extents.left == 0.0f && extents.right == 1.0f);
            CHECK(extents.top > 0.5f && extents.bottom == 1.0f);

            CHECK(XGameUiTextEntryUpdatePositionHint(keyboard, XGameUiTextEntryPositionHint_Top) ==
                  XGR_S_OK);
            CHECK(XGameUiTextEntryGetExtents(keyboard, &extents) == XGR_S_OK);
            CHECK(extents.top == 0.0f && extents.bottom < 0.5f);
            CHECK(XGameUiTextEntryUpdatePositionHint(
                      keyboard, static_cast<XGameUiTextEntryPositionHint>(7)) == XGR_E_INVALIDARG);

            CHECK(XGameUiTextEntryUpdateVisibility(
                      keyboard, XGameUiTextEntryVisibilityFlags_OnlyShowCandidates) == XGR_S_OK);
            CHECK(XGameUiTextEntryUpdateVisibility(
                      keyboard, static_cast<XGameUiTextEntryVisibilityFlags>(9)) == XGR_E_INVALIDARG);

            // Argument checks
            CHECK(XGameUiTextEntryGetState(nullptr, &change, nullptr, nullptr, nullptr,
                                           sizeof(text), text) == XGR_E_INVALIDARG);
            CHECK(XGameUiTextEntryGetState(keyboard, nullptr, nullptr, nullptr, nullptr,
                                           sizeof(text), text) == XGR_E_INVALIDARG);
            CHECK(XGameUiTextEntryGetState(keyboard, &change, nullptr, nullptr, nullptr, 0,
                                           nullptr) == XGR_E_INVALIDARG);
            CHECK(XGameUiTextEntryGetExtents(nullptr, &extents) == XGR_E_INVALIDARG);
            CHECK(XGameUiTextEntryGetExtents(keyboard, nullptr) == XGR_E_INVALIDARG);
            CHECK(XGameUiTextEntryUpdatePositionHint(nullptr, XGameUiTextEntryPositionHint_Top) ==
                  XGR_E_INVALIDARG);
            CHECK(XGameUiTextEntryUpdateVisibility(nullptr,
                                                   XGameUiTextEntryVisibilityFlags_Default) ==
                  XGR_E_INVALIDARG);

            CHECK(XGameUiTextEntryClose(keyboard) == XGR_S_OK);
            CHECK(XGameUiTextEntryClose(nullptr) == XGR_S_OK);
            CHECK(XGameUiTextEntryGetExtents(keyboard, &extents) == XGR_E_INVALIDARG);
        }

        // PlayerPicker: min > max -> INVALIDARG
        CHECK(XGameUiShowPlayerPickerAsync(&pp3, user, "p", 0, nullptr, 0, nullptr, 3, 1) == XGR_E_INVALIDARG);
    }

    XUserCloseHandle(user);
    XTaskQueueTerminate(tp_q, true, nullptr, nullptr);
    XTaskQueueCloseHandle(tp_q);
    XGameRuntimeUninitialize();

    std::cout << "[test_xgameui] all checks passed\n";
    return 0;
}
