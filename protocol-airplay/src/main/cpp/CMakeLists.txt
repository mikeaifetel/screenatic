cmake_minimum_required(VERSION 3.22.1)
project(screenhop_airplay C CXX)

set(UXPLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/UxPlay)
set(PREBUILT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/prebuilt/${ANDROID_ABI})

add_library(screenhop_airplay SHARED
    jni_bridge.cpp
    video_decoder.cpp
    airplay_server.cpp
    audio_player.cpp
)

# Oboe (Apache-2.0): low-latency audio output, AAudio on API 27+, OpenSL ES below.
find_package(oboe REQUIRED CONFIG)
target_link_libraries(screenhop_airplay android mediandk log oboe::oboe)

# --- UxPlay protocol core (GPL-3.0), pinned to v1.73.7 -------------------------
# Enabled once third_party/UxPlay is checked out (scripts/fetch-uxplay.sh) AND
# OpenSSL (libcrypto) + libplist are cross-compiled into third_party/prebuilt/<abi>/.
# Until then a stub server is built so the app runs end to end.
if(EXISTS ${UXPLAY_DIR}/lib/raop.c AND EXISTS ${PREBUILT_DIR}/lib/libcrypto.a)
    message(STATUS "ScreenHop: building with UxPlay from ${UXPLAY_DIR}")
    file(GLOB UXPLAY_SRC
        ${UXPLAY_DIR}/lib/*.c
        ${UXPLAY_DIR}/lib/playfair/*.c
        ${UXPLAY_DIR}/lib/llhttp/*.c)
    # dnssd.c is kept: it builds the exact AirPlay/RAOP TXT records (features, pk, ...).
    # It includes <dns_sd.h>; compat/dns_sd.h + dns_sd_shim.c provide the TXTRecord*
    # helpers and a no-op DNSServiceRegister, because NsdManager does the actual publishing.
    add_library(uxplay_core STATIC ${UXPLAY_SRC} compat/dns_sd_shim.c)
    target_include_directories(uxplay_core PUBLIC
        ${UXPLAY_DIR}/lib ${UXPLAY_DIR}/lib/playfair ${UXPLAY_DIR}/lib/llhttp
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
        ${PREBUILT_DIR}/include)
    # Same defines UxPlay's lib/CMakeLists.txt uses on Linux (+ libplist >= 2.3).
    target_compile_definitions(uxplay_core PRIVATE
        NOHOLD STANDALONE TARGET_POSIX _LINUX _REENTRANT PLIST_210 PLIST_230)
    target_link_libraries(uxplay_core
        ${PREBUILT_DIR}/lib/libplist-2.0.a
        ${PREBUILT_DIR}/lib/libcrypto.a)
    target_compile_definitions(screenhop_airplay PRIVATE SCREENHOP_WITH_UXPLAY=1)
    target_link_libraries(screenhop_airplay uxplay_core)
else()
    message(WARNING "ScreenHop: UxPlay or prebuilt deps not found - building STUB AirPlay server")
endif()
