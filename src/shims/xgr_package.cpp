#include "scarlettx/gdk/xgameruntime_abi.h"

#include <cstdlib>
#include <cstring>
#include <string>

// XPackage HLE: the guest always runs as a single packaged title. Every query
// therefore describes exactly one package (the calling process) with no
// features, no DLC and nothing to download; installation is always complete.
// Signatures and semantics follow the public XPackage.h reference pages
// (learn.microsoft.com, gdk-2610).

namespace {

// Keep in sync with XPackageGetCurrentProcessPackageIdentifier.
std::string CurrentPackageIdentifier() {
    const char* env = std::getenv("SCARLETTX_PACKAGE_IDENTIFIER");
    return (env && *env) ? env : "ScarlettX.Title_1.0.0.0_x64__scarlettx";
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
