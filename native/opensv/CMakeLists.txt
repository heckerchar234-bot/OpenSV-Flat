# CMake wrapper — builds libopensv_native from an external OpenSV+ source tree.
# build.py passes OPENSV_SOURCE_DIR pointing at the OpenSV+ checkout.
cmake_minimum_required(VERSION 3.25)
project(OpenSVWrapper LANGUAGES NONE)

if(NOT DEFINED OPENSV_SOURCE_DIR OR NOT EXISTS "${OPENSV_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Set -DOPENSV_SOURCE_DIR=<path> to the OpenSV+ root that contains CMakeLists.txt.\n"
        "Extract the OpenSV+ source (with JUCE at <source>/third_party/JUCE/) to vendor/opensv-plus/.")
endif()

add_subdirectory("${OPENSV_SOURCE_DIR}" opensv-plus)
