idf_component_register(
    SRCS "fpga_svc.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board fpga_link cfg_bus host_link esp_driver_gpio freertos
)
