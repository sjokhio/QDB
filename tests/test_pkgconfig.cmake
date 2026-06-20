# cmake -P script: install qdb to a temp prefix and verify pkg-config output.
#
# Required -D variables:
#   BUILD_DIR        cmake binary directory
#   PROJECT_VERSION  expected version string (e.g. 1.1.0)
#   PKG_CONFIG       path to pkg-config or pkgconf executable
#   C_COMPILER       C compiler to use for the compile smoke test
#
# Design note — baked prefix vs. test prefix:
#   qdb.pc has prefix=@CMAKE_INSTALL_PREFIX@ baked in at configure time.
#   This test installs to a temporary directory that differs from that prefix.
#   To get correct include/lib paths, every prefix-sensitive pkg-config call
#   uses --define-variable=prefix=<TEMP_PREFIX> to override the baked value.
#   --modversion does not depend on the prefix so it is queried without the
#   override first to confirm the Version: field is present and correct.

cmake_minimum_required(VERSION 3.20)

foreach(_var BUILD_DIR PROJECT_VERSION PKG_CONFIG C_COMPILER)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "Required variable ${_var} not set")
    endif()
endforeach()

set(TEMP_PREFIX "${BUILD_DIR}/_pkgconfig_test_prefix")
file(REMOVE_RECURSE "${TEMP_PREFIX}")
file(MAKE_DIRECTORY "${TEMP_PREFIX}")

# ---------------------------------------------------------------------------
# Install the library to the temp prefix.
# ---------------------------------------------------------------------------

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${TEMP_PREFIX}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE  _errout
)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed:\n${_output}\n${_errout}")
endif()

# ---------------------------------------------------------------------------
# Locate qdb.pc.
# Use find(1) instead of a glob so that lib64/ and multiarch paths
# (lib/x86_64-linux-gnu/) are handled without special-casing.
# ---------------------------------------------------------------------------

execute_process(
    COMMAND find "${TEMP_PREFIX}" -name "qdb.pc"
    OUTPUT_VARIABLE _pc_file
    RESULT_VARIABLE _find_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(_find_result OR NOT _pc_file)
    message(FATAL_ERROR "qdb.pc not found under ${TEMP_PREFIX}")
endif()
get_filename_component(_pc_dir "${_pc_file}" DIRECTORY)

set(ENV{PKG_CONFIG_PATH} "${_pc_dir}")

# ---------------------------------------------------------------------------
# 1. --modversion: prefix-independent; verify the Version: field.
# ---------------------------------------------------------------------------

execute_process(
    COMMAND "${PKG_CONFIG}" --modversion qdb
    OUTPUT_VARIABLE _found_version
    RESULT_VARIABLE _result
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "pkg-config --modversion qdb failed")
endif()
if(NOT _found_version STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "Version mismatch: got '${_found_version}', expected '${PROJECT_VERSION}'")
endif()

# ---------------------------------------------------------------------------
# 2. --cflags / --libs: override the baked configure-time prefix with the
#    actual temp install location so the paths are resolvable.
# ---------------------------------------------------------------------------

set(_pfx "--define-variable=prefix=${TEMP_PREFIX}")

execute_process(
    COMMAND "${PKG_CONFIG}" "${_pfx}" --cflags qdb
    OUTPUT_VARIABLE _cflags
    RESULT_VARIABLE _result
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "pkg-config --cflags qdb failed")
endif()
if(NOT _cflags MATCHES "-I")
    message(FATAL_ERROR "pkg-config --cflags missing -I flag: '${_cflags}'")
endif()

execute_process(
    COMMAND "${PKG_CONFIG}" "${_pfx}" --libs qdb
    OUTPUT_VARIABLE _libs
    RESULT_VARIABLE _result
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "pkg-config --libs qdb failed")
endif()
if(NOT _libs MATCHES "-lqdb")
    message(FATAL_ERROR "pkg-config --libs missing -lqdb: '${_libs}'")
endif()

# ---------------------------------------------------------------------------
# 3. Compile a minimal consumer using the resolved flags.
# ---------------------------------------------------------------------------

set(_consumer_src "${TEMP_PREFIX}/pkgconfig_smoke.c")
file(WRITE "${_consumer_src}"
[=[
#include <qdb.h>
int main(void)
{
    (void)qdb_errmsg(0);
    return 0;
}
]=])

separate_arguments(_cflags_list UNIX_COMMAND "${_cflags}")
separate_arguments(_libs_list   UNIX_COMMAND "${_libs}")

execute_process(
    COMMAND "${C_COMPILER}"
            ${_cflags_list}
            "${_consumer_src}"
            ${_libs_list}
            -o "${TEMP_PREFIX}/pkgconfig_smoke"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE  _errout
)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR
        "Compile smoke test failed:\n${_output}\n${_errout}")
endif()

file(REMOVE_RECURSE "${TEMP_PREFIX}")
message(STATUS
    "PASS: version=${_found_version}  cflags=${_cflags}  libs=${_libs}")
