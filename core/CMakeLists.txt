cmake_minimum_required(VERSION 3.16)
project(omegacore C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OMEGA_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/../build/deps CACHE PATH "third-party sources (scripts/fetch-deps.sh)")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# mbedTLS (static, PIC) for HTTPS.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
add_subdirectory(${OMEGA_DEPS}/mbedtls-3.6.8 mbedtls EXCLUDE_FROM_ALL)
foreach(_mb mbedcrypto mbedx509 mbedtls)
    if(TARGET ${_mb})
        # Thread-safe shared DRBG/entropy/PSA crypto across parallel HTTPS threads (consistent ABI via PUBLIC).
        target_compile_definitions(${_mb} PUBLIC MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD)
    endif()
endforeach()
if(TARGET mbedcrypto)
    target_compile_options(mbedcrypto PRIVATE -O3)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)")
        # Runtime-detected ARMv8-A hardware SHA-256 acceleration (falls back to C if absent).
        target_compile_definitions(mbedcrypto PRIVATE MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT)
    endif()
endif()

# QuickJS-ng (static, PIC): runs the yt-dlp EJS solver on YouTube's player JS (fallback client).
set(OMEGA_QUICKJS ${OMEGA_DEPS}/quickjs-0.18.0)
add_library(omega_quickjs STATIC ${OMEGA_QUICKJS}/dtoa.c ${OMEGA_QUICKJS}/libregexp.c ${OMEGA_QUICKJS}/libunicode.c
            ${OMEGA_QUICKJS}/quickjs.c)
set_target_properties(omega_quickjs PROPERTIES C_STANDARD 11 C_VISIBILITY_PRESET hidden)
target_compile_definitions(omega_quickjs PRIVATE _GNU_SOURCE)
target_compile_options(omega_quickjs PRIVATE -O2 -w)
target_include_directories(omega_quickjs PUBLIC ${OMEGA_QUICKJS})
target_link_libraries(omega_quickjs PUBLIC m)

# zlib (static, PIC, inflate-only): gzip decompression for HTTP API responses.
set(OMEGA_ZLIB ${OMEGA_DEPS}/zlib-1.4)
add_library(omega_zlib STATIC
    ${OMEGA_ZLIB}/adler32.c
    ${OMEGA_ZLIB}/crc32.c
    ${OMEGA_ZLIB}/inffast.c
    ${OMEGA_ZLIB}/inflate.c
    ${OMEGA_ZLIB}/inftrees.c
    ${OMEGA_ZLIB}/zutil.c
)
set_target_properties(omega_zlib PROPERTIES C_STANDARD 11 C_VISIBILITY_PRESET hidden)
target_compile_options(omega_zlib PRIVATE -O3 -w)
target_include_directories(omega_zlib PUBLIC ${OMEGA_ZLIB})

# Opus 1.6.1 (floating-point) & FAAD2 2.11.4 (floating-point, DRC off): clean software decode for PCM DSP.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_FIXED_POINT OFF CACHE BOOL "" FORCE)
set(OPUS_ENABLE_FLOAT_API ON CACHE BOOL "" FORCE)
set(OPUS_FLOAT_APPROX OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)")
    # Tizen TV targets ARMv8-A with guaranteed NEON; inline NEON intrinsics directly without RTCD dispatch.
    set(OPUS_PRESUME_NEON ON CACHE BOOL "" FORCE)
endif()
# GCC's 32-bit <arm_neon.h> omits vcvtaq_s32_f32 even with -march=armv8-a; restrict that branch to AArch64.
set(OPUS_ARM_MATHOPS "${OMEGA_DEPS}/opus-1.6.1/celt/arm/mathops_arm.h")
if(EXISTS "${OPUS_ARM_MATHOPS}")
    file(READ "${OPUS_ARM_MATHOPS}" _opus_mathops)
    string(REPLACE "defined(__aarch64__) || (defined(__ARM_ARCH) && __ARM_ARCH >= 8)"
                   "defined(__aarch64__)" _opus_mathops_fixed "${_opus_mathops}")
    if(NOT "${_opus_mathops_fixed}" STREQUAL "${_opus_mathops}")
        file(WRITE "${OPUS_ARM_MATHOPS}" "${_opus_mathops_fixed}")
    endif()
endif()
add_subdirectory(${OMEGA_DEPS}/opus-1.6.1 opus EXCLUDE_FROM_ALL)
if(TARGET opus)
    set_target_properties(opus PROPERTIES C_VISIBILITY_PRESET hidden)
    target_compile_options(opus PRIVATE -O3 -w -fno-math-errno -ffp-contract=fast)
endif()

set(FAAD_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(FAAD_APPLY_DRC OFF CACHE BOOL "" FORCE)
add_subdirectory(${OMEGA_DEPS}/faad2-2.11.4 faad2 EXCLUDE_FROM_ALL)
if(TARGET faad)
    set_target_properties(faad PROPERTIES C_VISIBILITY_PRESET hidden)
    # Override FAAD2's legacy GCC -ffloat-store flag so FPU/NEON registers and hardware FMA are used directly.
    target_compile_options(faad PRIVATE -O3 -w -fno-float-store -fno-math-errno -ffp-contract=fast)
endif()

# The EJS solver bundles (embedded as C strings).
file(READ ${OMEGA_DEPS}/ejs-0.8.0/lib.min.js OMEGA_EJS_LIB)
file(READ ${OMEGA_DEPS}/ejs-0.8.0/core.min.js OMEGA_EJS_CORE)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ejs.cpp
     "extern const char omega_ejs_lib[];\nconst char omega_ejs_lib[] = R\"EJS(${OMEGA_EJS_LIB})EJS\";\n"
     "extern const char omega_ejs_core[];\nconst char omega_ejs_core[] = R\"EJS(${OMEGA_EJS_CORE})EJS\";\n")

# Mozilla CA bundle embedded as a C string.
file(READ ${OMEGA_DEPS}/cacert.pem OMEGA_CACERT)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/cacert.cpp
     "extern const char omega_cacert_pem[];\nconst char omega_cacert_pem[] = R\"PEM(${OMEGA_CACERT})PEM\";\n")

# Single version source (repo VERSION file) and the default remote config, both embedded.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION OMEGA_VERSION LIMIT_COUNT 1)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../remote-config.json OMEGA_REMOTE_CONFIG)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/remote_config_default.cpp
     "extern const char omega_remote_config_default[];\nconst char omega_remote_config_default[] = R\"CFG(${OMEGA_REMOTE_CONFIG})CFG\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION ${CMAKE_CURRENT_SOURCE_DIR}/../remote-config.json)
add_compile_definitions(OMEGA_VERSION="${OMEGA_VERSION}")

add_library(omegacore SHARED
    ${CMAKE_CURRENT_BINARY_DIR}/cacert.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/remote_config_default.cpp
    src/util/remote_config.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/ejs.cpp
    src/yt/innertube.cpp
    src/yt/js_solver.cpp
    src/yt/auth.cpp
    src/yt/sponsorblock.cpp
    src/yt/watch.cpp
    src/core.cpp
    src/util/log.cpp
    src/net/http.cpp
    src/media/demuxer.cpp
    src/media/mp4.cpp
    src/media/webm.cpp
    src/audio/dsp.cpp
    src/player/esplayer_api.cpp
    src/player/player.cpp
)
target_include_directories(omegacore PUBLIC include PRIVATE src ${OMEGA_DEPS})
# qrcodegen is included as <qrcodegen/qrcodegen.hpp>
target_compile_options(omegacore PRIVATE -Wall -Wextra -O3 $<$<CXX_COMPILER_ID:GNU>:-Wno-psabi>)
set_target_properties(omegacore PROPERTIES CXX_VISIBILITY_PRESET hidden)
# Safe hardware math flags for 64-bit float audio DSP (ARMv8 vfma.f64 & direct vsqrt.f64 without -ffast-math hazards).
set_source_files_properties(src/audio/dsp.cpp PROPERTIES COMPILE_OPTIONS "-fno-math-errno;-ffp-contract=fast")

find_package(Threads REQUIRED)
target_link_libraries(omegacore PRIVATE mbedtls mbedx509 mbedcrypto omega_quickjs omega_zlib opus faad Threads::Threads m ${CMAKE_DL_LIBS})

# The TV ships its own libstdc++; link ours statically so we never depend on its version.
target_link_libraries(omegacore PRIVATE -static-libstdc++ -static-libgcc -Wl,--no-undefined)

# Host-side tool to check the demuxer against local files (not shipped).
if(OMEGA_HOST_TOOLS)
    add_executable(mp4dump tools/mp4dump.cpp src/media/demuxer.cpp src/media/mp4.cpp src/media/webm.cpp)
    target_include_directories(mp4dump PRIVATE src)
    # Fetches a YouTube video's formats and downloads ranges with the same code the TV runs.
    add_executable(ytcheck tools/ytcheck.cpp src/media/demuxer.cpp src/media/mp4.cpp src/media/webm.cpp src/yt/innertube.cpp src/yt/js_solver.cpp ${CMAKE_CURRENT_BINARY_DIR}/ejs.cpp src/yt/auth.cpp src/yt/sponsorblock.cpp src/net/http.cpp src/util/log.cpp
                   src/util/remote_config.cpp ${CMAKE_CURRENT_BINARY_DIR}/cacert.cpp
                   ${CMAKE_CURRENT_BINARY_DIR}/remote_config_default.cpp)
    target_include_directories(ytcheck PRIVATE src ${OMEGA_DEPS})
    target_link_libraries(ytcheck PRIVATE mbedtls mbedx509 mbedcrypto omega_quickjs omega_zlib Threads::Threads)
endif()

# Link-Time Optimization (Release builds only, verified against the active cross/host toolchain).
include(CheckIPOSupported)
check_ipo_supported(RESULT OMEGA_IPO_SUPPORTED LANGUAGES C CXX)
if(OMEGA_IPO_SUPPORTED)
    foreach(_tgt omegacore opus faad omega_zlib ytcheck)
        if(TARGET ${_tgt})
            set_property(TARGET ${_tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
    endforeach()
endif()
