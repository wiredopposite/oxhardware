#pragma once

#include <stdint.h>

#include "usbd/usbd.h"

#ifdef __cplusplus
extern "C" {
#endif

usbd_handle_t *hid_test_init(void);
void hid_test_task(usbd_handle_t *handle);
uint32_t hid_test_reports(void);        // IN reports the host has taken
uint32_t hid_test_out_packets(void);

#ifdef __cplusplus
}
#endif
