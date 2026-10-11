idf_component_register(
    SRCS "src/touch.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_lcd_touch_cst9220
    PRIV_REQUIRES config esp_driver_gpio esp_driver_i2c esp_timer log
)

# esp_lcd_touch / gpio 的配置结构体是 C 写的，字段只会越来越多；
# C++ 这边用聚合初始化必然缺字段。和 display 组件同样的处理。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
