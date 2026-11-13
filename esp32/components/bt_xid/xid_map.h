#pragma once

// BlueRetro output callbacks for the Xbox gamepad (xid_map.c)

#include "blueretro.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XID_MAP_STATE_SIZE  18U     // xid_in_t

void xid_map_meta_init(struct wired_ctrl *ctrl_data);
void xid_map_init_buffer(int32_t dev_mode, struct wired_data *wired_data);
void xid_map_from_generic(int32_t dev_mode, struct wired_ctrl *ctrl_data, struct wired_data *wired_data);
void xid_map_fb_to_generic(int32_t dev_mode, struct raw_fb *raw_fb_data, struct generic_fb *fb_data);
// The console's rumble (XID output report motors) for output wired_id
void xid_map_rumble(uint8_t wired_id, uint16_t left, uint16_t right);

#ifdef __cplusplus
}
#endif
