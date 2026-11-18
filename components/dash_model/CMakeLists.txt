# Pure C data model, shared by firmware and simulator. No ESP-IDF includes.
if(ESP_PLATFORM)
    idf_component_register(SRCS "dash_model.c" "dash_schedule.c" "dash_flights.c" "dash_calendar.c" "dash_health.c" "dash_idle.c" "dash_actions.c" INCLUDE_DIRS "include" REQUIRES cjson)
else()
    add_library(dash_model STATIC dash_model.c dash_schedule.c dash_flights.c dash_calendar.c dash_health.c dash_idle.c dash_actions.c)
    target_include_directories(dash_model PUBLIC include)
    target_link_libraries(dash_model PUBLIC cjson)
    target_compile_options(dash_model PRIVATE -Wall -Wextra)
endif()
