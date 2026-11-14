idf_component_register(
    SRCS "chip_update.cpp"
    INCLUDE_DIRS "include"
    REQUIRES oxprog
    PRIV_REQUIRES cfg_bus nor fpga_link app_update esp_partition esp_timer
)
