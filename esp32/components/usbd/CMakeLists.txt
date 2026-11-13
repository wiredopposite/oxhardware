idf_component_register(
    SRCS "usbd.c" "dcd_fpga.cpp"
    INCLUDE_DIRS "include" "include/common"
    REQUIRES fpga_link esp_hw_support
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC USBD_DEVICES_MAX=4)
