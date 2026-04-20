cmake_minimum_required(VERSION 3.20)
project(Clew VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00)
endif()

# MSVC-specific settings
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Option to enable WebView2 GUI support
option(CLEW_ENABLE_WEBVIEW2 "Enable WebView2 GUI support" OFF)

# ============================================================
# Dependencies (vcpkg)
# ============================================================
find_package(quill CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)

# ============================================================
# Clew main executable
# ============================================================

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/WinDivert-2.2.2-A/include
)

# Link directories
link_directories(
    ${CMAKE_SOURCE_DIR}/WinDivert-2.2.2-A/x64
)

# Source files (header-only architecture, single translation unit)
set(CLEW_SOURCES
    src/main.cpp
)

# Embed the Clew icon via a Win32 resource file.
if(WIN32)
    list(APPEND CLEW_SOURCES ${CMAKE_SOURCE_DIR}/assets/clew.rc)
endif()

# Executable
if(CLEW_ENABLE_WEBVIEW2)
    add_executable(clew WIN32 ${CLEW_SOURCES})
    target_compile_definitions(clew PRIVATE CLEW_HAS_WEBVIEW2)
else()
    add_executable(clew ${CLEW_SOURCES})
endif()

# UAC elevation: embed manifest requesting administrator privileges
# (WinDivert and SetInterfaceDnsSettings both require elevation)
if(MSVC)
    target_link_options(clew PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

# Base libraries
set(CLEW_LIBRARIES
    ws2_32
    iphlpapi
    WinDivert
    quill::quill
    nlohmann_json::nlohmann_json
    httplib::httplib
    asio::asio
)

# WebView2 configuration
if(CLEW_ENABLE_WEBVIEW2)
    set(WEBVIEW2_PATH "${CMAKE_SOURCE_DIR}/src/ui/third_party/WebView2" CACHE PATH "Path to WebView2 SDK")

    if(EXISTS "${WEBVIEW2_PATH}/build/native/include/WebView2.h")
        message(STATUS "Found WebView2 SDK at ${WEBVIEW2_PATH}")
        include_directories("${WEBVIEW2_PATH}/build/native/include")

        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(WEBVIEW2_ARCH "x64")
        else()
            set(WEBVIEW2_ARCH "x86")
        endif()

        if(EXISTS "${WEBVIEW2_PATH}/build/native/${WEBVIEW2_ARCH}/WebView2LoaderStatic.lib")
            list(APPEND CLEW_LIBRARIES
                "${WEBVIEW2_PATH}/build/native/${WEBVIEW2_ARCH}/WebView2LoaderStatic.lib"
            )
            message(STATUS "Using WebView2LoaderStatic.lib")
        elseif(EXISTS "${WEBVIEW2_PATH}/build/native/${WEBVIEW2_ARCH}/WebView2Loader.dll.lib")
            list(APPEND CLEW_LIBRARIES
                "${WEBVIEW2_PATH}/build/native/${WEBVIEW2_ARCH}/WebView2Loader.dll.lib"
            )
            add_custom_command(TARGET clew POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_PATH}/build/native/${WEBVIEW2_ARCH}/WebView2Loader.dll"
                "$<TARGET_FILE_DIR:clew>"
            )
            message(STATUS "Using WebView2Loader.dll")
        else()
            message(WARNING "WebView2Loader library not found")
        endif()
    else()
        message(WARNING "WebView2 SDK not found at ${WEBVIEW2_PATH}")
    endif()
endif()

# Link libraries
target_link_libraries(clew ${CLEW_LIBRARIES})

# Copy WinDivert DLL to output directory
add_custom_command(TARGET clew POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/WinDivert-2.2.2-A/x64/WinDivert.dll"
    "$<TARGET_FILE_DIR:clew>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/WinDivert-2.2.2-A/x64/WinDivert64.sys"
    "$<TARGET_FILE_DIR:clew>"
)
