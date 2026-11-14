# Vendored cJSON v1.7.19 (MIT). Shared by the ESP-IDF firmware and the macOS simulator.
if(ESP_PLATFORM)
    idf_component_register(SRCS "cJSON.c" INCLUDE_DIRS ".")
else()
    add_library(cjson STATIC cJSON.c)
    target_include_directories(cjson PUBLIC ${CMAKE_CURRENT_LIST_DIR})
endif()
