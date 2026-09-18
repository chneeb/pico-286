add_library(picocalc-lcd INTERFACE)

target_sources(picocalc-lcd INTERFACE ${CMAKE_CURRENT_LIST_DIR}/picocalc_lcd.c)

target_link_libraries(picocalc-lcd INTERFACE hardware_pio hardware_clocks hardware_dma)

target_include_directories(picocalc-lcd INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_generate_pio_header(picocalc-lcd
        ${CMAKE_CURRENT_LIST_DIR}/picocalc_lcd.pio
)
