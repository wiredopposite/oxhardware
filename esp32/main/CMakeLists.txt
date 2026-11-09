idf_component_register(
    SRCS "main.cpp" "sys_cmds.cpp"
    INCLUDE_DIRS "."
    REQUIRES host_link esp_app_format esp_system esp_timer
)
