idf_component_register(
    SRCS "usb_dev.cpp" "hid_test.c" "hub.c" "xid_pad.c"
    INCLUDE_DIRS "include"
    REQUIRES usbd fpga_link fpga_svc host_link esp_timer
)
