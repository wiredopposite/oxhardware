/*
 * Copyright (c) 2025, 8BitGeist
 * SPDX-License-Identifier: Apache-2.0
 */

// BlueRetro output for the Xbox gamepad: generic controller state to the 18-byte XID
// state (the input report after its header), and the console's rumble back to generic
// feedback.

#include <string.h>

#include "blueretro.h"
#include "tools/util.h"
#include "xid_map.h"

#define JOY_AXES    4
#define TRIG_AXES   2

enum {
    AN_A, AN_B, AN_X, AN_Y, AN_BLACK, AN_WHITE, AN_LT, AN_RT,
};

#define BTN_UP      0x01
#define BTN_DOWN    0x02
#define BTN_LEFT    0x04
#define BTN_RIGHT   0x08
#define BTN_START   0x10
#define BTN_BACK    0x20
#define BTN_LS      0x40
#define BTN_RS      0x80

struct __packed xid_map_state {
    uint16_t buttons;
    uint8_t analog[8];
    int16_t axes[JOY_AXES];         // LX, LY, RX, RY
};
_Static_assert(sizeof(struct xid_map_state) == 18, "XID state is 18 bytes");

static DRAM_ATTR const struct ctrl_meta axes_meta[ADAPTER_MAX_AXES] = {
    {0, 0, 0x7FFF, 0x8000, 0, -32768, 32767, 0},
    {0, 0, 0x7FFF, 0x8000, 0, -32768, 32767, 0},
    {0, 0, 0x7FFF, 0x8000, 0, -32768, 32767, 0},
    {0, 0, 0x7FFF, 0x8000, 0, -32768, 32767, 0},
    {0, 0, 255, 0, 0, 0, 255, 0},
    {0, 0, 255, 0, 0, 0, 255, 0},
};

static const uint32_t ox_mask[4] = {0xBBFF0FFF, 0x00000000, 0x00000000, BR_COMBO_MASK};
static const uint32_t ox_desc[4] = {0x110000FF, 0x00000000, 0x00000000, 0x00000000};

static DRAM_ATTR const uint16_t btns_mask[32] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    BTN_LEFT, BTN_RIGHT, BTN_DOWN, BTN_UP,
    0, 0, 0, 0,
    0, 0, 0, 0,
    BTN_START, BTN_BACK, 0, 0,
    0, 0, 0, BTN_LS,
    0, 0, 0, BTN_RS,
};

static DRAM_ATTR const int8_t analog_btn_idx[32] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    AN_X, AN_B, AN_A, AN_Y,
    -1, -1, -1, -1,
    -1, AN_WHITE, -1, -1,
    -1, AN_BLACK, -1, -1,
};

static int32_t clamp(int32_t v, const struct ctrl_meta *meta)
{
    return v > meta->size_max ? meta->size_max : v < meta->size_min ? meta->size_min : v;
}

void IRAM_ATTR xid_map_init_buffer(int32_t dev_mode, struct wired_data *wired_data)
{
    (void)dev_mode;
    memset(wired_data->output, 0, sizeof(wired_data->output));
    memset(wired_data->output_mask, 0x00, sizeof(wired_data->output_mask));
}

void xid_map_meta_init(struct wired_ctrl *ctrl_data)
{
    memset((void *)ctrl_data, 0, sizeof(*ctrl_data) * WIRED_MAX_DEV);
    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
        for (uint32_t j = 0; j < ARRAY_SIZE(axes_meta); j++) {
            ctrl_data[i].mask = ox_mask;
            ctrl_data[i].desc = ox_desc;
            ctrl_data[i].axes[j].meta = &axes_meta[j];
        }
    }
}

void xid_map_from_generic(int32_t dev_mode, struct wired_ctrl *ctrl_data, struct wired_data *wired_data)
{
    (void)dev_mode;
    if (ctrl_data == NULL || wired_data == NULL) {
        return;
    }
    // In IRAM, which faults on the byte loads the compiler narrows bit tests to
    const volatile struct wired_ctrl *ctrl = ctrl_data;
    struct xid_map_state st = {0};

    for (uint32_t i = 0; i < ARRAY_SIZE(generic_btns_mask); i++) {
        if (!(ctrl->map_mask[0] & BIT(i))) {
            continue;
        }
        if (ctrl->btns[0].value & generic_btns_mask[i]) {
            if (analog_btn_idx[i] >= 0) {
                st.analog[analog_btn_idx[i]] = 0xFF;
            } else {
                st.buttons |= btns_mask[i];
            }
            wired_data->cnt_mask[i] = ctrl->btns[0].cnt_mask[i];
        } else {
            wired_data->cnt_mask[i] = 0;
        }
    }
    for (uint32_t i = 0; i < JOY_AXES; i++) {
        if (ctrl->map_mask[0] & (axis_to_btn_mask(i) & ctrl->desc[0])) {
            st.axes[i] = (int16_t)clamp(ctrl->axes[i].value, &axes_meta[i]);
        }
        wired_data->cnt_mask[axis_to_btn_id(i)] = ctrl->axes[i].cnt_mask;
    }
    for (uint32_t i = 0; i < TRIG_AXES; i++) {
        const uint32_t a = i + JOY_AXES;
        if (ctrl->map_mask[0] & (axis_to_btn_mask(a) & ctrl->desc[0])) {
            st.analog[AN_LT + i] = (uint8_t)clamp(ctrl->axes[a].value, &axes_meta[a]);
        }
        wired_data->cnt_mask[axis_to_btn_id(a)] = ctrl->axes[a].cnt_mask;
    }
    memcpy(wired_data->output, &st, sizeof(st));
}

void xid_map_fb_to_generic(int32_t dev_mode, struct raw_fb *raw_fb_data, struct generic_fb *fb_data)
{
    (void)dev_mode;
    fb_data->wired_id = raw_fb_data->header.wired_id;
    fb_data->type = raw_fb_data->header.type;
    if (fb_data->type == FB_TYPE_RUMBLE) {
        const uint16_t lf = raw_fb_data->data[0] | raw_fb_data->data[1] << 8;
        const uint16_t hf = raw_fb_data->data[2] | raw_fb_data->data[3] << 8;
        fb_data->state = (lf || hf) ? 1 : 0;
        fb_data->lf_pwr = (uint8_t)(lf >> 8);
        fb_data->hf_pwr = (uint8_t)(hf >> 8);
    }
}

void xid_map_rumble(uint8_t wired_id, uint16_t left, uint16_t right)
{
    struct raw_fb fb = {0};
    fb.header.wired_id = wired_id;
    fb.header.type = FB_TYPE_RUMBLE;
    fb.header.data_len = 4;
    fb.data[0] = (uint8_t)left;
    fb.data[1] = (uint8_t)(left >> 8);
    fb.data[2] = (uint8_t)right;
    fb.data[3] = (uint8_t)(right >> 8);
    blueretro_feedback(&fb);
}
