cmake_minimum_required(VERSION 3.25)

project(Extract VERSION 0.8.1 LANGUAGES C CXX)

# 部分 Windows CMake 将 MSVC 的 UTF-8 中文 /showIncludes 前缀按 GBK
# 再编码，Ninja 因而漏记头文件依赖。仅修正已观察到的错误前缀；
# 英文或其它正常探测结果保持不变。
if(CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "^娉ㄦ剰:")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件: ")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${CMAKE_CL_SHOWINCLUDES_PREFIX}")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_CL_SHOWINCLUDES_PREFIX}")
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Extract currently targets Windows only.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "The initial build supports the MSVC-compatible toolchain only.")
endif()

enable_language(RC)

# Ninja 的目标架构来自 MSVC 环境，防止预设目录名与实际编译器不一致。
if(EXTRACT_TARGET_ARCHITECTURE)
    set(extract_compiler_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    string(TOLOWER "${extract_compiler_arch}" extract_compiler_arch)
    if(NOT extract_compiler_arch STREQUAL EXTRACT_TARGET_ARCHITECTURE)
        message(FATAL_ERROR "Requested ${EXTRACT_TARGET_ARCHITECTURE}, but compiler targets ${extract_compiler_arch}. Use scripts/build.ps1 -Architecture or the matching vcvars environment.")
    endif()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(extract_project_options INTERFACE)
target_compile_definitions(extract_project_options INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    WINVER=0x0A00 _WIN32_WINNT=0x0A00
)
target_compile_options(extract_project_options INTERFACE
    "$<$<COMPILE_LANGUAGE:C,CXX>:/W4;/WX;/utf-8;/GS;/sdl;/guard:cf>"
    "$<$<COMPILE_LANGUAGE:CXX>:/permissive-;/Zc:__cplusplus;/EHsc>"
)
target_link_options(extract_project_options INTERFACE
    # 完整生成 CFG 目标表，避免本机 MSVC 增量链接跳板遗漏导致运行时快速失败。
    /DYNAMICBASE /NXCOMPAT /guard:cf /INCREMENTAL:NO
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_options(extract_project_options INTERFACE /HIGHENTROPYVA)
endif()

configure_file(cmake/version.h.in generated/extract/version.h @ONLY)
configure_file(resources/version.rc.in generated/version.rc @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/logo.ico"
)

add_library(extract_lzma STATIC third_party/lzma/LzmaDec.c third_party/lzma/Lzma2Dec.c
    third_party/lzma/7zArcIn.c third_party/lzma/7zDec.c third_party/lzma/7zBuf.c
    third_party/lzma/7zCrc.c third_party/lzma/7zCrcOpt.c third_party/lzma/7zStream.c
    third_party/lzma/Bcj2.c third_party/lzma/Bra.c third_party/lzma/Bra86.c
    third_party/lzma/BraIA64.c third_party/lzma/CpuArch.c third_party/lzma/Delta.c)
target_compile_definitions(extract_lzma PRIVATE Z7_EXTRACT_ONLY)
target_include_directories(extract_lzma SYSTEM PUBLIC third_party/lzma)
add_library(extract_zlib STATIC
    third_party/zlib/adler32.c third_party/zlib/crc32.c third_party/zlib/inflate.c
    third_party/zlib/inffast.c third_party/zlib/inftrees.c third_party/zlib/zutil.c)
target_include_directories(extract_zlib SYSTEM PUBLIC third_party/zlib)
target_compile_definitions(extract_zlib PRIVATE Z_SOLO)
add_library(extract_bzip2 STATIC
    third_party/bzip2/blocksort.c third_party/bzip2/huffman.c third_party/bzip2/crctable.c
    third_party/bzip2/randtable.c third_party/bzip2/compress.c third_party/bzip2/decompress.c third_party/bzip2/bzlib.c)
target_include_directories(extract_bzip2 SYSTEM PUBLIC third_party/bzip2)
target_compile_definitions(extract_bzip2 PRIVATE _CRT_SECURE_NO_WARNINGS)
add_library(extract_nsis_bzip2 STATIC src/codecs/nsis_bzip.c
    third_party/nsis_bzip2/bzlib.c third_party/nsis_bzip2/decompress.c third_party/nsis_bzip2/huffman.c)
target_include_directories(extract_nsis_bzip2 PRIVATE third_party/nsis_bzip2)
foreach(codec IN ITEMS extract_lzma extract_zlib extract_bzip2 extract_nsis_bzip2)
    target_compile_options(${codec} PRIVATE /W3 /utf-8 /GS /guard:cf)
endforeach()

add_library(extract_core STATIC
    src/core/types.cpp
    src/core/progress.cpp
    src/core/control.cpp
    src/core/package.cpp
    src/core/tree.cpp
    src/platform/files.cpp
    src/platform/log.cpp
    src/io/output.cpp
    src/io/input.cpp
    src/codecs/cab.cpp
    src/codecs/stream.cpp
    src/formats/msi.cpp
    src/formats/cab.cpp
    src/formats/burn.cpp
    src/formats/pe.cpp
    src/formats/inno.cpp
    src/formats/nsis.cpp
    src/formats/archive.cpp
    src/formats/update_package.cpp
    src/formats/msix.cpp
)
target_include_directories(extract_core PUBLIC src)
target_link_libraries(extract_core PUBLIC extract_project_options msi cabinet bcrypt ole32 ntdll xmllite shlwapi shell32)
target_link_libraries(extract_core PRIVATE extract_lzma extract_zlib extract_bzip2 extract_nsis_bzip2)

add_executable(Extract WIN32
    src/app/main.cpp
    src/platform/console.cpp
    src/platform/console.h
    src/platform/jobs.cpp
    src/platform/worker.cpp
    src/platform/notifications.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc"
    resources/app.manifest
)
target_include_directories(Extract PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(Extract PRIVATE extract_core shell32 propsys advapi32 runtimeobject)
set_target_properties(Extract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(extract_progress_probe tests/progress_probe.cpp)
    target_link_libraries(extract_progress_probe PRIVATE extract_core)
    add_executable(extract_log_probe tests/log_probe.cpp)
    target_link_libraries(extract_log_probe PRIVATE extract_core)
    add_executable(extract_path_tests tests/path_tests.cpp)
    target_link_libraries(extract_path_tests PRIVATE extract_core)
    add_test(NAME path_policy COMMAND extract_path_tests "${CMAKE_CURRENT_BINARY_DIR}/path-tests")
    add_executable(notification_activation_probe EXCLUDE_FROM_ALL tests/notification_activation.cpp)
    target_link_libraries(notification_activation_probe PRIVATE extract_project_options ole32 shell32)
    find_program(EXTRACT_POWERSHELL NAMES pwsh REQUIRED)
    add_test(NAME msi_integration COMMAND "${EXTRACT_POWERSHELL}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.ps1"
        -Executable "$<TARGET_FILE:Extract>"
        -WorkRoot "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures")
    set_tests_properties(msi_integration PROPERTIES TIMEOUT 180)
    find_package(Python3 COMPONENTS Interpreter)
    set(EXTRACT_INNO_COMPILER "" CACHE FILEPATH "Official ISCC.exe used only to generate test fixtures")
    set(EXTRACT_NSIS_COMPILER "" CACHE FILEPATH "Official makensis.exe used only to generate test fixtures")
    set(EXTRACT_7Z_TEST_TOOL "" CACHE FILEPATH "Official 7zr.exe used only to generate archive test fixtures")
    if(Python3_Interpreter_FOUND)
        add_test(NAME msix_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/msix_integration.py"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/msix-fixtures")
        set_tests_properties(msix_integration PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        add_test(NAME update_package_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/update_package_integration.py"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/update-package-fixtures")
        set_tests_properties(update_package_integration PROPERTIES TIMEOUT 240)
        add_test(NAME worker_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_integration.py"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/worker-fixtures")
        set_tests_properties(worker_integration PROPERTIES TIMEOUT 240)
        add_test(NAME progress_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/progress_integration.py"
            --probe "$<TARGET_FILE:extract_progress_probe>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/progress-fixtures")
        set_tests_properties(progress_integration PROPERTIES TIMEOUT 180)
        add_test(NAME large_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/large_integration.py"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/large-fixtures")
        set_tests_properties(large_integration PROPERTIES TIMEOUT 600)
        add_test(NAME log_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/log_integration.py"
            --probe "$<TARGET_FILE:extract_log_probe>"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/log-fixtures")
        set_tests_properties(log_integration PROPERTIES TIMEOUT 120)
        add_test(NAME cab_burn_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/cab_burn_integration.py"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/cab-burn-fixtures")
        set_tests_properties(cab_burn_integration PROPERTIES TIMEOUT 600)
        set(inno_compiler_args)
        if(EXTRACT_INNO_COMPILER)
            list(APPEND inno_compiler_args --compiler "${EXTRACT_INNO_COMPILER}")
        endif()
        add_test(NAME volume_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/volume_integration.py"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/volume-fixtures" ${inno_compiler_args})
        set_tests_properties(volume_integration PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        add_test(NAME inno_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/inno_integration.py"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/inno-fixtures" ${inno_compiler_args})
        set_tests_properties(inno_integration PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        set(nsis_compiler_args)
        if(EXTRACT_NSIS_COMPILER)
            list(APPEND nsis_compiler_args --compiler "${EXTRACT_NSIS_COMPILER}")
        endif()
        add_test(NAME nsis_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/nsis_integration.py"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/nsis-fixtures" ${nsis_compiler_args})
        set_tests_properties(nsis_integration PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        set(nested_compiler_args)
        if(EXTRACT_INNO_COMPILER)
            list(APPEND nested_compiler_args --inno-compiler "${EXTRACT_INNO_COMPILER}")
        endif()
        if(EXTRACT_NSIS_COMPILER)
            list(APPEND nested_compiler_args --nsis-compiler "${EXTRACT_NSIS_COMPILER}")
        endif()
        add_test(NAME nested_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/nested_integration.py"
            --executable "$<TARGET_FILE:Extract>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/nested-fixtures" ${nested_compiler_args})
        set_tests_properties(nested_integration PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        if(EXTRACT_7Z_TEST_TOOL)
            set(archive_compiler_args)
            if(EXTRACT_NSIS_COMPILER)
                list(APPEND archive_compiler_args --nsis-compiler "${EXTRACT_NSIS_COMPILER}")
            endif()
            add_test(NAME archive_integration COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_integration.py"
                --executable "$<TARGET_FILE:Extract>"
                --work-root "${CMAKE_CURRENT_BINARY_DIR}/archive-fixtures"
                --sevenzip "${EXTRACT_7Z_TEST_TOOL}" ${archive_compiler_args})
            set_tests_properties(archive_integration PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        endif()
    endif()
endif()

install(TARGETS Extract RUNTIME DESTINATION .)
install(FILES README.md README.zh-CN.md DESTINATION .)
install(FILES resources/logo.png DESTINATION resources)
install(DIRECTORY docs/ DESTINATION docs FILES_MATCHING PATTERN "*.md" PATTERN "*.csv" PATTERN "*.json")
install(FILES third_party/README.md DESTINATION third_party)
install(FILES third_party/lzma/LICENSE.txt DESTINATION third_party/lzma)
install(FILES third_party/zlib/LICENSE DESTINATION third_party/zlib)
install(FILES third_party/bzip2/LICENSE DESTINATION third_party/bzip2)
install(DIRECTORY third_party/notices/ DESTINATION third_party/notices)
