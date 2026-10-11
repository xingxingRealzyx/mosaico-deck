idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        config
        osal
        display
        input
        touch
        usb_console
        esp_driver_i2c
        esp_driver_gpio
        esp_timer
        log
        heap
)

# IDF 的 i2c_master_bus_config_t / gpio_config_t 只初始化我们关心的字段，
# 其余走零值默认。IDF 默认 -Wextra -Werror，在 C++ 下 partial initializer
# 会变成硬错误。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
