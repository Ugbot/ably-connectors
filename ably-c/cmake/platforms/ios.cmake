# iOS toolchain hints for ably-c.
#
# Usage (CMake 3.14+ has built-in iOS support):
#
#   cmake -B build-ios \
#     -DCMAKE_SYSTEM_NAME=iOS \
#     -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
#     -DABLY_BUILD_EXAMPLES=OFF \
#     -DABLY_BUILD_TESTS=OFF
#
# For a device + simulator fat library, build twice (arm64 / x86_64) then
# use `lipo -create` to merge the two `libably.a` archives.
#
# Notes:
#   - Requires Xcode (not just the Command Line Tools).
#   - mbedTLS and wslay have no platform-specific code that blocks iOS.
#   - pthreads are available on iOS; Threads::Threads resolves correctly.
#   - ABLY_SANITIZE is available in the Simulator but may not work on device.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(WARNING
        "ios.cmake included on a non-iOS build.  "
        "Pass -DCMAKE_SYSTEM_NAME=iOS to enable iOS cross-compilation."
    )
endif()

# Silence the Xcode generator warning about missing SDKROOT for static libs.
set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO CACHE BOOL "" FORCE)

# Static libraries do not need code signing on iOS.
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO" CACHE STRING "" FORCE)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY    ""    CACHE STRING "" FORCE)
