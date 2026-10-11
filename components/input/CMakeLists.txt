# 注意 esp_driver_gpio 这个名字：IDF 6.x 把老的 "driver" 拆成了 esp_driver_*，
# driver/gpio.h 现在由 esp_driver_gpio 提供，而它只是老 driver 组件的 **私有**
# 依赖（不向使用者传递）。所以这里写 "driver" 会在构建时报
#   input.cpp includes driver/gpio.h, provided by esp_driver_gpio component(s).
#   However, esp_driver_gpio is not in the requirements list of "input".
idf_component_register(
    SRCS "src/input.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES config esp_timer esp_driver_gpio log
)

# gpio_config_t 是 C 结构体、字段会增（本工程已经在 hys_ctrl_mode 上踩过一次），
# C++ 聚合初始化必然缺字段。和 display / touch 组件同样处理。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
