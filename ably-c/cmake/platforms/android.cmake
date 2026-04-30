# Android NDK toolchain hints for ably-c.
#
# Usage (with the NDK's own toolchain file):
#
#   cmake -B build-android \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a \
#     -DANDROID_PLATFORM=android-21 \
#     -DABLY_BUILD_EXAMPLES=OFF \
#     -DABLY_BUILD_TESTS=OFF
#
# Supported ABIs: arm64-v8a, armeabi-v7a, x86, x86_64
#
# Notes:
#   - mbedTLS supports Android natively; no special configuration is required.
#   - ABLY_SANITIZE is not supported on Android NDK builds.
#   - pthreads are provided by the NDK's libc; Threads::Threads resolves
#     correctly when CMAKE_SYSTEM_NAME is Android.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    message(WARNING
        "android.cmake included on a non-Android build.  "
        "Pass -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake "
        "to enable Android cross-compilation."
    )
endif()

# NDK r23+ defaults to clang; ensure C11 / C++17 flags are accepted.
set(CMAKE_C_STANDARD   11 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)

# Android does not have a separate pthreads library (it is in libc).
set(CMAKE_THREAD_LIBS_INIT "" CACHE STRING "" FORCE)
set(CMAKE_HAVE_THREADS_LIBRARY 1 CACHE BOOL "" FORCE)
set(CMAKE_USE_WIN32_THREADS_INIT 0 CACHE BOOL "" FORCE)
set(CMAKE_USE_PTHREADS_INIT 1 CACHE BOOL "" FORCE)
set(THREADS_PREFER_PTHREAD_FLAG OFF CACHE BOOL "" FORCE)
