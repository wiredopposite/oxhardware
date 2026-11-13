idf_component_register(
    SRCS "bt_xid.cpp" "xid_map.c"
    INCLUDE_DIRS "include"
    REQUIRES blueretro
    PRIV_REQUIRES usb_dev host_link console_power
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-address-of-packed-member
    # BlueRetro's generic state is in IRAM: word accesses only
    -fno-builtin-memset -fno-builtin-memcpy)
