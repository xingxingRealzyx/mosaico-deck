idf_component_register(
    SRCS "src/audio.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_codec_dev
    PRIV_REQUIRES config osal esp_driver_i2s esp_driver_i2c esp_driver_gpio log
)

# The esp_codec_dev / i2s config structs are written in C and gain fields over time
# (this time it was clk_src), so C++ aggregate initialization necessarily misses fields.
# Handled the same way as the display / touch / input components.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
