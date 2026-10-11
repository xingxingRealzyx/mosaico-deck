idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        config
        osal
        display
        hostlink
        audio
        input
        touch
        usb_console
        esp_driver_i2c
        esp_driver_gpio
        esp_timer
        log
        heap
)

# For IDF's i2c_master_bus_config_t / gpio_config_t we only initialize the fields
# we care about; the rest fall back to zero defaults. IDF enables -Wextra -Werror by
# default, and under C++ a partial initializer becomes a hard error.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
