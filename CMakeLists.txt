cmake_minimum_required(VERSION 3.22)

# OTA protocol compatibility is visible in the app descriptor before any erase.
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE DASH_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT DASH_REV)
    set(DASH_REV "local")
endif()
option(DASH_OTA_TEST_FAIL_BOOT "OTA rollback verification image; never a normal release" OFF)
set(DASH_OTA_BUILD_TAG "" CACHE STRING "Optional short label for an OTA release")
if(DASH_OTA_BUILD_TAG AND NOT DASH_OTA_BUILD_TAG MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "DASH_OTA_BUILD_TAG must contain only letters, digits, '_' or '-'")
endif()
if(DASH_OTA_TEST_FAIL_BOOT)
    set(PROJECT_VER "ota1-test-${DASH_REV}")
else()
    set(PROJECT_VER "ota1-${DASH_REV}")
endif()
if(DASH_OTA_BUILD_TAG)
    string(APPEND PROJECT_VER "-${DASH_OTA_BUILD_TAG}")
endif()
string(SUBSTRING "${PROJECT_VER}" 0 31 PROJECT_VER)

# Shared with the macOS simulator: LVGL (pinned), cJSON, dash_model, dash_ui.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components
    ${CMAKE_CURRENT_LIST_DIR}/third_party/lvgl
    ${CMAKE_CURRENT_LIST_DIR}/third_party/cjson)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Use third_party/lv_conf.h only (same file as the simulator), never LVGL's Kconfig.
idf_build_set_property(COMPILE_DEFINITIONS "LV_KCONFIG_IGNORE" APPEND)
project(esp_dash)
