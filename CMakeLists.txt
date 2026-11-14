cmake_minimum_required(VERSION 3.22)

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
