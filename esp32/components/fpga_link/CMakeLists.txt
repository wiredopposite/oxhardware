idf_component_register(
    SRCS "fpga_link.cpp" "link_cmds.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board host_link esp_driver_spi esp_driver_gpio
)
