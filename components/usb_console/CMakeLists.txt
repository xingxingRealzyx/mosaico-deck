idf_component_register(
    SRCS "src/usb_console.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_tinyusb
    PRIV_REQUIRES osal esp_timer esp_system soc
)

# esp_timer_create_args_t only initializes the fields we care about; the rest take
# zero defaults. IDF enables -Wextra -Werror by default, which turns a partial
# initializer into a hard error under C++.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
