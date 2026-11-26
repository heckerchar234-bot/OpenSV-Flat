cmake_minimum_required(VERSION 3.25)
project(OpenSVPlus VERSION 1.0.0 DESCRIPTION "Standalone OpenSV neural singing synthesis engine" LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(OPENSV_BUILD_CLI "Build the opensv-plus command-line program" ON)
option(OPENSV_BUILD_SHARED "Build the versioned OpenSV C shared library" ON)
set(OPENSV_JUCE_DIR "" CACHE PATH "Use an installed JUCE 9.0.3 source tree instead of the included copy")

if(OPENSV_JUCE_DIR)
    if(NOT EXISTS "${OPENSV_JUCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "OPENSV_JUCE_DIR must point to a JUCE checkout containing CMakeLists.txt")
    endif()
    set(juce_SOURCE_DIR "${OPENSV_JUCE_DIR}")
else()
    set(juce_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE")
endif()

if(NOT EXISTS "${juce_SOURCE_DIR}/modules/juce_core/juce_core.h")
    message(FATAL_ERROR "JUCE modules are missing. Restore third_party/JUCE from this OpenSV+ source package.")
endif()

set(JUCE_MODULES_DIR "${juce_SOURCE_DIR}/modules")
set(JUCE_VERSION "9.0.3")
include("${juce_SOURCE_DIR}/extras/Build/CMake/JUCEModuleSupport.cmake")
juce_add_modules(ALIAS_NAMESPACE juce
    "${JUCE_MODULES_DIR}/juce_core"
    "${JUCE_MODULES_DIR}/juce_audio_basics"
    "${JUCE_MODULES_DIR}/juce_audio_formats"
    "${JUCE_MODULES_DIR}/juce_dsp")

file(GLOB opensv_synthesis_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/synthesis/*.cpp")

add_library(opensv_synthesis STATIC ${opensv_synthesis_sources})
add_library(OpenSVPlus::Synthesis ALIAS opensv_synthesis)
target_include_directories(opensv_synthesis PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_definitions(opensv_synthesis PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(opensv_synthesis PUBLIC
    juce::juce_core juce::juce_audio_basics juce::juce_audio_formats juce::juce_dsp)

if(MSVC)
    target_compile_options(opensv_synthesis PRIVATE /utf-8 /EHsc /permissive-)
else()
    target_compile_options(opensv_synthesis PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(OPENSV_BUILD_SHARED)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(opensv_library_kind STATIC)
    else()
        set(opensv_library_kind SHARED)
    endif()
    add_library(opensv_native ${opensv_library_kind} src/api/OpenSVApi.cpp)
    add_library(OpenSVPlus::C ALIAS opensv_native)
    set_target_properties(opensv_native PROPERTIES
        OUTPUT_NAME opensv_native
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
    target_compile_definitions(opensv_native PRIVATE
        OPENSV_BUILDING_LIBRARY=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_include_directories(opensv_native PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    target_link_libraries(opensv_native PRIVATE opensv_synthesis)
    if(MSVC)
        target_compile_options(opensv_native PRIVATE /utf-8 /EHsc /permissive-)
    endif()

    if(ANDROID)
        target_link_options(opensv_native PRIVATE "LINKER:-z,max-page-size=16384")
    endif()

    install(TARGETS opensv_native
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/opensv/opensv.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/opensv)
endif()

if(OPENSV_BUILD_CLI)
    add_executable(opensv-plus tools/opensv-plus.cpp)
    target_include_directories(opensv-plus PRIVATE src include)
    target_compile_definitions(opensv-plus PRIVATE
        OPENSV_STATIC=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_sources(opensv-plus PRIVATE src/api/OpenSVApi.cpp)
    target_link_libraries(opensv-plus PRIVATE opensv_synthesis)
    if(MSVC)
        target_compile_options(opensv-plus PRIVATE /utf-8 /EHsc /permissive-)
        target_link_libraries(opensv-plus PRIVATE shell32)
    endif()
    install(TARGETS opensv-plus RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/opensv-plus)
install(FILES third_party/JUCE/LICENSE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/opensv-plus RENAME LICENSE.JUCE.md)
