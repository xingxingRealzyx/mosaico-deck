idf_component_register(
    SRCS "src/audio.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_codec_dev
    PRIV_REQUIRES config osal esp_driver_i2s esp_driver_i2c esp_driver_gpio log
)

# esp_codec_dev / i2s 的配置结构体是 C 写的、字段会增（这次就在 clk_src 上踩了），
# C++ 这边聚合初始化必然缺字段。和 display / touch / input 组件同样处理。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
