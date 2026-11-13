idf_component_register(
    SRCS "usb_dev.cpp" "hub.c" "xid_pad.c"
    INCLUDE_DIRS "include"
    REQUIRES usbd cfg_bus fpga_link fpga_svc host_link esp_timer
)
