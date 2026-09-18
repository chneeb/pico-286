add_library(picocalc-kbd INTERFACE)

target_sources(picocalc-kbd INTERFACE ${CMAKE_CURRENT_LIST_DIR}/picocalc_kbd.c)

target_link_libraries(picocalc-kbd INTERFACE hardware_i2c hardware_gpio pico_stdlib)

target_include_directories(picocalc-kbd INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)
