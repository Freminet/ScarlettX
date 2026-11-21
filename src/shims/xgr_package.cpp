#include "scarlettx/gdk/xgameruntime_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>

#if defined(_WIN32)
#  include <windows.h>
#endif

#include "xgr_task_queue.h"

namespace fs = std::filesystem;

// XPackage HLE: the guest always runs as a single packaged title. Every query
// therefore describes exactly one package (the calling process) with no
// features, no DLC and nothing to download; installation is always complete.
// Signatures and semantics follow the public XPackage.h reference pages
// (learn.microsoft.com, gdk-2610).

namespace {

bool TraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

std::string CurrentPackageIdentifier() {
    const char* env = std::getenv("SCARLETTX_PACKAGE_IDENTIFIER");
    return (env && *env) ? env : "ScarlettX.Title_1.0.0.0_x64__scarlettx";
}

bool IsCurrentPackage(const char* packageIdentifier) {
    return packageIdentifier && CurrentPackageIdentifier() == packageIdentifier;
}

// The mounted folder is SCARLETTX_PACKAGE_ROOT when set, otherwise the folder
// that holds the executable (or the working directory outside Windows).
std::string PackageRootPath() {
    const char* env = std::getenv("SCARLETTX_PACKAGE_ROOT");
    if (env && *env) {
        return env;
    }
#if defined(_WIN32)
    char module_path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return fs::path(std::string(module_path, len)).parent_path().string();
    }
#endif
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    return ec ? std::string(".") : cwd.string();
}

XGameResult CopyStringToBuffer(const std::string& src,
                               size_t dst_size,
                               char* dst) {
    const size_t required = src.size() + 1;
    if (!dst || dst_size < required) {
        return XGR_E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(dst, src.c_str(), required);
    return XGR_S_OK;
}

bool IsValidKind(XPackageKind kind) {
    return static_cast<uint32_t>(kind) <= static_cast<uint32_t>(XPackageKind_PublisherContent);
}

bool IsValidScope(XPackageEnumerationScope scope) {
    return static_cast<uint32_t>(scope) <=
           static_cast<uint32_t>(XPackageEnumerationScope_ThisPublisher);
}

bool IsValidSelectorType(XPackageChunkSelectorType type) {
    return static_cast<uint32_t>(type) <=
           static_cast<uint32_t>(XPackageChunkSelectorType_Feature);
}

constexpr uint32_t kMountMagic = 0x58504D54;    // 'XPMT'
constexpr uint32_t kMonitorMagic = 0x5850494D;  // 'XPIM'

// Registration tokens handed to the guest. Nothing ever fires, so the token is
// only an identity: the matching unregister accepts it exactly once.
std::mutex g_registration_mutex;
uint64_t g_registration_counter = 0;
std::unordered_set<uint64_t> g_package_installed_tokens;

uint64_t NextRegistrationTokenLocked() {
    return ++g_registration_counter;
}

}  // namespace

// The class name XPackageMount is hidden by the XPackageMount function, so the
// definition is only reachable through an elaborated specifier.
struct XPackageMount {
    uint32_t magic = kMountMagic;
    std::string path;
};
using MountObject = struct XPackageMount;

// Installation monitor: a snapshot that is always complete and never changes.
struct XPackageInstallationMonitor {
    uint32_t magic = kMonitorMagic;
    std::mutex mutex;
    std::unordered_set<uint64_t> tokens;
};
using MonitorObject = struct XPackageInstallationMonitor;

namespace {

bool ValidMonitor(XPackageInstallationMonitorHandle monitor) {
    return monitor && monitor->magic == kMonitorMagic;
}

XPackageInstallationMonitorHandle NewInstallationMonitor() {
    return new (std::nothrow) MonitorObject();
}

}  // namespace

// ---------------------------------------------------------------------------
// Package identity and locale
// ---------------------------------------------------------------------------
bool XPackageIsPackagedProcess() {
    return true;
}

XGameResult XPackageGetCurrentProcessPackageIdentifier(size_t bufferSize, char* buffer) {
    return CopyStringToBuffer(CurrentPackageIdentifier(), bufferSize, buffer);
}

XGameResult XPackageGetUserLocale(size_t localeSize, char* locale) {
    const char* env = std::getenv("SCARLETTX_USER_LOCALE");
    const std::string value = (env && *env) ? env : "en-US";
    return CopyStringToBuffer(value, localeSize, locale);
}

// ---------------------------------------------------------------------------
// Package enumeration and queries
// ---------------------------------------------------------------------------
XGameResult XPackageEnumeratePackages(
    XPackageKind kind,
    XPackageEnumerationScope scope,
    void* context,
    XPackageEnumerationCallback* callback) {
    if (!callback || !IsValidKind(kind) || !IsValidScope(scope)) {
        return XGR_E_INVALIDARG;
    }
    // Only the title itself exists, and it is a game: content and
    // publisher-hosted package queries find nothing.
    if (kind != XPackageKind_Game) {
        return XGR_S_OK;
    }
    // Every scope includes at least the calling process, so the same single
    // package is reported for ThisOnly, ThisAndRelated and ThisPublisher.
    const std::string identifier = CurrentPackageIdentifier();
    XVersion version{};
    version.major = 1;

    XPackageDetails details{};
    details.packageIdentifier = identifier.c_str();
    details.version = version;
    details.kind = XPackageKind_Game;
    details.displayName = "ScarlettX Title";
    details.description = "";
    details.publisher = "ScarlettX";
    details.storeId = "";
    details.installing = false;
    details.index = 0;
    details.count = 1;
    details.ageRestricted = false;
    details.titleID = "5CA00001";  // synthetic, 8 uppercase hex digits as documented
    callback(context, &details);
    return XGR_S_OK;
}

XGameResult XPackageGetPackageKind(const char* packageIdentifier, XPackageKind* kind) {
    if (!packageIdentifier || !kind) {
        return XGR_E_INVALIDARG;
    }
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    *kind = XPackageKind_Game;
    return XGR_S_OK;
}

XGameResult XPackageEnumerateFeatures(
    const char* packageIdentifier,
    void* /*context*/,
    XPackageFeatureEnumerationCallback* callback) {
    if (!packageIdentifier || !callback) {
        return XGR_E_INVALIDARG;
    }
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    // The title installs as a single feature and exposes no additional ones.
    return XGR_S_OK;
}

XGameResult XPackageGetWriteStats(XPackageWriteStats* writeStats) {
    if (!writeStats) {
        return XGR_E_INVALIDARG;
    }
    // The HLE does not throttle guest writes, so nothing is budgeted or spent.
    *writeStats = XPackageWriteStats{};
    return XGR_S_OK;
}

XGameResult XPackageEstimateDownloadSize(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    uint64_t* downloadSize,
    bool* shouldPresentUserConfirmation) {
    if (!packageIdentifier || !downloadSize || (selectorCount > 0 && !selectors)) {
        return XGR_E_INVALIDARG;
    }
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    // Everything selectable is already installed: nothing to download and no
    // size large enough to need a confirmation prompt.
    *downloadSize = 0;
    if (shouldPresentUserConfirmation) {
        *shouldPresentUserConfirmation = false;
    }
    return XGR_S_OK;
}

XGameResult XPackageUninstallUWPInstance(const char* packageName) {
    if (!packageName) {
        return XGR_E_INVALIDARG;
    }
    // Documented answer for a platform that does not support the API.
    constexpr XGameResult kENotImpl = static_cast<XGameResult>(0x80004001u);
    return kENotImpl;
}

bool XPackageUninstallPackage(const char* packageIdentifier) {
    if (!packageIdentifier) {
        return false;
    }
    // A title cannot uninstall itself while it is running.
    return false;
}

// ---------------------------------------------------------------------------
// Mount
// ---------------------------------------------------------------------------
XGameResult XPackageMount(const char* packageIdentifier, XPackageMountHandle* mount) {
    if (!packageIdentifier || !mount) {
        return XGR_E_INVALIDARG;
    }
    *mount = nullptr;
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    auto* handle = new (std::nothrow) MountObject();
    if (!handle) {
        return XGR_E_OUTOFMEMORY;
    }
    handle->path = PackageRootPath();
    if (TraceEnabled()) {
        fprintf(stderr, "[ScarlettX][xgameruntime] XPackageMount(%s) -> \"%s\"\n",
                packageIdentifier, handle->path.c_str());
    }
    *mount = handle;
    return XGR_S_OK;
}

XGameResult XPackageGetMountPathSize(XPackageMountHandle mount, size_t* pathSize) {
    if (!mount || !pathSize || mount->magic != kMountMagic) {
        return XGR_E_INVALIDARG;
    }
    *pathSize = mount->path.size() + 1;
    return XGR_S_OK;
}

XGameResult XPackageGetMountPath(XPackageMountHandle mount, size_t pathSize, char* path) {
    if (!mount || mount->magic != kMountMagic) {
        return XGR_E_INVALIDARG;
    }
    return CopyStringToBuffer(mount->path, pathSize, path);
}

void XPackageCloseMountHandle(XPackageMountHandle mount) {
    if (!mount) {
        return;
    }
    if (mount->magic == kMountMagic) {
        mount->magic = 0;
        delete mount;
    }
}

XGameResult XPackageMountWithUiAsync(const char* packageIdentifier, XAsyncBlock* async) {
    if (!packageIdentifier || !async) {
        return XGR_E_INVALIDARG;
    }
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    // The mount needs no user confirmation, so no UI is ever shown.
    return xgr::BeginHleAsync(async, [](xgr::HleAsyncState* state) {
        auto* handle = new (std::nothrow) MountObject();
        if (!handle) {
            xgr::CompleteHleAsync(state, XGR_E_OUTOFMEMORY, 0);
            return;
        }
        handle->path = PackageRootPath();
        state->result_handle = handle;
        xgr::CompleteHleAsync(state, XGR_S_OK, sizeof(XPackageMountHandle));
    });
}

XGameResult XPackageMountWithUiResult(XAsyncBlock* async, XPackageMountHandle* mount) {
    if (!async || !mount) {
        return XGR_E_INVALIDARG;
    }
    const XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *mount = nullptr;
        return hr;
    }
    auto state = xgr::GetHleAsyncState(async);
    if (!state || !state->result_handle) {
        *mount = nullptr;
        return XGR_E_FAIL;
    }
    *mount = static_cast<XPackageMountHandle>(state->result_handle);
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

// ---------------------------------------------------------------------------
// Chunks and installation
// ---------------------------------------------------------------------------
// Every chunk of the title is installed: availability queries answer Ready,
// installs are already complete and no monitor ever reports a change.
XGameResult XPackageEnumerateChunkAvailability(
    const char* packageIdentifier,
    XPackageChunkSelectorType type,
    void* /*context*/,
    XPackageChunkAvailabilityCallback* callback) {
    if (!packageIdentifier || !callback) {
        return XGR_E_INVALIDARG;
    }
    if (!IsValidSelectorType(type)) {
        return XGR_E_INVALIDARG;
    }
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    // A single-chunk installation has no per-language, per-tag or per-feature
    // entries to report.
    return XGR_S_OK;
}

XGameResult XPackageFindChunkAvailability(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    XPackageChunkAvailability* availability) {
    if (!packageIdentifier || !availability || (selectorCount > 0 && !selectors)) {
        return XGR_E_INVALIDARG;
    }
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    *availability = XPackageChunkAvailability_Ready;
    return XGR_S_OK;
}

XGameResult XPackageChangeChunkInstallOrder(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors) {
    if (!packageIdentifier || (selectorCount > 0 && !selectors)) {
        return XGR_E_INVALIDARG;
    }
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    // There is no pending installation whose order could be changed.
    return XGR_S_OK;
}

XGameResult XPackageUninstallChunks(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors) {
    if (!packageIdentifier || (selectorCount > 0 && !selectors)) {
        return XGR_E_INVALIDARG;
    }
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    // Using only the integer chunk ID is documented as E_INVALIDARG.
    for (uint32_t i = 0; i < selectorCount; ++i) {
        if (selectors[i].type == XPackageChunkSelectorType_Chunk) {
            return XGR_E_INVALIDARG;
        }
    }
    return XGR_S_OK;
}

XGameResult XPackageCreateInstallationMonitor(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    uint32_t /*minimumUpdateIntervalMs*/,
    XTaskQueueHandle /*queue*/,
    XPackageInstallationMonitorHandle* installationMonitor) {
    if (!packageIdentifier || !installationMonitor ||
        (selectorCount > 0 && !selectors)) {
        return XGR_E_INVALIDARG;
    }
    *installationMonitor = nullptr;
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    auto* monitor = NewInstallationMonitor();
    if (!monitor) {
        return XGR_E_OUTOFMEMORY;
    }
    *installationMonitor = monitor;
    return XGR_S_OK;
}

XGameResult XPackageInstallChunks(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    uint32_t minimumUpdateIntervalMs,
    bool /*suppressUserConfirmation*/,
    XTaskQueueHandle queue,
    XPackageInstallationMonitorHandle* installationMonitor) {
    return XPackageCreateInstallationMonitor(packageIdentifier, selectorCount, selectors,
                                             minimumUpdateIntervalMs, queue,
                                             installationMonitor);
}

XGameResult XPackageInstallChunksAsync(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    uint32_t /*minimumUpdateIntervalMs*/,
    bool /*suppressUserConfirmation*/,
    XAsyncBlock* asyncBlock) {
    if (!packageIdentifier || !asyncBlock || (selectorCount > 0 && !selectors)) {
        return XGR_E_INVALIDARG;
    }
    if (!IsCurrentPackage(packageIdentifier)) {
        return XGR_E_NOTFOUND;
    }
    return xgr::BeginHleAsync(asyncBlock, [](xgr::HleAsyncState* state) {
        auto* monitor = NewInstallationMonitor();
        if (!monitor) {
            xgr::CompleteHleAsync(state, XGR_E_OUTOFMEMORY, 0);
            return;
        }
        state->result_handle = monitor;
        xgr::CompleteHleAsync(state, XGR_S_OK, sizeof(XPackageInstallationMonitorHandle));
    });
}

XGameResult XPackageInstallChunksResult(
    XAsyncBlock* asyncBlock,
    XPackageInstallationMonitorHandle* installationMonitor) {
    if (!asyncBlock || !installationMonitor) {
        return XGR_E_INVALIDARG;
    }
    const XGameResult hr = XAsyncGetStatus(asyncBlock, true);
    if (hr != XGR_S_OK) {
        *installationMonitor = nullptr;
        return hr;
    }
    auto state = xgr::GetHleAsyncState(asyncBlock);
    if (!state || !state->result_handle) {
        *installationMonitor = nullptr;
        return XGR_E_FAIL;
    }
    *installationMonitor = static_cast<XPackageInstallationMonitorHandle>(state->result_handle);
    xgr::ReleaseHleAsyncResult(asyncBlock);
    return XGR_S_OK;
}

void XPackageCloseInstallationMonitorHandle(
    XPackageInstallationMonitorHandle installationMonitor) {
    if (!installationMonitor) {
        return;
    }
    if (installationMonitor->magic == kMonitorMagic) {
        installationMonitor->magic = 0;
        delete installationMonitor;
    }
}

void XPackageGetInstallationProgress(
    XPackageInstallationMonitorHandle installationMonitor,
    XPackageInstallationProgress* progress) {
    if (!ValidMonitor(installationMonitor) || !progress) {
        return;
    }
    // Nothing is left to install, so the installation is complete and launchable.
    *progress = XPackageInstallationProgress{};
    progress->launchable = true;
    progress->completed = true;
}

bool XPackageUpdateInstallationMonitor(XPackageInstallationMonitorHandle installationMonitor) {
    if (!ValidMonitor(installationMonitor)) {
        return false;
    }
    // The snapshot never changes, so there is never an update to report.
    return false;
}

XGameResult XPackageRegisterInstallationProgressChanged(
    XPackageInstallationMonitorHandle installationMonitor,
    void* /*context*/,
    XPackageInstallationProgressCallback* callback,
    XTaskQueueRegistrationToken* token) {
    if (!ValidMonitor(installationMonitor) || !callback || !token) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(installationMonitor->mutex);
    token->token = 0;
    {
        std::lock_guard<std::mutex> registry_lock(g_registration_mutex);
        token->token = NextRegistrationTokenLocked();
    }
    installationMonitor->tokens.insert(token->token);
    return XGR_S_OK;
}

bool XPackageUnregisterInstallationProgressChanged(
    XPackageInstallationMonitorHandle installationMonitor,
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    if (!ValidMonitor(installationMonitor)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(installationMonitor->mutex);
    // No callback is ever pending, so unregistration never has to wait.
    return installationMonitor->tokens.erase(token.token) > 0;
}

XGameResult XPackageRegisterPackageInstalled(
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XPackageInstalledCallback* callback,
    XTaskQueueRegistrationToken* token) {
    if (!callback || !token) {
        return XGR_E_INVALIDARG;
    }
    // Nothing is ever installed while the title runs, so the callback is only
    // recorded in order to be accepted back by the unregister call.
    std::lock_guard<std::mutex> lock(g_registration_mutex);
    token->token = NextRegistrationTokenLocked();
    g_package_installed_tokens.insert(token->token);
    return XGR_S_OK;
}

bool XPackageUnregisterPackageInstalled(XTaskQueueRegistrationToken token, bool /*wait*/) {
    std::lock_guard<std::mutex> lock(g_registration_mutex);
    return g_package_installed_tokens.erase(token.token) > 0;
}
