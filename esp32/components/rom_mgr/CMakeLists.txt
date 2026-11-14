idf_component_register(
    SRCS "rom_mgr.cpp"
    INCLUDE_DIRS "include"
    REQUIRES oxprog nor fpga_link fpga_svc cfg_bus smbus chip_update esp_timer freertos
)
