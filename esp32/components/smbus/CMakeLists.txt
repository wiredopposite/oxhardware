idf_component_register(
    SRCS "smbus.cpp" "smbus_cmds.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board host_link esp_driver_i2c
)
