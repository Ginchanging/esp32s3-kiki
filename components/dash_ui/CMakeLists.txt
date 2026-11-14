# Pure LVGL UI, shared by firmware and simulator. No ESP-IDF includes.
set(DASH_UI_SRCS
    dash_ui.c
    dash_i18n.c
    dash_cat.c
    dash_pix.c
    sprites/dash_sprites.c
    fonts/dash_font_zh12.c
    fonts/dash_font_zh16.c
    fonts/dash_font_zh20.c
    fonts/dash_font_clock.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${DASH_UI_SRCS} INCLUDE_DIRS "include" REQUIRES lvgl dash_model)
    # frame tables list only the fields they need
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
else()
    add_library(dash_ui STATIC ${DASH_UI_SRCS})
    target_include_directories(dash_ui PUBLIC include)
    target_link_libraries(dash_ui PUBLIC lvgl dash_model)
    target_compile_options(dash_ui PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    set_source_files_properties(fonts/dash_font_zh12.c fonts/dash_font_zh16.c
        fonts/dash_font_zh20.c fonts/dash_font_clock.c sprites/dash_sprites.c PROPERTIES COMPILE_OPTIONS "-w")
endif()
