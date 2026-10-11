idf_component_register(
    SRCS "src/usb_console.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_tinyusb
    PRIV_REQUIRES osal esp_timer esp_system soc
)

# esp_timer_create_args_t 只初始化我们关心的字段，其余走零值默认。
# IDF 默认 -Wextra -Werror，在 C++ 下 partial initializer 会变成硬错误。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
