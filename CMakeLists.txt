cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(EditorAI VERSION 2.1.10)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

if (ANDROID)
    # NDK r28's libc++ (required: Geode 5.7+ needs clang >= 19) exposes a
    # missing <type_traits> include in the bundled nontype_functional
    # dependency. Force-include it for every TU; harmless otherwise.
    add_compile_options(-include type_traits)
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Workaround for SDK 5.10.1 iOS bindings bug: CCKeyboardDispatcher modify has empty address
# Codegen generates `GEODE_APPLY_MODIFY_FOR_CONSTRUCTOR(, Default, ...)` which expands to
# `static auto address = ;` -> error: expected expression on iOS. Patch at configure and build time.
if (IOS OR "${CMAKE_SYSTEM_NAME}" STREQUAL "iOS")
    # Configure-time patch (after codegen has run once)
    set(_ios_patch_file "${CMAKE_CURRENT_BINARY_DIR}/bindings/bindings/Geode/modify/CCKeyboardDispatcher.hpp")
    if (EXISTS "${_ios_patch_file}")
        file(READ "${_ios_patch_file}" _ios_content)
        string(REPLACE "GEODE_APPLY_MODIFY_FOR_CONSTRUCTOR(, Default, cocos2d::CCKeyboardDispatcher, )" "// PATCHED IOS: constructor disabled" _ios_content "${_ios_content}")
        string(REPLACE "GEODE_APPLY_MODIFY_FOR_DESTRUCTOR(, Default, cocos2d::CCKeyboardDispatcher)" "// PATCHED IOS: destructor disabled" _ios_content "${_ios_content}")
        file(WRITE "${_ios_patch_file}" "${_ios_content}")
        message(STATUS "Patched CCKeyboardDispatcher.hpp for iOS (configure-time)")
    endif()
    # Build-time patch via custom target (runs every build before main lib)
    add_custom_target(patch-ios-bindings ALL
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-ios.cmake"
        COMMENT "Patching iOS bindings CCKeyboardDispatcher if needed"
        VERBATIM
    )
    # Ensure patch runs before compiling the mod
    add_dependencies(${PROJECT_NAME} patch-ios-bindings)
endif()

# ImGui overlay — the mod's primary UI (E key on desktop, floating bubble
# on mobile). Platforms without it fall back to the classic cocos popups
# (src/overlay.cpp compiles to nothing; main.cpp re-adds the AI button).
if (WIN32 OR APPLE OR ANDROID)
    # IMGUI_VERSION is read by gd-imgui-cocos's own CMakeLists (its CPM
    # fetch of dear imgui) — not dead, despite no reference in this file.
    # Main-HEAD backend needs >= 1.89.5 (AddMouseSourceEvent).
    set(IMGUI_VERSION "v1.92.4")
    # Pinned to main HEAD: the 1.0.0 tag (Dec 2023) predates Geode 5.x APIs.
    CPMAddPackage("gh:matcool/gd-imgui-cocos#b93f08ccef778a53ebba09b20c347f6a63980119")
    target_link_libraries(${PROJECT_NAME} imgui-cocos)
    target_compile_definitions(${PROJECT_NAME} PRIVATE EDITORAI_HAS_IMGUI=1)
endif()

setup_geode_mod(${PROJECT_NAME})
