idf_component_register(
    SRCS "src/hostlink.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES config display audio osal usb_console esp_timer log
)
