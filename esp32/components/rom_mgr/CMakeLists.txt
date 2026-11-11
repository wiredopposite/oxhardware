idf_component_register(
    SRCS "rom_mgr.cpp"
    INCLUDE_DIRS "include"
    REQUIRES oxprog nor fpga_link smbus esp_timer
)
