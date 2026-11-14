# macOS simulator for the ESP32-S3 dashboard.
# Builds the SAME LVGL sources, lv_conf.h and dash_* components as the firmware.
cmake_minimum_required(VERSION 3.16)
project(dash_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# LVGL: use the shared lv_conf.h, no Kconfig, no demos/examples/ThorVG.
set(LV_BUILD_CONF_PATH ${ROOT}/third_party/lv_conf.h CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(LV_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_get_variable(SDL2_PREFIX_INCLUDE sdl2 includedir)

add_subdirectory(${ROOT}/third_party/lvgl ${CMAKE_BINARY_DIR}/lvgl EXCLUDE_FROM_ALL)
target_compile_definitions(lvgl PUBLIC DASH_LV_SIM=1)
target_include_directories(lvgl PUBLIC ${SDL2_PREFIX_INCLUDE} ${SDL2_INCLUDE_DIRS})
target_link_directories(lvgl PUBLIC ${SDL2_LIBRARY_DIRS})
target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})

add_subdirectory(${ROOT}/third_party/cjson ${CMAKE_BINARY_DIR}/cjson)
add_subdirectory(${ROOT}/components/dash_model ${CMAKE_BINARY_DIR}/dash_model)
add_subdirectory(${ROOT}/components/dash_ui ${CMAKE_BINARY_DIR}/dash_ui)

add_executable(dash_sim main.c)
target_link_libraries(dash_sim PRIVATE dash_ui dash_model cjson lvgl)
target_compile_options(dash_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
