idf_component_register(
    SRCS "nor.cpp" "nor_cmds.cpp"
    INCLUDE_DIRS "include"
    REQUIRES fpga_link cfg_bus host_link esp_timer
)
