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

// Keep in sync with XPackageGetCurrentProcessPackageIdentifier.
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
