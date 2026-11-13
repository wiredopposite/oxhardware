#pragma once

// USB device controller driver for the FPGA's USB device (docs/01-interface.md §5.9).
// usbd talks to it through DCD_DRIVER_FPGA; the task that runs usbd_task() also calls
// dcd_fpga_poll() first, which collects the FPGA's events for every device.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads and clears the FPGA's USB events and queues them for usbd_task(). Returns true
// if there was anything (call usbd_task() and poll again).
bool dcd_fpga_poll(void);

// Events still queued for usbd_task() (a SETUP held back behind an earlier completion)
bool dcd_fpga_pending(void);

typedef struct {
    uint32_t polls;           // polls that found events
    uint32_t bus_resets;
    uint32_t setups;
    uint32_t in_done;
    uint32_t out_done;
    uint32_t suspends;
    uint32_t link_errors;
    uint8_t  addr[8];         // each device's USB address, 0xFF = none
} dcd_fpga_stats_t;

void dcd_fpga_get_stats(dcd_fpga_stats_t *stats);

#ifdef __cplusplus
}
#endif
