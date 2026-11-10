idf_component_register(
    SRCS "prog_server.cpp" "mailbox.cpp" "prog_cmds.cpp"
    INCLUDE_DIRS "include"
    REQUIRES oxprog fpga_link smbus host_link esp_app_format esp_system
)
