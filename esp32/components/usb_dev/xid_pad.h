#pragma once

// An Xbox gamepad (XID class 0x58, subclass 0x42) as its own device, the way a retail
// controller's pad sits behind the controller's hub.

#include <stdint.h>

#include "usbd/usbd.h"

#ifdef __cplusplus
extern "C" {
#endif

// The 20-byte input report after its two header bytes
typedef struct __attribute__((packed)) {
    uint8_t  buttons;           // XID_BTN_*
    uint8_t  reserved;
    uint8_t  analog[8];         // A, B, X, Y, black, white, left trigger, right trigger
    int16_t  lx, ly, rx, ry;
} xid_in_t;

// The 6-byte output report after its two header bytes
typedef struct __attribute__((packed)) {
    uint16_t left;              // rumble motors
    uint16_t right;
} xid_out_t;

#define XID_BTN_UP      0x01U
#define XID_BTN_DOWN    0x02U
#define XID_BTN_LEFT    0x04U
#define XID_BTN_RIGHT   0x08U
#define XID_BTN_START   0x10U
#define XID_BTN_BACK    0x20U
#define XID_BTN_LS      0x40U
#define XID_BTN_RS      0x80U

usbd_handle_t *xid_pad_init(void);
void xid_pad_task(usbd_handle_t *handle);
// From any task: the pad's state; sent when it changes
void xid_pad_set(const xid_in_t *state);
void xid_pad_rumble(xid_out_t *out);
uint32_t xid_pad_reports(void);         // IN reports the host has taken
uint32_t xid_pad_rumbles(void);         // output reports received

#ifdef __cplusplus
}
#endif
