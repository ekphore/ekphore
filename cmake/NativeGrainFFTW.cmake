# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later

include_guard(GLOBAL)

set(EKPHORE_NATIVE_GRAIN_FFTW_ARCHIVE "" CACHE FILEPATH
    "Offline official FFTW3.3.9 archive; its SHA256 is always verified")
set(EKPHORE_NATIVE_GRAIN_FFTW_INCLUDE_DIR "" CACHE PATH
    "Explicit verified FFTW float header directory (requires LIBRARY)")
set(EKPHORE_NATIVE_GRAIN_FFTW_LIBRARY "" CACHE FILEPATH
    "Explicit verified FFTW3.3.9 float static library (requires INCLUDE_DIR)")
option(EKPHORE_NATIVE_GRAIN_FFTW_REUSE_CONTRACT
    "Acknowledge explicit FFTW reuse satisfies the documented native arithmetic contract" OFF)

if(EKPHORE_NATIVE_GRAIN_FFTW_INCLUDE_DIR OR EKPHORE_NATIVE_GRAIN_FFTW_LIBRARY)
    if(NOT EKPHORE_NATIVE_GRAIN_FFTW_INCLUDE_DIR OR NOT EKPHORE_NATIVE_GRAIN_FFTW_LIBRARY)
        message(FATAL_ERROR "Native grain FFTW reuse requires both INCLUDE_DIR and LIBRARY")
    endif()
    if(NOT EKPHORE_NATIVE_GRAIN_FFTW_REUSE_CONTRACT)
        message(FATAL_ERROR "Native grain FFTW reuse requires REUSE_CONTRACT=ON; ordinary system FFTW may contract scalar arithmetic")
    endif()
    if(NOT EXISTS "${EKPHORE_NATIVE_GRAIN_FFTW_INCLUDE_DIR}/fftw3.h" OR
       NOT EXISTS "${EKPHORE_NATIVE_GRAIN_FFTW_LIBRARY}")
        message(FATAL_ERROR "Explicit native grain FFTW header/library does not exist")
    endif()
else()
    if(NOT UNIX OR NOT CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        message(FATAL_ERROR "Native grain FFTW autotools build requires Unix GCC/Clang; otherwise supply an explicitly verified static dependency")
    endif()
    include(ExternalProject)
    find_program(_grain_fftw_make NAMES gmake make REQUIRED)
    set(_grain_fftw_root "${CMAKE_CURRENT_BINARY_DIR}/native-grain-fftw")
    set(_grain_fftw_source "${_grain_fftw_root}/source")
    set(_grain_fftw_build "${_grain_fftw_root}/build")
    set(EKPHORE_NATIVE_GRAIN_FFTW_INCLUDE_DIR "${_grain_fftw_root}/include")
    set(EKPHORE_NATIVE_GRAIN_FFTW_LIBRARY "${_grain_fftw_build}/.libs/libfftw3f.a")
    file(MAKE_DIRECTORY "${EKPHORE_NATIVE_GRAIN_FFTW_INCLUDE_DIR}")
    set(_grain_fftw_url "https://www.fftw.org/fftw-3.3.9.tar.gz")
    if(EKPHORE_NATIVE_GRAIN_FFTW_ARCHIVE)
        if(NOT EXISTS "${EKPHORE_NATIVE_GRAIN_FFTW_ARCHIVE}")
            message(FATAL_ERROR "Offline FFTW archive does not exist")
        endif()
        set(_grain_fftw_url "${EKPHORE_NATIVE_GRAIN_FFTW_ARCHIVE}")
    endif()
    set(_grain_fftw_flags "-O3 -fomit-frame-pointer -fstrict-aliasing -fPIC -ffp-contract=off")
    set(_grain_fftw_simd --disable-neon)
    set(_grain_fftw_sdk_env)
    if(APPLE)
        set(_grain_fftw_arch "${CMAKE_OSX_ARCHITECTURES}")
        if(NOT _grain_fftw_arch)
            set(_grain_fftw_arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        list(LENGTH _grain_fftw_arch _grain_fftw_arch_count)
        if(_grain_fftw_arch_count GREATER 1)
            message(FATAL_ERROR "Build native grain FFTW separately per Apple architecture, then supply a verified universal static library explicitly")
        endif()
        if(_grain_fftw_arch MATCHES "^(arm64|aarch64)$")
            set(_grain_fftw_simd --enable-neon)
            string(APPEND _grain_fftw_flags " -arch arm64")
        elseif(_grain_fftw_arch STREQUAL "x86_64")
            string(APPEND _grain_fftw_flags " -arch x86_64")
        endif()
        if(IS_DIRECTORY "${CMAKE_OSX_SYSROOT}")
            # keep SDK paths out of CFLAGS: autotools splits paths with spaces,
            # and embedded quotes also break ExternalProject's log script
            list(APPEND _grain_fftw_sdk_env "SDKROOT=${CMAKE_OSX_SYSROOT}")
        endif()
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            string(APPEND _grain_fftw_flags " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
    endif()
    ExternalProject_Add(ekphore_native_grain_fftw_build
        PREFIX "${_grain_fftw_root}"
        SOURCE_DIR "${_grain_fftw_source}"
        BINARY_DIR "${_grain_fftw_build}"
        URL "${_grain_fftw_url}"
        URL_HASH SHA256=bf2c7ce40b04ae811af714deb512510cc2c17b9ab9d6ddcf49fe4487eea7af3d
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        TLS_VERIFY TRUE
        PATCH_COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            <SOURCE_DIR>/api/fftw3.h "${EKPHORE_NATIVE_GRAIN_FFTW_INCLUDE_DIR}/fftw3.h"
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
            "CC=${CMAKE_C_COMPILER}" "CFLAGS=${_grain_fftw_flags}"
            "CPPFLAGS=" "LDFLAGS=" "LIBS="
            ${_grain_fftw_sdk_env}
            <SOURCE_DIR>/configure --enable-float --enable-static
            --disable-shared --disable-fortran --disable-fma --disable-threads
            --disable-openmp --with-pic ${_grain_fftw_simd}
        BUILD_COMMAND "${CMAKE_COMMAND}" -E env ${_grain_fftw_sdk_env}
            "${_grain_fftw_make}" -j2
        BUILD_BYPRODUCTS "${EKPHORE_NATIVE_GRAIN_FFTW_LIBRARY}"
        INSTALL_COMMAND ""
        LOG_CONFIGURE TRUE LOG_BUILD TRUE)
endif()

add_library(ekphore_native_grain_fftw STATIC IMPORTED GLOBAL)
set_target_properties(ekphore_native_grain_fftw PROPERTIES
    IMPORTED_LOCATION "${EKPHORE_NATIVE_GRAIN_FFTW_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${EKPHORE_NATIVE_GRAIN_FFTW_INCLUDE_DIR}")
if(UNIX AND NOT APPLE)
    set_property(TARGET ekphore_native_grain_fftw PROPERTY INTERFACE_LINK_LIBRARIES m)
endif()
if(TARGET ekphore_native_grain_fftw_build)
    add_dependencies(ekphore_native_grain_fftw ekphore_native_grain_fftw_build)
endif()
add_library(Ekphore::NativeGrainFFTW ALIAS ekphore_native_grain_fftw)
