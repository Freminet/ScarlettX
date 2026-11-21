#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "scarlettx/gdk/xgameruntime_abi.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "[test_xpackage] FAILED: " #cond                        \
                      << " at line " << __LINE__ << "\n";                        \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

namespace fs = std::filesystem;

// E_NOTIMPL, documented for XPackageUninstallUWPInstance on unsupported platforms.
constexpr XGameResult kENotImpl = static_cast<XGameResult>(0x80004001u);

struct EnumerationResult {
    int count = 0;
    std::string packageIdentifier;
    std::string displayName;
    std::string titleID;
    XPackageKind kind = XPackageKind_Content;
    XVersion version{};
    bool installing = true;
    uint32_t index = 99;
    uint32_t reportedCount = 0;
    bool ageRestricted = true;
};

bool SCARLETTX_GDK_CALLBACK CollectPackages(void* context, const XPackageDetails* details) {
    auto* out = static_cast<EnumerationResult*>(context);
    if (!details) {
        return false;
    }
    ++out->count;
    out->packageIdentifier = details->packageIdentifier ? details->packageIdentifier : "";
    out->displayName = details->displayName ? details->displayName : "";
    out->titleID = details->titleID ? details->titleID : "";
    out->kind = details->kind;
    out->version = details->version;
    out->installing = details->installing;
    out->index = details->index;
    out->reportedCount = details->count;
    out->ageRestricted = details->ageRestricted;
    return true;
}

bool SCARLETTX_GDK_CALLBACK StopAfterFirst(void* context, const XPackageDetails* details) {
    if (details) {
        ++*static_cast<int*>(context);
    }
    return false;
}

bool SCARLETTX_GDK_CALLBACK CollectFeatures(void* context, const XPackageFeature* /*feature*/) {
    ++*static_cast<int*>(context);
    return true;
}

}  // namespace

int main() {
    std::string identifier;
    {
        char buffer[256]{};
        CHECK(XPackageGetCurrentProcessPackageIdentifier(sizeof(buffer), buffer) == XGR_S_OK);
        identifier = buffer;
        CHECK(!identifier.empty());
    }

    // XPackage identity and locale: the process always runs as a packaged title
    // with the fixed identifier, and the buffers are sized as documented.
    {
        CHECK(XPackageIsPackagedProcess());
        CHECK(identifier == "ScarlettX.Title_1.0.0.0_x64__scarlettx");

        char small[4]{};
        CHECK(XPackageGetCurrentProcessPackageIdentifier(sizeof(small), small) ==
              XGR_E_NOT_SUFFICIENT_BUFFER);
        CHECK(XPackageGetCurrentProcessPackageIdentifier(0, nullptr) == XGR_E_NOT_SUFFICIENT_BUFFER);

        char locale[32]{};
        CHECK(XPackageGetUserLocale(sizeof(locale), locale) == XGR_S_OK);
        CHECK(std::strcmp(locale, "en-US") == 0);
        CHECK(XPackageGetUserLocale(3, locale) == XGR_E_NOT_SUFFICIENT_BUFFER);
    }

    // XPackageGetPackageKind: the running title is a game package.
    {
        XPackageKind kind = XPackageKind_Content;
        CHECK(XPackageGetPackageKind(identifier.c_str(), &kind) == XGR_S_OK);
        CHECK(kind == XPackageKind_Game);

        CHECK(XPackageGetPackageKind(nullptr, &kind) == XGR_E_INVALIDARG);
        CHECK(XPackageGetPackageKind(identifier.c_str(), nullptr) == XGR_E_INVALIDARG);

        kind = XPackageKind_Content;
        CHECK(XPackageGetPackageKind("Other.Title_1.0.0.0_x64__other", &kind) == XGR_E_NOTFOUND);
        CHECK(kind == XPackageKind_Content);
    }

    // XPackageEnumeratePackages: one package for every game scope.
    {
        for (XPackageEnumerationScope scope :
             {XPackageEnumerationScope_ThisOnly,
              XPackageEnumerationScope_ThisAndRelated,
              XPackageEnumerationScope_ThisPublisher}) {
            EnumerationResult found{};
            CHECK(XPackageEnumeratePackages(XPackageKind_Game, scope, &found, CollectPackages) == XGR_S_OK);
            CHECK(found.count == 1);
            CHECK(found.packageIdentifier == identifier);
            CHECK(found.kind == XPackageKind_Game);
            CHECK(!found.installing);
            CHECK(!found.ageRestricted);
            CHECK(found.index == 0);
            CHECK(found.reportedCount == 1);
            CHECK(found.version.major == 1);
            CHECK(!found.displayName.empty());
            CHECK(found.titleID.size() == 8);
        }

        // No DLC or publisher-hosted content exists.
        EnumerationResult content{};
        CHECK(XPackageEnumeratePackages(XPackageKind_Content, XPackageEnumerationScope_ThisOnly,
                                        &content, CollectPackages) == XGR_S_OK);
        CHECK(content.count == 0);
        CHECK(XPackageEnumeratePackages(XPackageKind_PublisherContent,
                                        XPackageEnumerationScope_ThisPublisher,
                                        &content, CollectPackages) == XGR_S_OK);
        CHECK(content.count == 0);

        // A callback that stops enumeration is honored.
        int stopped = 0;
        CHECK(XPackageEnumeratePackages(XPackageKind_Game, XPackageEnumerationScope_ThisOnly,
                                        &stopped, StopAfterFirst) == XGR_S_OK);
        CHECK(stopped == 1);

        CHECK(XPackageEnumeratePackages(XPackageKind_Game, XPackageEnumerationScope_ThisOnly,
                                        &content, nullptr) == XGR_E_INVALIDARG);
        CHECK(XPackageEnumeratePackages(static_cast<XPackageKind>(7),
                                        XPackageEnumerationScope_ThisOnly,
                                        &content, CollectPackages) == XGR_E_INVALIDARG);
        CHECK(XPackageEnumeratePackages(XPackageKind_Game,
                                        static_cast<XPackageEnumerationScope>(7),
                                        &content, CollectPackages) == XGR_E_INVALIDARG);
    }

    // XPackageEnumerateFeatures: the title exposes no extra features.
    {
        int features = 0;
        CHECK(XPackageEnumerateFeatures(identifier.c_str(), &features, CollectFeatures) == XGR_S_OK);
        CHECK(features == 0);
        CHECK(XPackageEnumerateFeatures(identifier.c_str(), &features, nullptr) == XGR_E_INVALIDARG);
        CHECK(XPackageEnumerateFeatures(nullptr, &features, CollectFeatures) == XGR_E_INVALIDARG);
        CHECK(XPackageEnumerateFeatures("Other.Title_1.0.0.0_x64__other", &features,
                                        CollectFeatures) == XGR_E_NOTFOUND);
    }

    // XPackageGetWriteStats: no write budget is applied.
    {
        CHECK(XPackageGetWriteStats(nullptr) == XGR_E_INVALIDARG);
        XPackageWriteStats stats{1, 2, 3, 4};
        CHECK(XPackageGetWriteStats(&stats) == XGR_S_OK);
        CHECK(stats.interval == 0);
        CHECK(stats.budget == 0);
        CHECK(stats.elapsed == 0);
        CHECK(stats.bytesWritten == 0);
    }

    // XPackageEstimateDownloadSize: everything is installed, nothing to download.
    {
        XPackageChunkSelector selector{};
        selector.type = XPackageChunkSelectorType_Language;
        selector.language = "en-US";

        uint64_t size = 1234;
        bool confirmation = true;
        CHECK(XPackageEstimateDownloadSize(identifier.c_str(), 1, &selector, &size,
                                           &confirmation) == XGR_S_OK);
        CHECK(size == 0);
        CHECK(!confirmation);

        // No selectors and no confirmation pointer are both valid.
        CHECK(XPackageEstimateDownloadSize(identifier.c_str(), 0, nullptr, &size, nullptr) == XGR_S_OK);
        CHECK(size == 0);

        CHECK(XPackageEstimateDownloadSize(nullptr, 1, &selector, &size, &confirmation) == XGR_E_INVALIDARG);
        CHECK(XPackageEstimateDownloadSize(identifier.c_str(), 1, &selector, nullptr,
                                           &confirmation) == XGR_E_INVALIDARG);
        CHECK(XPackageEstimateDownloadSize(identifier.c_str(), 2, nullptr, &size,
                                           &confirmation) == XGR_E_INVALIDARG);
        CHECK(XPackageEstimateDownloadSize("Other.Title_1.0.0.0_x64__other", 1, &selector, &size,
                                           &confirmation) == XGR_E_NOTFOUND);
    }

    // XPackageMount and the mount-path pair.
    {
        CHECK(XPackageMount(nullptr, nullptr) == XGR_E_INVALIDARG);
        XPackageMountHandle bad = reinterpret_cast<XPackageMountHandle>(0x1);
        CHECK(XPackageMount("Other.Title_1.0.0.0_x64__other", &bad) == XGR_E_NOTFOUND);
        CHECK(bad == nullptr);

        XPackageMountHandle mount = nullptr;
        CHECK(XPackageMount(identifier.c_str(), &mount) == XGR_S_OK);
        CHECK(mount != nullptr);

        size_t path_size = 0;
        CHECK(XPackageGetMountPathSize(mount, &path_size) == XGR_S_OK);
        CHECK(path_size > 1);
        CHECK(XPackageGetMountPathSize(nullptr, &path_size) == XGR_E_INVALIDARG);
        CHECK(XPackageGetMountPathSize(mount, nullptr) == XGR_E_INVALIDARG);

        CHECK(XPackageGetMountPath(mount, 2, nullptr) == XGR_E_NOT_SUFFICIENT_BUFFER);
        char tiny[2]{};
        CHECK(XPackageGetMountPath(mount, sizeof(tiny), tiny) == XGR_E_NOT_SUFFICIENT_BUFFER);

        std::string path(path_size, '\0');
        CHECK(XPackageGetMountPath(mount, path_size, path.data()) == XGR_S_OK);
        path.resize(std::strlen(path.c_str()));
        CHECK(!path.empty());
        CHECK(fs::exists(fs::path(path)));
        CHECK(fs::is_directory(fs::path(path)));

        XPackageCloseMountHandle(mount);
        XPackageCloseMountHandle(nullptr);
    }

    // XPackageMountWithUiAsync / XPackageMountWithUiResult: no UI, mounts now.
    {
        XTaskQueueHandle queue = nullptr;
        CHECK(XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool,
                               XTaskQueueDispatchMode_Manual,
                               &queue) == XGR_S_OK);

        XAsyncBlock async{};
        async.queue = queue;
        CHECK(XPackageMountWithUiAsync(identifier.c_str(), &async) == XGR_S_OK);
        XPackageMountHandle mount = nullptr;
        CHECK(XPackageMountWithUiResult(&async, &mount) == XGR_S_OK);
        CHECK(mount != nullptr);

        size_t path_size = 0;
        CHECK(XPackageGetMountPathSize(mount, &path_size) == XGR_S_OK);
        std::string path(path_size, '\0');
        CHECK(XPackageGetMountPath(mount, path_size, path.data()) == XGR_S_OK);
        CHECK(std::strlen(path.c_str()) > 0);
        XPackageCloseMountHandle(mount);

        CHECK(XPackageMountWithUiAsync(nullptr, &async) == XGR_E_INVALIDARG);
        CHECK(XPackageMountWithUiAsync(identifier.c_str(), nullptr) == XGR_E_INVALIDARG);
        CHECK(XPackageMountWithUiAsync("Other.Title_1.0.0.0_x64__other", &async) == XGR_E_NOTFOUND);
        CHECK(XPackageMountWithUiResult(nullptr, &mount) == XGR_E_INVALIDARG);
        CHECK(XPackageMountWithUiResult(&async, nullptr) == XGR_E_INVALIDARG);

        XTaskQueueTerminate(queue, true, nullptr, nullptr);
        XTaskQueueCloseHandle(queue);
    }

    // Uninstall entry points: the running title cannot be removed and UWP
    // instances are not a supported concept on this platform.
    {
        CHECK(!XPackageUninstallPackage(nullptr));
        CHECK(!XPackageUninstallPackage(identifier.c_str()));
        CHECK(!XPackageUninstallPackage("Other.Title_1.0.0.0_x64__other"));

        CHECK(XPackageUninstallUWPInstance(nullptr) == XGR_E_INVALIDARG);
        CHECK(XPackageUninstallUWPInstance("Other.Title") == kENotImpl);
    }

    std::cout << "[test_xpackage] all checks passed\n";
    return 0;
}
