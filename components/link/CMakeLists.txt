idf_component_register(
    SRCS "src/link.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES config display osal usb_console esp_timer log
)
