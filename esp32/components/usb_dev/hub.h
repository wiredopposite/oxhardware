#pragma once

// A full-speed hub, device 0 on the FPGA's port. The other devices sit behind its ports;
// each answers at address 0 only after the host resets its port. Attach and detach
// work at any time, from the task that runs usbd_task().

#include <stdbool.h>
#include <stdint.h>

#include "usbd/usbd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_PORTS_MAX   7U      // the status change bitmap is one byte

usbd_handle_t *hub_init(uint8_t ports);     // before any device behind it
bool hub_attach(uint8_t port, usbd_handle_t *dev);     // port 1..ports
usbd_handle_t *hub_detach(uint8_t port);                // the device unplugged, or NULL
void hub_task(usbd_handle_t *handle);

#ifdef __cplusplus
}
#endif
