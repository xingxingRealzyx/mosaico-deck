# Note the name esp_driver_gpio: IDF 6.x split the old "driver" into esp_driver_*,
# driver/gpio.h is now provided by esp_driver_gpio, and that is only a **private**
# dependency of the old driver component (it does not propagate to users). So
# writing "driver" here produces this build error:
#   input.cpp includes driver/gpio.h, provided by esp_driver_gpio component(s).
#   However, esp_driver_gpio is not in the requirements list of "input".
idf_component_register(
    SRCS "src/input.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES config esp_timer esp_driver_gpio log
)

# gpio_config_t is a C struct and gains fields (this project already tripped over
# hys_ctrl_mode once), so C++ aggregate initialization will inevitably be missing
# some. Same treatment as the display / touch components.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
