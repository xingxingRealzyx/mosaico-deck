idf_component_register(
    SRCS "src/display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES osal
    PRIV_REQUIRES
        config
        esp_driver_gpio
        esp_lcd
        esp_timer
        freertos
        log
)

# espressif/esp_lcd_co5300 由本目录的 idf_component.yml 声明，
# 组件管理器会自动接进本组件的依赖，不需要在这里重复列出。

# IDF 的 CO5300 配置宏是给 C 写的，展开后只初始化结构体的一部分字段。
# C 里这是常规写法，但 IDF 默认 -Wextra -Werror，在 C++ 下就变成编译错误。
# 这些字段本来就有正确的默认值（0），所以只降级这一条警告，不做别的放宽。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
