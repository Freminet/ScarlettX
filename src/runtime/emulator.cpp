#include "scarlettx/runtime/emulator.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/runtime/virtual_drive.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

#if defined(_WIN32)
std::mutex g_boot_mutex;

class BootEnvironment {
public:
    BootEnvironment() {
        char buffer[MAX_PATH] = {};
        const DWORD length = GetCurrentDirectoryA(MAX_PATH, buffer);
        if (length > 0 && length < MAX_PATH) {
            previous_working_directory_ = buffer;
        }
    }

    void Set(const char* name, const char* value) {
        Entry entry;
        entry.name = name;
        entry.wide_name.assign(entry.name.begin(), entry.name.end());
        if (const char* previous = std::getenv(name)) entry.crt_value = previous;
        const DWORD capacity = GetEnvironmentVariableW(entry.wide_name.c_str(), nullptr, 0);
        if (capacity != 0) {
            std::wstring previous(capacity, L'\0');
            const DWORD length = GetEnvironmentVariableW(entry.wide_name.c_str(), previous.data(), capacity);
            if (length < capacity) {
                previous.resize(length);
                entry.windows_value = std::move(previous);
            }
        }
        entries_.push_back(std::move(entry));
        SetEnvironmentVariableA(name, value);
        _putenv_s(name, value);
    }

    ~BootEnvironment() {
        for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
            _putenv_s(entry->name.c_str(), entry->crt_value.c_str());
            SetEnvironmentVariableW(entry->wide_name.c_str(),
                                    entry->windows_value ? entry->windows_value->c_str() : nullptr);
        }
        if (previous_working_directory_) {
            SetCurrentDirectoryA(previous_working_directory_->c_str());
        }
    }

private:
    struct Entry {
        std::string name;
        std::wstring wide_name;
        std::string crt_value;
        std::optional<std::wstring> windows_value;
    };
    std::vector<Entry> entries_;
    std::optional<std::string> previous_working_directory_;
};
#endif

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
    std::error_code ec;
    const std::string abs_pkg_dir = std::filesystem::absolute(package_dir, ec).string();
    if (ec) {
        return Status::Error(StatusCode::IoError, "Cannot resolve package path: " + ec.message());
    }
    PackageInspectionReport report;
    Status st = InspectPackage(abs_pkg_dir, report);
    if (!st.ok()) {
        return st;
    }

    if (!report.config.IsNativeScarlett()) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Package is not a native XboxGen9/Scarlett Game Core target");
    }

#if defined(_WIN32)
    const auto resolve_optional_path = [](const std::string& path, std::string& resolved) {
        if (path.empty()) return Status::Ok();
        std::error_code error;
        resolved = std::filesystem::absolute(path, error).string();
        if (error) return Status::Error(StatusCode::IoError, "Cannot resolve path " + path + ": " + error.message());
        return Status::Ok();
    };
    std::string shim_dir;
    std::string rt_dump_path;
    std::string raster_dump_path;
    st = resolve_optional_path(config_.shim_directory, shim_dir);
    if (!st.ok()) return st;
    st = resolve_optional_path(config_.rt_dump_path, rt_dump_path);
    if (!st.ok()) return st;
    st = resolve_optional_path(config_.raster_dump_path, raster_dump_path);
    if (!st.ok()) return st;

    // Inject HLE shim directory at the front of PATH so guest imports of
    // d3d12_xs.dll, xg_xs.dll, AcpHal.dll, xgameruntime.dll, etc. resolve to ScarlettX shims.
    if (shim_dir.empty()) {
        char exe_buf[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exe_buf, MAX_PATH) > 0) {
            shim_dir = std::filesystem::path(exe_buf).parent_path().string();
        }
    }

    // The guest's settings and package drive are process-wide.
    const std::lock_guard<std::mutex> lock(g_boot_mutex);
    BootEnvironment environment;

    if (!shim_dir.empty()) {
        std::string new_path = shim_dir;
        const char* cur_path = std::getenv("PATH");
        if (cur_path && *cur_path) {
            new_path += ";";
            new_path += cur_path;
        }
        environment.Set("PATH", new_path.c_str());
    }

    environment.Set("SCARLETTX_HEADLESS", config_.headless ? "1" : "0");
    environment.Set("SCARLETTX_PREFER_WARP", config_.prefer_warp ? "1" : "0");
    environment.Set("SCARLETTX_TRACE", config_.trace ? "1" : "0");
    environment.Set("SCARLETTX_STATS", config_.stats ? "1" : "0");
    environment.Set("SCARLETTX_FCAP", std::to_string(config_.fps_cap).c_str());
    if (!report.config.title_id.empty()) {
        environment.Set("SCARLETTX_TITLE_ID", report.config.title_id.c_str());
    }
    environment.Set("SCARLETTX_PACKAGE_ROOT", abs_pkg_dir.c_str());

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
        environment.Set("SCARLETTX_MAX_FRAMES", std::to_string(config_.max_frames).c_str());
    }
    if (!rt_dump_path.empty()) {
        environment.Set("SCARLETTX_RT_DUMP", rt_dump_path.c_str());
    }
    if (!raster_dump_path.empty()) {
        environment.Set("SCARLETTX_RASTER_DUMP", raster_dump_path.c_str());
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
            environment.Set("SCARLETTX_REAL_TITLE", "1");
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

    CloseHandle(pi.hThread);
    // Keep the package drive mapped for the entire guest session.
    if (WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0) {
        const DWORD error = GetLastError();
        CloseHandle(pi.hProcess);
        return Status::Error(StatusCode::DeviceError,
                             "Waiting for the guest process failed: " + std::to_string(error));
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        const DWORD error = GetLastError();
        CloseHandle(pi.hProcess);
        return Status::Error(StatusCode::DeviceError,
                             "Reading the guest exit code failed: " + std::to_string(error));
    }
    CloseHandle(pi.hProcess);

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
