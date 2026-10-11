idf_component_register(
    SRCS "src/osal.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer esp_ringbuf
)
