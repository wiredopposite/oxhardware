idf_component_register(
    SRCS "cfg_bus.cpp" "cfg_cmds.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board host_link esp_driver_spi esp_driver_gpio esp_timer
)
