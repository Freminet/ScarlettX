// Real-title proxy path: the output window of ID3D12CommandQueue::PresentX.
// The window must not be able to stop the thread that presents: while the
// window sits in a modal loop (here its system menu), PresentX still returns.
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "sx_check.h"

#include <windows.h>

#include <d3d12.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

constexpr int kRelease = 2;
constexpr int kDevCreateCommandQueue = 8;
constexpr int kDevCreateCommandAllocator = 9;
constexpr int kDevCreateCommandList = 12;
constexpr int kDevCreateDescriptorHeap = 14;
constexpr int kDevCreateRenderTargetView = 20;
constexpr int kDevCreateCommittedResource = 27;
constexpr int kHeapGetCpuStart = 9;
constexpr int kListClose = 9;
constexpr int kListClearRenderTargetView = 52;
constexpr int kQueueExecuteCommandLists = 10;
constexpr int kDevSetFrameIntervalX = 78;
constexpr int kQueuePresentX = 30;

HWND FindPresentWindow() {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExA(nullptr, hwnd, "ScarlettXPresentWindowClass", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return hwnd;
    }
    return nullptr;
}

bool InMenuMode(DWORD thread_id) {
    GUITHREADINFO info = {};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(thread_id, &info) && (info.flags & GUI_INMENUMODE) != 0;
}

bool WaitForMenuMode(DWORD thread_id, bool wanted, DWORD timeout_ms) {
    for (DWORD waited = 0; waited <= timeout_ms; waited += 20) {
        if (InMenuMode(thread_id) == wanted) return true;
        Sleep(20);
    }
    return false;
}

// Ends the test if the presenting thread gets stuck, where CTest would wait
// out its own much longer timeout.
HANDLE g_presents_done = nullptr;
DWORD WINAPI Watchdog(void*) {
    if (WaitForSingleObject(g_presents_done, 120000) == WAIT_TIMEOUT) {
        std::fprintf(stderr, "test_d3d12_present_window: PresentX did not return while the window was busy\n");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const bool headless = argc > 1 && std::strcmp(argv[1], "--headless") == 0;
    _putenv_s("SCARLETTX_HEADLESS", headless ? "1" : "0");
    // Select the guest-facing proxies (also set by CTest for this test).
    _putenv_s("SCARLETTX_REAL_TITLE", "1");

    void* device = nullptr;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    const IID device_iid = __uuidof(ID3D12Device);
    SX_REQUIRE(D3D12XboxCreateDevice(nullptr, &params, &device_iid, &device) >= 0);
    SX_REQUIRE(device != nullptr);

    void* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(device, kDevCreateCommandQueue, &qdesc, static_cast<void*>(nullptr), &queue) == 0);
    SX_REQUIRE(queue != nullptr);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 256;
    td.Height = 144;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    void* frame = nullptr;
    SX_REQUIRE(Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &td,
                    static_cast<uintptr_t>(D3D12_RESOURCE_STATE_COMMON), static_cast<void*>(nullptr),
                    static_cast<void*>(nullptr), &frame) == 0);
    SX_REQUIRE(frame != nullptr);

    // Plane parameters as the title passes them: the second quadword is a
    // count, the third the resource to show.
    uint64_t plane[8] = {};
    plane[1] = 1;
    plane[2] = reinterpret_cast<uint64_t>(frame);

    if (headless) {
        SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
        SX_REQUIRE(FindPresentWindow() == nullptr);
        const std::string prefix = "headless-present-" + std::to_string(GetCurrentProcessId());
        _putenv_s("SCARLETTX_DUMP_FRAME", prefix.c_str());
        _putenv_s("SCARLETTX_DUMP_FRAME_AT", "2,3,4,5");
        SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
        SX_REQUIRE(FindPresentWindow() == nullptr);
        const auto path = prefix + "_2.bmp";
        std::ifstream bmp(path, std::ios::binary);
        char magic[2]{};
        SX_REQUIRE(bmp.read(magic, sizeof(magic)) && magic[0] == 'B' && magic[1] == 'M');
        bmp.close();
        SX_REQUIRE(std::filesystem::file_size(path) == 54 + td.Width * td.Height * 3);
        SX_REQUIRE(std::filesystem::remove(path));

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        void* heap = nullptr;
        SX_REQUIRE(Call(device, kDevCreateDescriptorHeap, &heap_desc, nullptr, &heap) == 0 && heap);
        const auto rtv = static_cast<uintptr_t>(Call(heap, kHeapGetCpuStart));
        SX_REQUIRE(rtv != 0);
        int frame_number = 2;
        for (const auto format : {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  DXGI_FORMAT_R8G8B8A8_UNORM}) {
            td.Format = format;
            td.SampleDesc.Count = format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : 1;
            void* color_frame = nullptr;
            SX_REQUIRE(Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &td,
                            uintptr_t{D3D12_RESOURCE_STATE_RENDER_TARGET}, nullptr, nullptr, &color_frame) == 0);
            Call(device, kDevCreateRenderTargetView, color_frame, nullptr, rtv);
            void* allocator = nullptr;
            void* list = nullptr;
            SX_REQUIRE(Call(device, kDevCreateCommandAllocator, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                            nullptr, &allocator) == 0);
            SX_REQUIRE(Call(device, kDevCreateCommandList, uintptr_t{0}, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                            allocator, nullptr, nullptr, &list) == 0);
            const float clear_color[] = {1.0f, 0.6f, 0.2f, 1.0f};
            Call(list, kListClearRenderTargetView, rtv, static_cast<const float*>(clear_color), uint32_t{0}, nullptr);
            SX_REQUIRE(Call(list, kListClose) == 0);
            Call(queue, kQueueExecuteCommandLists, uintptr_t{1}, &list);
            plane[2] = reinterpret_cast<uint64_t>(color_frame);
            SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, nullptr) == 0);
            const auto color_path = prefix + "_" + std::to_string(++frame_number) + ".bmp";
            std::ifstream color_bmp(color_path, std::ios::binary);
            color_bmp.seekg(54);
            unsigned char bgr[3]{};
            SX_REQUIRE(color_bmp.read(reinterpret_cast<char*>(bgr), sizeof(bgr)));
            SX_REQUIRE(bgr[0] == 51 && bgr[1] == 153 && bgr[2] == 255);
            color_bmp.close();
            SX_REQUIRE(std::filesystem::remove(color_path));
            SX_REQUIRE(Call(list, kRelease) == 0);
            SX_REQUIRE(Call(allocator, kRelease) == 0);
            SX_REQUIRE(Call(color_frame, kRelease) == 0);
        }
        SX_REQUIRE(FindPresentWindow() == nullptr);
        SX_REQUIRE(Call(heap, kRelease) == 0);
        SX_REQUIRE(Call(frame, kRelease) == 0);
        SX_REQUIRE(Call(queue, kRelease) == 0);
        SX_REQUIRE(Call(device, kRelease) == 0);
        return 0;
    }

    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);

    // Changing the device interval affects an already-created queue.
    constexpr auto interval = std::chrono::milliseconds(50);
    SX_REQUIRE(Call(device, kDevSetFrameIntervalX, uintptr_t{0}, uintptr_t{50000},
                    uintptr_t{2}, uintptr_t{0}) == 0);
    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    }
    SX_REQUIRE(std::chrono::steady_clock::now() - start >= 3 * interval - std::chrono::milliseconds(10));
    SX_REQUIRE(Call(device, kDevSetFrameIntervalX, uintptr_t{0}, uintptr_t{0},
                    uintptr_t{2}, uintptr_t{0}) == 0);

    const HWND hwnd = FindPresentWindow();
    SX_REQUIRE(hwnd != nullptr);
    const DWORD window_thread = GetWindowThreadProcessId(hwnd, nullptr);
    SX_REQUIRE(window_thread != 0);
    SX_REQUIRE(window_thread != GetCurrentThreadId());

    // Alt or F10 alone does not put the window into menu mode.
    DWORD_PTR result = 0;
    SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0, SMTO_ABORTIFHUNG, 10000, &result) != 0);
    SX_REQUIRE(!InMenuMode(window_thread));

    // Closing the window is not a way to take the output away from the title.
    SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 10000, &result) != 0);
    SX_REQUIRE(IsWindow(hwnd));

    RECT windowed{};
    SX_REQUIRE(GetWindowRect(hwnd, &windowed));
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    SX_REQUIRE(GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor));
    const auto alt_enter = [&](LPARAM flags) {
        SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_SYSKEYDOWN, VK_RETURN, flags,
                                      SMTO_ABORTIFHUNG, 10000, &result) != 0);
    };
    constexpr LPARAM kAltDown = 1L << 29;
    constexpr LPARAM kKeyRepeat = 1L << 30;
    alt_enter(kAltDown);
    alt_enter(kAltDown | kKeyRepeat);
    RECT fullscreen{};
    SX_REQUIRE(GetWindowRect(hwnd, &fullscreen));
    SX_REQUIRE(EqualRect(&fullscreen, &monitor.rcMonitor));
    SX_REQUIRE((GetWindowLongPtrA(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == 0);
    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    alt_enter(kAltDown);
    RECT restored{};
    SX_REQUIRE(GetWindowRect(hwnd, &restored));
    SX_REQUIRE(EqualRect(&restored, &windowed));
    SX_REQUIRE((GetWindowLongPtrA(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW);
    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);

    // Open the system menu: a modal loop inside DefWindowProc.
    PostMessageA(hwnd, WM_SYSCOMMAND, SC_KEYMENU, ' ');  // Alt+Space
    const bool modal = WaitForMenuMode(window_thread, true, 5000);
    if (!modal) {
        std::printf("test_d3d12_present_window: the system menu did not open on this desktop; "
                    "presenting during a modal loop was NOT checked\n");
    }

    g_presents_done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    SX_REQUIRE(g_presents_done != nullptr);
    const HANDLE watchdog = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
    SX_REQUIRE(watchdog != nullptr);
    for (int i = 0; i < 3; ++i) {
        SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    }
    SetEvent(g_presents_done);
    WaitForSingleObject(watchdog, INFINITE);
    CloseHandle(watchdog);

    if (modal) {
        // The menu was open for all three presents.
        SX_REQUIRE(InMenuMode(window_thread));
        SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_CANCELMODE, 0, 0, SMTO_ABORTIFHUNG, 10000, &result) != 0);
        SX_REQUIRE(WaitForMenuMode(window_thread, false, 5000));
    }

    // Games can switch the output size while keeping the same host window.
    td.Width = 320;
    td.Height = 180;
    void* resized_frame = nullptr;
    SX_REQUIRE(Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &td,
                    static_cast<uintptr_t>(D3D12_RESOURCE_STATE_COMMON), static_cast<void*>(nullptr),
                    static_cast<void*>(nullptr), &resized_frame) == 0 && resized_frame);
    plane[2] = reinterpret_cast<uint64_t>(resized_frame);
    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    SX_REQUIRE(FindPresentWindow() == hwnd);
    plane[2] = reinterpret_cast<uint64_t>(frame);
    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    SX_REQUIRE(Call(resized_frame, kRelease) == 0);

    td.SampleDesc.Count = 4;
    void* msaa_frame = nullptr;
    SX_REQUIRE(Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &td,
                    static_cast<uintptr_t>(D3D12_RESOURCE_STATE_COMMON), static_cast<void*>(nullptr),
                    static_cast<void*>(nullptr), &msaa_frame) == 0 && msaa_frame);
    plane[2] = reinterpret_cast<uint64_t>(msaa_frame);
    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    plane[2] = reinterpret_cast<uint64_t>(frame);
    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    SX_REQUIRE(Call(msaa_frame, kRelease) == 0);

    td.SampleDesc.Count = 1;
    td.MipLevels = 3;
    td.DepthOrArraySize = 2;
    void* array_frame = nullptr;
    SX_REQUIRE(Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &td,
                    static_cast<uintptr_t>(D3D12_RESOURCE_STATE_COMMON), static_cast<void*>(nullptr),
                    static_cast<void*>(nullptr), &array_frame) == 0 && array_frame);
    plane[2] = reinterpret_cast<uint64_t>(array_frame);
    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    SX_REQUIRE(Call(array_frame, kRelease) == 0);

    // A guest may release its last frame immediately before releasing the queue.
    SX_REQUIRE(Call(frame, kRelease) == 0);
    SX_REQUIRE(Call(queue, kRelease) == 0);
    SX_REQUIRE(!IsWindow(hwnd));
    SX_REQUIRE(Call(device, kRelease) == 0);

    std::printf("test_d3d12_present_window: OK%s\n", modal ? "" : " (modal part skipped)");
    return 0;
}
