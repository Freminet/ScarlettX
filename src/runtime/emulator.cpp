#include "scarlettx/runtime/emulator.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>

#include "../loader/guest_modules.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/runtime/virtual_drive.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

// Prints the imports the run actually touched: the auto-stub trampolines with
// their call counts, then the names the guest resolved with GetProcAddress.
// Together they describe the surface a title wants and we do not provide.
void DumpImportReport(const MappedPeImage& image) {
    auto stubs = image.Stubs().SnapshotRecords();
    std::sort(stubs.begin(), stubs.end(), [](const StubCallRecord& a, const StubCallRecord& b) {
        return a.hit_count > b.hit_count;
    });
    std::cout << "[imports] " << stubs.size() << " auto-stub imports, "
              << guest::SnapshotDynamicLookups().size() << " dynamic lookups" << std::endl;
    for (const auto& stub : stubs) {
        std::cout << "[imports] stub " << stub.dll_name << "!";
        if (stub.by_ordinal) {
            std::cout << "#" << stub.ordinal;
        } else {
            std::cout << stub.symbol_name;
        }
        std::cout << " calls=" << stub.hit_count;
        if (stub.hit_count > 0) {
            std::cout << " last=0x" << std::hex << stub.last_args[0] << std::dec;
        }
        std::cout << std::endl;
    }

    auto lookups = guest::SnapshotDynamicLookups();
    std::sort(lookups.begin(), lookups.end(),
              [](const guest::DynamicLookupRecord& a, const guest::DynamicLookupRecord& b) {
                  if (a.resolved != b.resolved) {
                      return !a.resolved;  // unresolved first: they are the gaps
                  }
                  return a.hit_count > b.hit_count;
              });
    for (const auto& lookup : lookups) {
        std::cout << "[imports] dynamic " << lookup.dll_name << "!" << lookup.symbol_name
                  << " calls=" << lookup.hit_count
                  << (lookup.resolved ? " resolved" : " MISSING") << std::endl;
    }
}

// Real titles run until the user interrupts them, so the report is also printed
// from the console control handler: a guest that never returns still gets one.
const MappedPeImage* g_import_report_image = nullptr;
std::mutex g_import_report_mutex;
bool g_import_report_printed = false;

void PrintImportReportOnce() {
    std::lock_guard<std::mutex> lock(g_import_report_mutex);
    if (g_import_report_printed || !g_import_report_image) {
        return;
    }
    g_import_report_printed = true;
    DumpImportReport(*g_import_report_image);
}

#if defined(_WIN32)
BOOL WINAPI ImportReportCtrlHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        PrintImportReportOnce();
    }
    return FALSE;  // let the default handler terminate the process
}
#endif

// Owns the handler registration for the lifetime of the mapped image.
class ImportReportScope {
public:
    explicit ImportReportScope(const MappedPeImage* image) {
        if (!image) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_import_report_mutex);
            g_import_report_image = image;
            g_import_report_printed = false;
        }
        active_ = true;
        guest::SetDynamicLookupLogging(true);
#if defined(_WIN32)
        SetConsoleCtrlHandler(ImportReportCtrlHandler, TRUE);
#endif
    }

    ~ImportReportScope() {
        if (!active_) {
            return;
        }
        PrintImportReportOnce();
#if defined(_WIN32)
        SetConsoleCtrlHandler(ImportReportCtrlHandler, FALSE);
#endif
        guest::SetDynamicLookupLogging(false);
        std::lock_guard<std::mutex> lock(g_import_report_mutex);
        g_import_report_image = nullptr;
    }

    ImportReportScope(const ImportReportScope&) = delete;
    ImportReportScope& operator=(const ImportReportScope&) = delete;

private:
    bool active_ = false;
};

std::string FindGameConfigInDir(const std::string& package_dir) {
    const std::filesystem::path dir(package_dir);
    for (const char* candidate : {"MicrosoftGame.Config", "MicrosoftGame.config"}) {
        const auto p = dir / candidate;
        if (std::filesystem::exists(p)) {
            return p.string();
        }
    }
    return {};
}

}  // namespace

Emulator::Emulator(EmulatorConfig config) : config_(std::move(config)) {}

Status Emulator::InspectPackage(const std::string& package_dir,
                                PackageInspectionReport& out_report) const {
    out_report = PackageInspectionReport{};
    const std::string cfg_path = FindGameConfigInDir(package_dir);
    if (cfg_path.empty()) {
        return Status::Error(StatusCode::NotFound,
                             "MicrosoftGame.Config not found in package directory: " + package_dir);
    }

    Status st = LoadGameConfigFile(cfg_path, out_report.config);
    if (!st.ok()) {
        return st;
    }

    const GameExecutableEntry* exe = out_report.config.PrimaryExecutable();
    if (!exe) {
        return Status::Error(StatusCode::ParseError, "No primary executable in MicrosoftGame.Config");
    }

    const std::filesystem::path exe_path = std::filesystem::path(package_dir) / exe->name;
    if (!std::filesystem::exists(exe_path)) {
        return Status::Error(StatusCode::NotFound, "Package executable missing: " + exe_path.string());
    }
    out_report.executable_path = exe_path.string();

    return InspectPeFile(out_report.executable_path, redirects_, out_report.main_pe);
}

Status Emulator::BootPackage(const std::string& package_dir) {
    PackageInspectionReport report;
    Status st = InspectPackage(package_dir, report);
    if (!st.ok()) {
        return st;
    }

    if (!report.config.IsNativeScarlett()) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Package is not a native XboxGen9/Scarlett Game Core target");
    }

#if defined(_WIN32)
    // Inject HLE shim directory at the front of PATH so guest imports of
    // d3d12_xs.dll, xg_xs.dll, AcpHal.dll, xgameruntime.dll, etc. resolve to ScarlettX shims.
    std::string shim_dir = config_.shim_directory;
    if (shim_dir.empty()) {
        char exe_buf[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exe_buf, MAX_PATH) > 0) {
            shim_dir = std::filesystem::path(exe_buf).parent_path().string();
        }
    }

    auto set_env = [](const char* key, const char* value) {
        SetEnvironmentVariableA(key, value);
        _putenv_s(key, value);
    };

    if (!shim_dir.empty()) {
        std::string new_path = shim_dir;
        const char* cur_path = std::getenv("PATH");
        if (cur_path && *cur_path) {
            new_path += ";";
            new_path += cur_path;
        }
        set_env("PATH", new_path.c_str());
    }

    set_env("SCARLETTX_HEADLESS", config_.headless ? "1" : "0");
    set_env("SCARLETTX_PREFER_WARP", config_.prefer_warp ? "1" : "0");
    set_env("SCARLETTX_TRACE", config_.trace ? "1" : "0");
    set_env("SCARLETTX_STATS", config_.stats ? "1" : "0");
    set_env("SCARLETTX_STATS", config_.stats ? "1" : "0");
    if (!report.config.title_id.empty()) {
        set_env("SCARLETTX_TITLE_ID", report.config.title_id.c_str());
    }
    const std::string abs_pkg_dir = std::filesystem::absolute(package_dir).string();
    set_env("SCARLETTX_PACKAGE_ROOT", abs_pkg_dir.c_str());

    // Real Xbox Series X|S titles (e.g. Unity Game Core nativePlugins.txt) reference G:/.
    // The mapping is skipped when G: is already assigned on the host.
    struct VirtualDriveGuard {
        ~VirtualDriveGuard() { vfs::UnmapPackageDrive(); }
    } g_drive_guard;
    const bool drive_mapped = vfs::MapPackageDrive('G', abs_pkg_dir);
    if (drive_mapped) {
        std::cout << "[ScarlettX][vfs] Mapped virtual G:\\ -> " << abs_pkg_dir << "\n";
    }

    if (config_.max_frames > 0) {
        set_env("SCARLETTX_MAX_FRAMES", std::to_string(config_.max_frames).c_str());
    }
    if (!config_.rt_dump_path.empty()) {
        set_env("SCARLETTX_RT_DUMP",
                std::filesystem::absolute(config_.rt_dump_path).string().c_str());
    }
    if (!config_.raster_dump_path.empty()) {
        set_env("SCARLETTX_RASTER_DUMP",
                std::filesystem::absolute(config_.raster_dump_path).string().c_str());
    }

    if (config_.in_process) {
        MappedPeImage image;
        HleSymbolRegistry hle_registry;
        PeMapOptions map_opts;
        map_opts.force_relocate = config_.force_relocate;
        map_opts.auto_stub_missing = true;
        map_opts.shim_directory = shim_dir;

        Status map_st = image.MapFile(report.executable_path, redirects_, hle_registry, map_opts);
        if (!map_st.ok()) {
            return map_st;
        }
        ImportReportScope import_report(config_.dump_imports ? &image : nullptr);
        std::cout << "[ScarlettX][loader] Mapped " << report.executable_path << " at 0x"
                  << std::hex << image.MappedBase() << std::dec
                  << " (relocated=" << (image.WasRelocated() ? "yes" : "no")
                  << ", relocs=" << image.RelocationsApplied()
                  << ", imports=" << image.Resolutions().size()
                  << ", stubs=" << image.Stubs().StubCount() << ")" << std::endl;
        if (image.Stubs().StubCount() > 0) {
            for (const auto& res : image.Resolutions()) {
                if (res.kind == ImportResolutionKind::StubThunk) {
                    std::cout << "  [stub #" << res.stub_id << "] " << res.dll_name << "!";
                    if (res.by_ordinal) {
                        std::cout << "#" << res.ordinal;
                    } else {
                        std::cout << res.symbol_name;
                    }
                    std::cout << " (target=" << res.target_dll << ")" << std::endl;
                }
            }
        }

        int exit_code = 0;
        Status call_st = Status::Ok();
        if (image.GetExportAddress(config_.in_process_entry) != nullptr) {
            call_st = image.CallExportedEntry(config_.in_process_entry, exit_code);
        } else {
            set_env("SCARLETTX_REAL_TITLE", "1");
            // A title's relative paths start at its package root, as on the console.
            SetCurrentDirectoryA(drive_mapped ? "G:\\" : abs_pkg_dir.c_str());
            std::cout << "[ScarlettX][loader] Entering native PE entry point at "
                      << image.EntryPointAddress() << std::endl;
            call_st = image.CallEntryPoint(exit_code);
        }
        if (!call_st.ok()) {
            return call_st;
        }
        if (exit_code != 0) {
            return Status::Error(StatusCode::DeviceError,
                                 "In-process guest exited with non-zero code: " +
                                     std::to_string(exit_code));
        }
        return Status::Ok();
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + report.executable_path + "\"";
    std::string work_dir = std::filesystem::path(report.executable_path).parent_path().string();

    // The guest inherits this: if it cannot load it must end with an exit
    // code instead of waiting on a system error dialog.
    const UINT previous_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Searched before System32 and inherited by the child; PATH is not.
    if (!shim_dir.empty()) {
        SetDllDirectoryA(shim_dir.c_str());
    }
    const BOOL created = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        work_dir.c_str(), &si, &pi);
    if (!shim_dir.empty()) {
        SetDllDirectoryA(nullptr);
    }
    SetErrorMode(previous_error_mode);
    if (!created) {
        return Status::Error(StatusCode::DeviceError,
                             "CreateProcessA failed for guest executable: " + report.executable_path);
    }

    const bool timed_out = WaitForSingleObject(pi.hProcess, 60000) == WAIT_TIMEOUT;
    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (timed_out) {
        return Status::Error(StatusCode::DeviceError, "Guest process did not exit within 60 seconds and was stopped");
    }

    if (exit_code != 0) {
        char hex_buf[32] = {};
        std::snprintf(hex_buf, sizeof(hex_buf), "0x%08lX", static_cast<unsigned long>(exit_code));
        return Status::Error(StatusCode::DeviceError,
                             "Guest process exited with non-zero code: " +
                                 std::to_string(exit_code) + " (" + hex_buf + ")");
    }
    return Status::Ok();
#else
    return Status::Error(StatusCode::NotImplemented, "Guest PE boot currently requires Windows");
#endif
}

}  // namespace scarlettx
