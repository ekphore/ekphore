# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Prepare the float/scalar FFTW archive for the explicit MSVC reuse path.
if(NOT DEFINED FFTW_ROOT)
    message(FATAL_ERROR "Pass -DFFTW_ROOT=<build directory>")
endif()
file(MAKE_DIRECTORY "${FFTW_ROOT}")
file(DOWNLOAD https://www.fftw.org/fftw-3.3.9.tar.gz
    "${FFTW_ROOT}/fftw.tar.gz"
    EXPECTED_HASH SHA256=bf2c7ce40b04ae811af714deb512510cc2c17b9ab9d6ddcf49fe4487eea7af3d
    TLS_VERIFY ON)
file(ARCHIVE_EXTRACT INPUT "${FFTW_ROOT}/fftw.tar.gz" DESTINATION "${FFTW_ROOT}")
set(source "${FFTW_ROOT}/fftw-3.3.9")
file(READ "${source}/CMakeLists.txt" body)
# Upstream 3.3.9 puts /bigobj in definitions instead of compiler options.
string(REPLACE "target_compile_definitions (\${fftw3_lib} PRIVATE /bigobj)"
    "target_compile_options (\${fftw3_lib} PRIVATE /bigobj)" body "${body}")
file(WRITE "${source}/CMakeLists.txt" "${body}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${FFTW_ROOT}/build" -G Ninja
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${FFTW_ROOT}/install
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    -DCMAKE_C_FLAGS=/fp:strict -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF
    -DENABLE_FLOAT=ON -DENABLE_THREADS=OFF -DENABLE_OPENMP=OFF -DDISABLE_FORTRAN=ON
    -DENABLE_SSE=OFF -DENABLE_SSE2=OFF -DENABLE_AVX=OFF -DENABLE_AVX2=OFF
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${FFTW_ROOT}/build"
    --target install --parallel 2 COMMAND_ERROR_IS_FATAL ANY)
