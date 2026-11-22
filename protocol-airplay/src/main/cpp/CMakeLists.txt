cmake_minimum_required(VERSION 3.22.1)
project(screenatic_airplay C CXX)

set(UXPLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/UxPlay)
set(PREBUILT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/prebuilt/${ANDROID_ABI})

add_library(screenatic_airplay SHARED
    jni_bridge.cpp
    video_decoder.cpp
    airplay_server.cpp
    audio_player.cpp
)

# Apple ALAC decoder (Apache-2.0) for audio-only AirPlay. Decoder sources only.
set(ALAC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/alac/codec)
if(EXISTS ${ALAC_DIR}/ALACDecoder.cpp)
    add_library(alac STATIC
        ${ALAC_DIR}/ALACDecoder.cpp ${ALAC_DIR}/ALACBitUtilities.c ${ALAC_DIR}/ag_dec.c
        ${ALAC_DIR}/dp_dec.c ${ALAC_DIR}/matrix_dec.c ${ALAC_DIR}/EndianPortable.c)
    target_include_directories(alac PUBLIC ${ALAC_DIR})
    target_compile_options(alac PRIVATE -w)  # 2011-era code; keep our build log clean
    # EndianPortable.c only detects x86 as little-endian; on ARM it would treat the cookie as
    # native order (frameLength 352 -> 0x60010000 -> calloc fails, -108). Every Android ABI is LE.
    target_compile_definitions(alac PRIVATE TARGET_RT_LITTLE_ENDIAN=1)
    target_compile_definitions(screenatic_airplay PRIVATE SCREENATIC_WITH_ALAC=1)
    target_link_libraries(screenatic_airplay alac)
else()
    message(WARNING "Screenatic: third_party/alac missing - ALAC (audio-only AirPlay) disabled")
endif()

# Oboe (Apache-2.0): low-latency audio output, AAudio on API 27+, OpenSL ES below.
find_package(oboe REQUIRED CONFIG)
target_link_libraries(screenatic_airplay android mediandk log oboe::oboe)

# --- UxPlay protocol core (GPL-3.0), pinned to v1.73.7 -------------------------
# Enabled once third_party/UxPlay is checked out (scripts/fetch-uxplay.sh) AND
# OpenSSL (libcrypto) + libplist are cross-compiled into third_party/prebuilt/<abi>/.
# Until then a stub server is built so the app runs end to end.
if(EXISTS ${UXPLAY_DIR}/lib/raop.c AND EXISTS ${PREBUILT_DIR}/lib/libcrypto.a)
    message(STATUS "Screenatic: building with UxPlay from ${UXPLAY_DIR}")
    # Build from a patched copy so the submodule stays pristine. Patches live in
    # protocol-airplay/patches/ and are re-applied on every CMake configure.
    set(UXPLAY_LIB ${CMAKE_CURRENT_BINARY_DIR}/uxplay/lib)
    file(REMOVE_RECURSE ${CMAKE_CURRENT_BINARY_DIR}/uxplay)
    file(COPY ${UXPLAY_DIR}/lib DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/uxplay)
    file(GLOB UXPLAY_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/../../../patches/*.patch)
    list(SORT UXPLAY_PATCHES)
    foreach(p ${UXPLAY_PATCHES})
        execute_process(COMMAND patch -p1 --forward --silent -i ${p}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/uxplay RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "Screenatic: failed to apply UxPlay patch ${p}")
        endif()
        message(STATUS "Screenatic: applied ${p}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${UXPLAY_PATCHES})
    file(GLOB UXPLAY_SRC
        ${UXPLAY_LIB}/*.c
        ${UXPLAY_LIB}/playfair/*.c
        ${UXPLAY_LIB}/llhttp/*.c)
    # dnssd.c is kept: it builds the exact AirPlay/RAOP TXT records (features, pk, ...).
    # It includes <dns_sd.h>; compat/dns_sd.h + dns_sd_shim.c provide the TXTRecord*
    # helpers and a no-op DNSServiceRegister, because NsdManager does the actual publishing.
    add_library(uxplay_core STATIC ${UXPLAY_SRC} compat/dns_sd_shim.c)
    target_include_directories(uxplay_core PUBLIC
        ${UXPLAY_LIB} ${UXPLAY_LIB}/playfair ${UXPLAY_LIB}/llhttp
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
        ${PREBUILT_DIR}/include)
    # Same defines UxPlay's lib/CMakeLists.txt uses on Linux (+ libplist >= 2.3).
    target_compile_definitions(uxplay_core PRIVATE
        NOHOLD STANDALONE TARGET_POSIX _LINUX _REENTRANT PLIST_210 PLIST_230)
    target_link_libraries(uxplay_core
        ${PREBUILT_DIR}/lib/libplist-2.0.a
        ${PREBUILT_DIR}/lib/libcrypto.a)
    target_compile_definitions(screenatic_airplay PRIVATE SCREENATIC_WITH_UXPLAY=1)
    target_link_libraries(screenatic_airplay uxplay_core)
else()
    message(WARNING "Screenatic: UxPlay or prebuilt deps not found - building STUB AirPlay server")
endif()
