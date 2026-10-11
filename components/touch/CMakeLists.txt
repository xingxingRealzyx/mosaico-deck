idf_component_register(
    SRCS "src/touch.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_lcd_touch_cst9220
    PRIV_REQUIRES config esp_driver_gpio esp_driver_i2c esp_timer log
)

# The esp_lcd_touch / gpio configuration structs are written in C and only ever
# gain more fields; aggregate initialization on the C++ side will inevitably be
# missing some. Same treatment as the display component.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
