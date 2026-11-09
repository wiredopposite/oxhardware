idf_component_register(
    SRCS "host_link.cpp" "cobs.cpp" "crc32.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart
)
