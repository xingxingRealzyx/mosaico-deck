idf_component_register(
    SRCS "src/display.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES osal
        config
        esp_driver_gpio
        esp_lcd
        esp_timer
        freertos
        log
)

# espressif/esp_lcd_co5300 is declared in this directory's idf_component.yml,
# so the component manager wires it into this component's dependencies
# automatically; there is no need to list it again here.

# IDF's CO5300 configuration macros are written for C, and expanding them only
# initializes a subset of the struct fields. That is normal in C, but IDF enables
# -Wextra -Werror by default, which turns it into a compile error under C++.
# Those fields already have the correct default value (0), so we only downgrade
# this single warning and do not relax anything else.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
