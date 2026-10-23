# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)
project(ekphore VERSION 0.1.0 LANGUAGES C CXX)

# Canonical headers live beside their implementations. Export a namespaced SDK
# view in the build tree, keeping build-tree and installed includes identical.
set(EKPHORE_PUBLIC_HEADERS
    src/plugin/editor.h
    src/player/engine_ownership.h
    src/ui/native_editor.h
    src/ui/native_ui.h
    src/plugin/nks.h
    src/player/player.h
    src/plugin/plugin.h
    src/ui/ui.h
)
foreach(header IN LISTS EKPHORE_PUBLIC_HEADERS)
    get_filename_component(name "${header}" NAME)
    configure_file("${header}" "${CMAKE_CURRENT_BINARY_DIR}/public/ekphore/${name}" COPYONLY)
endforeach()

# A sampler's default local build must be capable of meeting an audio
# deadline. Keep symbols for reverse-engineering comparisons while enabling
# the optimizer unless the caller explicitly selected another configuration.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type (default: optimized with debug symbols)" FORCE)
endif()
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(SndFile CONFIG QUIET)
if(NOT SndFile_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
    add_library(SndFile::sndfile ALIAS PkgConfig::SNDFILE)
endif()
find_package(PNG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(TIFF REQUIRED)
find_package(Freetype REQUIRED)
include(FetchContent)
find_package(WebP CONFIG QUIET)
if(NOT WebP_FOUND)
    FetchContent_Declare(ekphore_webp_source
        GIT_REPOSITORY https://github.com/webmproject/libwebp.git
        GIT_TAG v1.4.0
        GIT_SHALLOW TRUE)
    set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(ekphore_webp_source)
    if(NOT TARGET WebP::webp)
        add_library(WebP::webp ALIAS webp)
    endif()
endif()

# Native UI resources use SVG for logos and controls. NanoSVG is a small,
# platform-neutral rasterizer and keeps this support identical in every host.
FetchContent_Declare(ekphore_nanosvg_source
    GIT_REPOSITORY https://github.com/memononen/nanosvg.git
    GIT_TAG 9da543e8329fdd81b64eb48742d8ccb09377aed1)
FetchContent_MakeAvailable(ekphore_nanosvg_source)

# Kontakt's Komplete UI modules are Lua programs after NI's declarative
# compiler lowers `@Component { ... }` expressions. Embed the interpreter so
# native instrument UIs behave identically in standalone and every plug-in
# backend without relying on a platform Lua installation.
FetchContent_Declare(ekphore_lua_source
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG v5.4.8
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ekphore_lua_source)
set(EKPHORE_LUA_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c
    ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c
    lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c
    lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
    lutf8lib.c lvm.c lzio.c)
list(TRANSFORM EKPHORE_LUA_SOURCES PREPEND "${ekphore_lua_source_SOURCE_DIR}/")
add_library(ekphore_lua STATIC ${EKPHORE_LUA_SOURCES})
set_target_properties(ekphore_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ekphore_lua PUBLIC
    "${ekphore_lua_source_SOURCE_DIR}")
if(APPLE)
    target_compile_definitions(ekphore_lua PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(ekphore_lua PRIVATE LUA_USE_LINUX)
    target_link_libraries(ekphore_lua PUBLIC ${CMAKE_DL_LIBS} m)
endif()

option(EKPHORE_BUNDLE_UNSUPPORTED_KONTAKT_FONTS
    "Embed locally extracted Kontakt fonts for private validation only" OFF)

# Keep the UI fonts inside every plug-in/standalone binary. This avoids
# platform font lookup and reproduces Kontakt's own embedded-font model.
set(EKPHORE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${EKPHORE_GENERATED_DIR}")
set(EKPHORE_EMBEDDED_FONTS "${EKPHORE_GENERATED_DIR}/embedded_fonts.h")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OpenSans-SemiBold.ttf"
    EKPHORE_OPEN_SANS_SEMIBOLD_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," EKPHORE_OPEN_SANS_SEMIBOLD_BYTES
    "${EKPHORE_OPEN_SANS_SEMIBOLD_HEX}")
set(EKPHORE_PRIVATE_FONT_DECLARATIONS "")
if(EKPHORE_BUNDLE_UNSUPPORTED_KONTAKT_FONTS)
    message(WARNING
        "ekphore is embedding proprietary Kontakt font material for private "
        "validation. This build and its binaries MUST NOT be distributed.")
    foreach(EKPHORE_FONT_RESOURCE IN ITEMS 1 2 3 4)
        if(EKPHORE_FONT_RESOURCE EQUAL 1)
            set(EKPHORE_FONT_FILE standardj.ttf)
        elseif(EKPHORE_FONT_RESOURCE EQUAL 2)
            set(EKPHORE_FONT_FILE r039014j.ttf)
        elseif(EKPHORE_FONT_RESOURCE EQUAL 3)
            set(EKPHORE_FONT_FILE stan0755j.ttf)
        else()
            set(EKPHORE_FONT_FILE prg65__u.ttf)
        endif()
        set(EKPHORE_FONT_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/../secrets/fonts/${EKPHORE_FONT_FILE}")
        if(NOT EXISTS "${EKPHORE_FONT_PATH}")
            message(FATAL_ERROR
                "Missing private validation font: ${EKPHORE_FONT_PATH}")
        endif()
        file(READ "${EKPHORE_FONT_PATH}" EKPHORE_PRIVATE_FONT_HEX HEX)
        string(REGEX REPLACE "(..)" "0x\\1," EKPHORE_PRIVATE_FONT_BYTES
            "${EKPHORE_PRIVATE_FONT_HEX}")
        string(APPEND EKPHORE_PRIVATE_FONT_DECLARATIONS
            "inline constexpr unsigned char resource_${EKPHORE_FONT_RESOURCE}[] = {${EKPHORE_PRIVATE_FONT_BYTES}};\n")
    endforeach()
endif()
file(WRITE "${EKPHORE_EMBEDDED_FONTS}"
    "#pragma once\n#include <cstddef>\nnamespace ekphore::embedded_fonts {\n"
    "inline constexpr unsigned char open_sans_semibold[] = {${EKPHORE_OPEN_SANS_SEMIBOLD_BYTES}};\n"
    "${EKPHORE_PRIVATE_FONT_DECLARATIONS}"
    "}\n")

# the welcome screen draws the logo, so keep it in every plug-in/standalone binary
set(EKPHORE_WELCOME_LOGO "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/ekphore-logo.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EKPHORE_WELCOME_LOGO}")
file(READ "${EKPHORE_WELCOME_LOGO}" EKPHORE_WELCOME_LOGO_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," EKPHORE_WELCOME_LOGO_BYTES "${EKPHORE_WELCOME_LOGO_HEX}")
file(WRITE "${EKPHORE_GENERATED_DIR}/embedded_logo.h.in"
    "#pragma once\nnamespace ekphore::embedded_logo {\n"
    "inline constexpr unsigned char png[] = {${EKPHORE_WELCOME_LOGO_BYTES}};\n"
    "}\n")
configure_file("${EKPHORE_GENERATED_DIR}/embedded_logo.h.in"
    "${EKPHORE_GENERATED_DIR}/embedded_logo.h" COPYONLY)

# Optional local Lumen face, kept under the same non-distributable validation
# policy as the classic fonts. Public builds retain the portable fallback.
if(EKPHORE_BUNDLE_UNSUPPORTED_KONTAKT_FONTS AND EXISTS
        "${CMAKE_CURRENT_SOURCE_DIR}/../secrets/fonts/lumen-roboto-medium.ttf")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../secrets/fonts/lumen-roboto-medium.ttf"
        EKPHORE_LUMEN_HEX HEX)
    string(REGEX REPLACE "(..)" "0x\\1," EKPHORE_LUMEN_BYTES "${EKPHORE_LUMEN_HEX}")
    file(APPEND "${EKPHORE_EMBEDDED_FONTS}"
        "namespace ekphore::embedded_fonts { inline constexpr unsigned char lumen_medium[] = {${EKPHORE_LUMEN_BYTES}}; }\n")
    add_compile_definitions(EKPHORE_HAS_PRIVATE_LUMEN_FONT=1)
endif()

# Finder icon for the macOS app and plug-in bundles
set(EKPHORE_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/ekphore.icns")
set_source_files_properties("${EKPHORE_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

option(EKPHORE_BUILD_CLAP "Build the CLAP instrument plug-in" ON)
option(EKPHORE_BUILD_LV2 "Build the native LV2 instrument plug-in" ON)
option(EKPHORE_BUILD_VST3 "Build the native VST3 instrument plug-in" ON)
option(EKPHORE_BUILD_AU "Build the native AUv2 instrument plug-in on macOS" ON)
option(EKPHORE_BUILD_STANDALONE_UI "Build the SDL3 standalone editor" ON)

add_subdirectory(../libnki ${CMAKE_CURRENT_BINARY_DIR}/libnki EXCLUDE_FROM_ALL)
add_subdirectory(../libksp ${CMAKE_CURRENT_BINARY_DIR}/libksp EXCLUDE_FROM_ALL)
add_subdirectory(../libnks ${CMAKE_CURRENT_BINARY_DIR}/libnks EXCLUDE_FROM_ALL)
if(LIBNKS_BUILD_DAEMON)
    set_target_properties(nksd PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_custom_target(ekphore-nks-daemon ALL DEPENDS nksd)
endif()

add_library(ekphore_dsp_primitives STATIC
    src/dsp/envelope/ahdsr.cpp
    src/dsp/envelope/fm7.cpp
    src/dsp/modulation/simple_lfo.cpp
    src/dsp/modulation/multi_lfo.cpp
    src/dsp/modulation/routing.cpp
    src/dsp/modulation/planes.cpp)
set_target_properties(ekphore_dsp_primitives PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ekphore_dsp_primitives PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/libnki/public)
target_compile_features(ekphore_dsp_primitives PUBLIC cxx_std_20)
target_compile_options(ekphore_dsp_primitives PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>)

add_library(ekphore_ksp_support STATIC src/ksp/midi.cpp src/ksp/observations.cpp)
set_target_properties(ekphore_ksp_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ekphore_ksp_support PUBLIC src)
target_compile_features(ekphore_ksp_support PUBLIC cxx_std_20)
target_compile_options(ekphore_ksp_support PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>)

add_library(ekphore_core
    src/ksp/host.cpp
    src/ksp/host_commands.cpp
    src/ksp/host_commands_ui.cpp
    src/ksp/execution.cpp
    src/ui/browser.cpp
    src/plugin/nks_bridge.cpp
    src/effects/amplifiers/ac_box.cpp
    src/effects/amplifiers/bass_invader.cpp
    src/effects/amplifiers/bass_pro.cpp
    src/effects/amplifiers/cabinet.cpp
    src/effects/stomps/cat.cpp
    src/effects/modulation/chorus.cpp
    src/effects/dynamics/compressor.cpp
    src/effects/dynamics/transient_master.cpp
    src/effects/stomps/cry_wah.cpp
    src/effects/stomps/dstortion.cpp
    src/dsp/sample_rate_converter.cpp
    src/effects/utilities/dyx.cpp
    src/effects/modulation/flair.cpp
    src/effects/reverbs/galois_reverb.cpp
    src/effects/dynamics/hilbert_limiter.cpp
    src/effects/amplifiers/hot_solo.cpp
    src/effects/reverbs/irc.cpp
    src/effects/reverbs/irc_pv.cpp
    src/effects/amplifiers/jump.cpp
    src/effects/reverbs/legacy_reverb.cpp
    src/effects/reverbs/plate_reverb.cpp
    src/effects/modulation/phaser.cpp
    src/effects/modulation/phasis.cpp
    src/effects/delays/psyche_delay.cpp
    src/effects/reverbs/raum.cpp
    src/effects/lo_fi/bite.cpp
    src/effects/stomps/dirt.cpp
    src/effects/modulation/freak.cpp
    src/effects/modulation/vibrato_chorus.cpp
    src/effects/tape/wow_flutter.cpp
    src/effects/amplifiers/ep_preamps.cpp
    src/effects/stomps/fuzz.cpp
    src/effects/spatial/stereo_tune.cpp
    src/effects/mangling/beat_masher.cpp
    src/effects/mangling/beat_slicer.cpp
    src/effects/mangling/gater.cpp
    src/effects/mangling/reverse_grain.cpp
    src/effects/mangling/transpose_stretch.cpp
    src/effects/amplifiers/super_fast_100.cpp
    src/effects/stomps/chainsaw.cpp
    src/effects/stomps/skreamer_deluxe.cpp
    src/effects/stomps/kolor.cpp
    src/effects/amplifiers/reverb_delight.cpp
    src/effects/delays/replika_delay.cpp
    src/effects/modulation/ring_modulator.cpp
    src/effects/modulation/rotator.cpp
    src/effects/utilities/routing.cpp
    src/effects/stomps/skreamer.cpp
    src/effects/utilities/solid_g_eq.cpp
    src/effects/dynamics/supercharger.cpp
    src/effects/tape/tape.cpp
    src/effects/dynamics/transient_master.cpp
    src/effects/delays/twin_delay.cpp
    src/effects/amplifiers/twang.cpp
    src/effects/amplifiers/van51.cpp
    src/filters/current_ladder.cpp
    src/filters/blue_monark.cpp
    src/filters/daft.cpp
    src/filters/dj.cpp
    src/filters/formant.cpp
    src/filters/multi_state_variable.cpp
    src/filters/parametric_eq.cpp
    src/filters/simple_lphp.cpp
    src/filters/state_variable_1.cpp
    src/filters/state_variable_2.cpp
    src/filters/state_variable_4.cpp
    src/filters/state_variable_6.cpp
    src/player/lifecycle.cpp
    src/player/resources.cpp
    src/player/events.cpp
    src/player/voices.cpp
    src/player/modulation.cpp
    src/player/render.cpp
    src/player/voice_effects.cpp
    src/player/routing.cpp
    src/player/ui.cpp
    src/plugin/editor.cpp
    src/plugin/plugin.cpp
    src/plugin/state.cpp
    src/sampling/audio_file.cpp
    src/sampling/sample_path.cpp
    src/sampling/grain_analysis.cpp
    src/sampling/time_machine.cpp
    src/sampling/time_machine_pcm.cpp
    src/sampling/slice_machine.cpp
    src/sampling/elastique_direct.cpp
    src/sampling/elastique_pro_split.cpp
    src/sampling/elastique_pro_psola.cpp
    src/sampling/elastique_pro_analysis.cpp
    src/sampling/elastique_pro_resample.cpp
    src/sampling/elastique_pro_direct.cpp
    src/sampling/time_machine_pro_stream.cpp
    src/sampling/wavetable.cpp
    src/sampling/windowed_sinc.cpp
    src/ui/native/binding.cpp
    src/ui/trace.cpp
    src/ui/native/runtime.cpp
    src/ui/native/renderer.cpp
    src/ui/ui.cpp)
set_source_files_properties(src/effects/amplifiers/hot_solo.cpp
    src/filters/dj.cpp
    src/effects/modulation/flair.cpp
    src/effects/modulation/phasis.cpp
    src/effects/dynamics/compressor.cpp
    src/effects/stomps/dstortion.cpp
    src/effects/modulation/chorus.cpp
    src/effects/reverbs/legacy_reverb.cpp
    src/effects/modulation/phaser.cpp
    src/effects/amplifiers/cabinet.cpp
    src/effects/reverbs/plate_reverb.cpp
    src/sampling/wavetable.cpp src/sampling/time_machine.cpp
    src/sampling/time_machine_pcm.cpp src/sampling/slice_machine.cpp src/sampling/grain_analysis.cpp
    src/sampling/elastique_direct.cpp
    src/sampling/elastique_pro_split.cpp
    src/sampling/elastique_pro_psola.cpp
    src/sampling/elastique_pro_analysis.cpp
    src/sampling/elastique_pro_resample.cpp
    src/sampling/elastique_pro_direct.cpp
    src/sampling/time_machine_pro_stream.cpp
    src/effects/reverbs/irc_pv.cpp
    PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>")
include(cmake/NativeGrainFFTW.cmake)
target_link_libraries(ekphore_core PRIVATE Ekphore::NativeGrainFFTW)
if(APPLE)
    target_sources(ekphore_core PRIVATE src/ui/platform/macos.mm)
    set_source_files_properties(src/ui/platform/macos.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(ekphore_core PUBLIC "-framework AppKit")
    target_link_libraries(ekphore_core PUBLIC "-framework CoreFoundation")
    target_link_libraries(ekphore_core PRIVATE "-framework Accelerate")
elseif(WIN32)
    target_sources(ekphore_core PRIVATE src/ui/platform/win32.cpp)
    target_link_libraries(ekphore_core PUBLIC user32 gdi32 shell32 ole32 comdlg32)
else()
    find_package(X11 REQUIRED)
    target_sources(ekphore_core PRIVATE src/ui/platform/x11.cpp)
    target_link_libraries(ekphore_core PUBLIC X11::X11)
endif()
target_compile_features(ekphore_core PUBLIC cxx_std_20)
target_include_directories(ekphore_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/public" PRIVATE
    "${EKPHORE_GENERATED_DIR}"
    "${ekphore_nanosvg_source_SOURCE_DIR}/src")
target_compile_options(ekphore_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)
if(EKPHORE_BUNDLE_UNSUPPORTED_KONTAKT_FONTS)
    target_compile_definitions(ekphore_core PRIVATE
        EKPHORE_HAS_PRIVATE_KONTAKT_FONTS=1)
endif()
target_link_libraries(ekphore_core PUBLIC
    libnki::libnki
    libksp::libksp
    libnks::libnks
    SndFile::sndfile
    PNG::PNG
    TIFF::TIFF
    WebP::webp
    Freetype::Freetype)
target_link_libraries(ekphore_core PUBLIC ekphore_lua ekphore_dsp_primitives ekphore_ksp_support)

add_executable(ekphore src/main.cpp)
add_library(ekphore_irc_staged STATIC src/effects/reverbs/irc_staged.cpp)
set_target_properties(ekphore_irc_staged PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(ekphore_irc_staged PUBLIC cxx_std_20)
target_compile_options(ekphore_irc_staged PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>)
target_link_libraries(ekphore_core PRIVATE ekphore_irc_staged)
option(EKPHORE_EXPERIMENTAL_STAGED_IRC
    "Use staged IRC in Player (historical option name); native alternative backend routing remains incomplete" ON)
if(EKPHORE_EXPERIMENTAL_STAGED_IRC)
    message(STATUS "IRC backend: staged ordinary (native alternative backend routing/PCM qualification remains open)")
    target_compile_definitions(ekphore_core PRIVATE EKPHORE_EXPERIMENTAL_STAGED_IRC=1)
endif()
target_link_libraries(ekphore PRIVATE ekphore_core)

if(EKPHORE_BUILD_CLAP)
    include(FetchContent)
    FetchContent_Declare(clap
        GIT_REPOSITORY https://github.com/free-audio/clap.git
        GIT_TAG 195b42a004144fab0b3cf95e9c067187d15365b7
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(clap)
    add_library(ekphore-clap MODULE src/plugin/clap_adapter.cpp)
    target_compile_features(ekphore-clap PRIVATE cxx_std_20)
    target_link_libraries(ekphore-clap PRIVATE ekphore_core clap)
    set_target_properties(ekphore-clap PROPERTIES PREFIX "" SUFFIX ".clap")
    if(UNIX AND NOT APPLE)
        # The CLAP entry is the complete public ABI. Keep C++/static-library
        # symbols private when several plug-ins share one host process.
        target_link_options(ekphore-clap PRIVATE
            "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/resources/clap/linux-exports.map")
        set_property(TARGET ekphore-clap APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/clap/linux-exports.map")
    endif()
    if(APPLE)
        set_target_properties(ekphore-clap PROPERTIES BUNDLE TRUE
            BUNDLE_EXTENSION clap
            MACOSX_BUNDLE_GUI_IDENTIFIER io.github.ekphore.clap
            MACOSX_BUNDLE_BUNDLE_NAME ekphore
            MACOSX_BUNDLE_ICON_FILE ekphore.icns)
        target_sources(ekphore-clap PRIVATE "${EKPHORE_ICNS}")
    endif()
endif()

if(EKPHORE_BUILD_VST3)
    include(FetchContent)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(vst3sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
        GIT_TAG 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
        GIT_SHALLOW TRUE
        GIT_SUBMODULES base cmake pluginterfaces public.sdk
        GIT_SUBMODULES_RECURSE TRUE)
    # Steinberg's SDK probes `xcodebuild` even for Make/Ninja builds. Apple's
    # standalone Command Line Tools provide the compiler and macOS SDK but not
    # that command. Supply the minimum detection result only while configuring
    # the dependency; no Xcode-only generator feature is used by ekphore.
    if(APPLE)
        execute_process(COMMAND xcodebuild -version
            RESULT_VARIABLE EKPHORE_XCODEBUILD_RESULT
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT EKPHORE_XCODEBUILD_RESULT EQUAL 0)
            set(EKPHORE_SAVED_XCODE_VERSION "$ENV{XCODE_VERSION}")
            set(ENV{XCODE_VERSION} "15.0")
            set(XCODE_VERSION "15.0")
        endif()
    endif()
    FetchContent_MakeAvailable(vst3sdk)
    if(DEFINED EKPHORE_SAVED_XCODE_VERSION)
        set(ENV{XCODE_VERSION} "${EKPHORE_SAVED_XCODE_VERSION}")
        unset(EKPHORE_SAVED_XCODE_VERSION)
    endif()
    # The SDK omits its required mode define for single-config generators when
    # CMAKE_BUILD_TYPE is intentionally empty. Keep that valid configuration
    # usable without imposing a build type on the whole workspace.
    foreach(EKPHORE_VST3_SDK_TARGET IN ITEMS
            base pluginterfaces sdk_common sdk sdk_hosting validator moduleinfotool)
        if(TARGET ${EKPHORE_VST3_SDK_TARGET})
            target_compile_definitions(${EKPHORE_VST3_SDK_TARGET} PRIVATE
                $<$<CONFIG:Debug>:DEVELOPMENT=1>
                $<$<NOT:$<CONFIG:Debug>>:RELEASE=1>)
        endif()
    endforeach()

    smtg_add_vst3plugin(ekphore-vst3
        PACKAGE_NAME "ekphore"
        src/plugin/vst3_adapter.cpp)
    if(APPLE)
        # the SDK only fills the bundle identifier for Xcode builds
        set_target_properties(ekphore-vst3 PROPERTIES
            MACOSX_BUNDLE_GUI_IDENTIFIER io.github.ekphore.vst3
            MACOSX_BUNDLE_ICON_FILE ekphore.icns)
        target_sources(ekphore-vst3 PRIVATE "${EKPHORE_ICNS}")
    endif()
    if(WIN32 AND SMTG_CREATE_BUNDLE_FOR_WINDOWS AND NOT CMAKE_CONFIGURATION_TYPES)
        # the SDK only configures Windows binary paths for multi-config generators
        get_target_property(EKPHORE_VST3_PACKAGE_PATH ekphore-vst3 SMTG_PLUGIN_PACKAGE_PATH)
        get_target_property(EKPHORE_VST3_PACKAGE_CONTENTS ekphore-vst3 SMTG_PLUGIN_PACKAGE_CONTENTS)
        get_target_property(EKPHORE_VST3_ARCHITECTURE ekphore-vst3 SMTG_WIN_ARCHITECTURE_NAME)
        set_target_properties(ekphore-vst3 PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${EKPHORE_VST3_PACKAGE_PATH}/${EKPHORE_VST3_PACKAGE_CONTENTS}/${EKPHORE_VST3_ARCHITECTURE}"
            RUNTIME_OUTPUT_DIRECTORY "${EKPHORE_VST3_PACKAGE_PATH}/${EKPHORE_VST3_PACKAGE_CONTENTS}/${EKPHORE_VST3_ARCHITECTURE}")
    endif()
    if(APPLE)
        target_sources(ekphore-vst3 PRIVATE
            "${vst3sdk_SOURCE_DIR}/public.sdk/source/main/macmain.cpp")
    elseif(WIN32)
        target_sources(ekphore-vst3 PRIVATE
            "${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp")
    else()
        target_sources(ekphore-vst3 PRIVATE
            "${vst3sdk_SOURCE_DIR}/public.sdk/source/main/linuxmain.cpp")
    endif()
    target_compile_features(ekphore-vst3 PRIVATE cxx_std_20)
    target_compile_definitions(ekphore-vst3 PRIVATE
        $<$<CONFIG:Debug>:DEVELOPMENT=1>
        $<$<NOT:$<CONFIG:Debug>>:RELEASE=1>)
    target_link_libraries(ekphore-vst3 PRIVATE ekphore_core sdk)
    smtg_target_configure_version_file(ekphore-vst3)

endif()

if(EKPHORE_BUILD_LV2)
    include(FetchContent)
    FetchContent_Declare(lv2
        GIT_REPOSITORY https://github.com/lv2/lv2.git
        GIT_TAG 0bcde338db1c63bbc503b4d1f6d7b55ed43154af
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(lv2)

    add_library(ekphore-lv2 MODULE src/plugin/lv2_adapter.cpp)
    target_compile_features(ekphore-lv2 PRIVATE cxx_std_20)
    target_include_directories(ekphore-lv2 PRIVATE "${lv2_SOURCE_DIR}/include")
    target_link_libraries(ekphore-lv2 PRIVATE ekphore_core)
    set_target_properties(ekphore-lv2 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "ekphore"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ekphore.lv2"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ekphore.lv2")

    if(WIN32)
        set(EKPHORE_LV2_BINARY "ekphore.dll")
        set_target_properties(ekphore-lv2 PROPERTIES SUFFIX ".dll")
    elseif(APPLE)
        set(EKPHORE_LV2_BINARY "ekphore.dylib")
        set_target_properties(ekphore-lv2 PROPERTIES SUFFIX ".dylib")
    else()
        set(EKPHORE_LV2_BINARY "ekphore.so")
        set_target_properties(ekphore-lv2 PROPERTIES SUFFIX ".so")
    endif()
    if(APPLE)
        set(EKPHORE_LV2_UI_CLASS "ui:CocoaUI")
    elseif(WIN32)
        set(EKPHORE_LV2_UI_CLASS "ui:WindowsUI")
    else()
        set(EKPHORE_LV2_UI_CLASS "ui:X11UI")
    endif()
    configure_file(resources/lv2/manifest.ttl.in
        "${CMAKE_CURRENT_BINARY_DIR}/ekphore.lv2/manifest.ttl" @ONLY)
    configure_file(resources/lv2/ekphore.ttl.in
        "${CMAKE_CURRENT_BINARY_DIR}/ekphore.lv2/ekphore.ttl" @ONLY)
endif()

if(APPLE AND EKPHORE_BUILD_AU)
    include(FetchContent)
    FetchContent_Declare(audiounitsdk
        GIT_REPOSITORY https://github.com/apple/AudioUnitSDK.git
        GIT_TAG bd98b31feff57a15989fcfab4cd86dc63382b1ac
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(audiounitsdk)

    add_library(ekphore-au-sdk STATIC
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/AUBase.cpp"
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/AUBuffer.cpp"
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/AUBufferAllocator.cpp"
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/AUInputElement.cpp"
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/AUMIDIBase.cpp"
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/AUOutputElement.cpp"
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/AUPlugInDispatch.cpp"
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/AUScopeElement.cpp"
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/ComponentBase.cpp"
        "${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK/MusicDeviceBase.cpp")
    target_compile_features(ekphore-au-sdk PUBLIC cxx_std_23)
    target_include_directories(ekphore-au-sdk PUBLIC
        "${audiounitsdk_SOURCE_DIR}/include")
    target_link_libraries(ekphore-au-sdk PUBLIC
        "-framework AudioToolbox" "-framework CoreFoundation")

    configure_file(resources/au/Info.plist.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/ekphore-au-Info.plist" @ONLY)
    configure_file(resources/au/PkgInfo
        "${CMAKE_CURRENT_BINARY_DIR}/generated/ekphore-au-PkgInfo" COPYONLY)
    add_library(ekphore-au MODULE
        src/plugin/au_adapter.cpp
        src/plugin/au_ui.mm
        "${EKPHORE_ICNS}")
    set_source_files_properties(src/plugin/au_ui.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_compile_features(ekphore-au PRIVATE cxx_std_23)
    target_link_libraries(ekphore-au PRIVATE ekphore_core ekphore-au-sdk)
    set_target_properties(ekphore-au PROPERTIES
        PREFIX ""
        OUTPUT_NAME "ekphore"
        BUNDLE TRUE
        BUNDLE_EXTENSION component
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_BINARY_DIR}/generated/ekphore-au-Info.plist"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/AU")
    add_custom_command(TARGET ekphore-au POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/generated/ekphore-au-PkgInfo"
            "$<TARGET_BUNDLE_CONTENT_DIR:ekphore-au>/PkgInfo"
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:ekphore-au>"
        COMMENT "Ad-hoc signing ekphore AUv2 component")
endif()

if(EKPHORE_BUILD_STANDALONE_UI)
    find_package(SDL3 CONFIG QUIET)
    if(SDL3_FOUND)
        add_library(ekphore_browser INTERFACE)
        target_link_libraries(ekphore_browser INTERFACE ekphore_core)
        add_executable(ekphore-ui WIN32 src/plugin/standalone.cpp)
        target_compile_features(ekphore-ui PRIVATE cxx_std_20)
        target_link_libraries(ekphore-ui PRIVATE ekphore_browser SDL3::SDL3)
        # macOS gets the padded variant, so the dock icon doesn't change once SDL sets it
        if(APPLE)
            set(EKPHORE_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/ekphore-macos.png")
        else()
            set(EKPHORE_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/ekphore.png")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EKPHORE_APP_ICON}")
        file(READ "${EKPHORE_APP_ICON}" EKPHORE_APP_ICON_HEX HEX)
        string(REGEX REPLACE "(..)" "0x\\1," EKPHORE_APP_ICON_BYTES "${EKPHORE_APP_ICON_HEX}")
        file(WRITE "${EKPHORE_GENERATED_DIR}/app_icon.h.in"
            "#pragma once\nnamespace ekphore::app_icon {\n"
            "inline constexpr unsigned char png[] = {${EKPHORE_APP_ICON_BYTES}};\n"
            "}\n")
        configure_file("${EKPHORE_GENERATED_DIR}/app_icon.h.in"
            "${EKPHORE_GENERATED_DIR}/app_icon.h" COPYONLY)
        target_include_directories(ekphore-ui PRIVATE "${EKPHORE_GENERATED_DIR}")
        if(WIN32)
            # rc may resolve relative paths from the build dir, so bake in an absolute one
            set(EKPHORE_ICO "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/ekphore.ico")
            configure_file(resources/windows/ekphore-ui.rc.in
                "${EKPHORE_GENERATED_DIR}/ekphore-ui.rc" @ONLY)
            target_sources(ekphore-ui PRIVATE "${EKPHORE_GENERATED_DIR}/ekphore-ui.rc")
        endif()
    else()
        message(STATUS "SDL3 not found; skipping the standalone editor")
    endif()
endif()
