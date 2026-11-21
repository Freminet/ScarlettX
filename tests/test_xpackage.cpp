#include <cstring>
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

    std::cout << "[test_xpackage] all checks passed\n";
    return 0;
}
