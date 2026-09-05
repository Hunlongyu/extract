cmake_minimum_required(VERSION 3.25)

project(Extract VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Extract currently targets Windows only.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "The initial build supports the MSVC-compatible toolchain only.")
endif()

enable_language(RC)

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
    /DYNAMICBASE /NXCOMPAT /guard:cf
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_options(extract_project_options INTERFACE /HIGHENTROPYVA)
endif()

configure_file(cmake/version.h.in generated/extract/version.h @ONLY)
configure_file(resources/version.rc.in generated/version.rc @ONLY)

add_executable(Extract WIN32
    src/app/main.cpp
    src/platform/console.cpp
    src/platform/console.h
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc"
    resources/app.manifest
)
target_include_directories(Extract PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(Extract PRIVATE extract_project_options shell32)
set_target_properties(Extract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)

install(TARGETS Extract RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs FILES_MATCHING PATTERN "*.md")
install(FILES third_party/README.md DESTINATION third_party)
