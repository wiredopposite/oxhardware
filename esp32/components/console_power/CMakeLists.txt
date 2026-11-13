idf_component_register(
    SRCS "console.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board fpga_link fpga_svc host_link esp_driver_gpio esp_timer
)
