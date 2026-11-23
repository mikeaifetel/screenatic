cmake_minimum_required(VERSION 3.15)

project(ProbeNative
        VERSION 1.0
        LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fail at compile time for unresolved symbols. This can catch cases
# where we forget to add .cpp files to the CMake target shared object
# library. If your use case requires dynamic symbol linking, remove
# these lines.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_link_options("LINKER:--no-undefined" "LINKER:-z,defs")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL Android)
  add_compile_definitions(ANDROID)
endif()

# Shared with the Android build: the AirPlay core wrapper and UxPlay (+ our patches).
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(AIRPLAY_CPP ${REPO_ROOT}/protocol-airplay/src/main/cpp)
set(UXPLAY_DIR ${REPO_ROOT}/protocol-airplay/third_party/UxPlay)
set(ALAC_DIR ${REPO_ROOT}/protocol-airplay/third_party/alac/codec)
# Static deps from scripts/build-deps-vega.sh, one set per Vega arch.
set(PREBUILT_DIR ${REPO_ROOT}/vega/third_party/prebuilt/${VODKA_ARCH})
if(NOT EXISTS ${PREBUILT_DIR}/lib/libcrypto.a)
  message(FATAL_ERROR "Screenatic: ${PREBUILT_DIR} missing - run scripts/build-deps-vega.sh")
endif()

set(HEADERS
  kepler/turbo-modules/generated/ProbeNativeSpec.h
  kepler/turbo-modules/ProbeNative.h
  kepler/turbo-modules/AirPlayTest.h
)
set(SOURCES
    kepler/turbo-modules/generated/ProbeNativeSpec.cpp
    kepler/turbo-modules/ProbeNative.cpp
    kepler/turbo-modules/AirPlayTest.cpp
    ${AIRPLAY_CPP}/airplay_server.cpp
    kepler/AutoLinkInit.cpp
)

find_package(turbomoduleAPI CONFIG REQUIRED)

kepler_add_turbo_module_library(ProbeNative SHARED ${HEADERS} ${SOURCES})

# --- UxPlay protocol core (GPL-3.0), patched copy, same setup as the Android build -----------
set(UXPLAY_LIB ${CMAKE_CURRENT_BINARY_DIR}/uxplay/lib)
file(REMOVE_RECURSE ${CMAKE_CURRENT_BINARY_DIR}/uxplay)
file(COPY ${UXPLAY_DIR}/lib DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/uxplay)
file(GLOB UXPLAY_PATCHES CONFIGURE_DEPENDS ${REPO_ROOT}/protocol-airplay/patches/*.patch)
list(SORT UXPLAY_PATCHES)
foreach(p ${UXPLAY_PATCHES})
  execute_process(COMMAND patch -p1 --forward --silent -i ${p}
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/uxplay RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "Screenatic: failed to apply UxPlay patch ${p}")
  endif()
endforeach()
file(GLOB UXPLAY_SRC ${UXPLAY_LIB}/*.c ${UXPLAY_LIB}/playfair/*.c ${UXPLAY_LIB}/llhttp/*.c)
# dns_sd shim: UxPlay's dnssd.c only builds the TXT records; we publish them ourselves.
add_library(uxplay_core STATIC ${UXPLAY_SRC} ${AIRPLAY_CPP}/compat/dns_sd_shim.c)
set_target_properties(uxplay_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(uxplay_core PUBLIC
    ${UXPLAY_LIB} ${UXPLAY_LIB}/playfair ${UXPLAY_LIB}/llhttp
    ${AIRPLAY_CPP}/compat ${PREBUILT_DIR}/include)
target_compile_definitions(uxplay_core PRIVATE
    NOHOLD STANDALONE TARGET_POSIX _LINUX _REENTRANT PLIST_210 PLIST_230)
target_compile_options(uxplay_core PRIVATE -w)

# Apple ALAC decoder (Apache-2.0), for audio-only AirPlay.
add_library(alac STATIC
    ${ALAC_DIR}/ALACDecoder.cpp ${ALAC_DIR}/ALACBitUtilities.c ${ALAC_DIR}/ag_dec.c
    ${ALAC_DIR}/dp_dec.c ${ALAC_DIR}/matrix_dec.c ${ALAC_DIR}/EndianPortable.c)
set_target_properties(alac PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(alac PUBLIC ${ALAC_DIR})
target_compile_options(alac PRIVATE -w)
target_compile_definitions(alac PRIVATE TARGET_RT_LITTLE_ENDIAN=1)

target_include_directories(ProbeNative PRIVATE ${AIRPLAY_CPP} ${PREBUILT_DIR}/include)
target_compile_definitions(ProbeNative PRIVATE SCREENATIC_WITH_UXPLAY=1 SCREENATIC_WITH_ALAC=1)
list(APPEND LIBRARIES
    uxplay_core alac
    ${PREBUILT_DIR}/lib/libplist-2.0.a
    ${PREBUILT_DIR}/lib/libavcodec.a
    ${PREBUILT_DIR}/lib/libavutil.a
    ${PREBUILT_DIR}/lib/libFLAC.a
    ${PREBUILT_DIR}/lib/libcrypto.a
    pthread dl m)

if (CMAKE_SYSTEM_NAME STREQUAL Android)
    list(APPEND LIBRARIES
        android log)
endif()
target_link_libraries(ProbeNative PRIVATE ${LIBRARIES})

install(TARGETS ProbeNative)
