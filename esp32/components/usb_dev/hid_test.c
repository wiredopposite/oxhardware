// A plain HID gamepad for bringing up the FPGA's USB device on a PC: 16 buttons, a hat and
// four axes. It presses and releases button 1 once a second, which shows in the PC's
// game controller settings.
#include "hid_test.h"

#include <stdint.h>

#include "class/hid_def.h"
#include "esp_timer.h"
#include "usb_def.h"
#include "usb_util.h"

#define EP0_SIZE        64U
#define EPADDR_IN       (1U | USB_EP_DIR_IN)
#define EPADDR_OUT      (2U | USB_EP_DIR_OUT)
#define EP_SIZE         64U
#define TOGGLE_US       (1000U * 1000U)

static const usb_desc_device_t DescDevice = {
    .bLength            = sizeof(usb_desc_device_t),
    .bDescriptorType    = USB_DTYPE_DEVICE,
    .bcdUSB             = USB_BCD_VERSION_1_1,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = EP0_SIZE,
    .idVendor           = 0x1209,   // pid.codes test VID/PID
    .idProduct          = 0x0001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

static const uint8_t DescReport[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x35, 0x00,        //   Physical Minimum (0)
    0x45, 0x01,        //   Physical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x10,        //   Report Count (16)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x10,        //   Usage Maximum (16)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x25, 0x07,        //   Logical Maximum (7)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x65, 0x14,        //   Unit (degrees)
    0x09, 0x39,        //   Usage (Hat switch)
    0x81, 0x42,        //   Input (Data, Var, Abs, Null State)
    0x65, 0x00,        //   Unit (None)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x46, 0xFF, 0x00,  //   Physical Maximum (255)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x09, 0x32,        //   Usage (Z)
    0x09, 0x35,        //   Usage (Rz)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x20,        //   Usage (0x20)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x0A, 0x21, 0x26,  //   Usage (0x2621)
    0x95, 0x08,        //   Report Count (8)
    0x91, 0x02,        //   Output (Data, Var, Abs)
    0xC0,              // End Collection
};

typedef struct __attribute__((packed)) {
    usb_desc_config_t   config;
    usb_desc_itf_t      itf;
    usb_desc_hid_t      hid;
    usb_desc_endpoint_t ep_out;
    usb_desc_endpoint_t ep_in;
} desc_config_t;

static const desc_config_t DescConfig = {
    .config = {
        .bLength                = sizeof(usb_desc_config_t),
        .bDescriptorType        = USB_DTYPE_CONFIGURATION,
        .wTotalLength           = sizeof(desc_config_t),
        .bNumInterfaces         = 1,
        .bConfigurationValue    = 1,
        .iConfiguration         = 0,
        .bmAttributes           = USB_ATTR_RESERVED | USB_ATTR_SELF_POWERED,
        .bMaxPower              = 50,
    },
    .itf = {
        .bLength                = sizeof(usb_desc_itf_t),
        .bDescriptorType        = USB_DTYPE_INTERFACE,
        .bInterfaceNumber       = 0,
        .bAlternateSetting      = 0,
        .bNumEndpoints          = 2,
        .bInterfaceClass        = USB_CLASS_HID,
        .bInterfaceSubClass     = 0x00,
        .bInterfaceProtocol     = 0x00,
        .iInterface             = 0,
    },
    .hid = {
        .bLength                = sizeof(usb_desc_hid_t),
        .bDescriptorType        = USB_DTYPE_HID,
        .bcdHID                 = 0x0111,
        .bCountryCode           = 0x00,
        .bNumDescriptors        = 1,
        .bDescriptorType0       = USB_DTYPE_HID_REPORT,
        .wDescriptorLength0     = sizeof(DescReport),
    },
    .ep_out = {
        .bLength                = sizeof(usb_desc_endpoint_t),
        .bDescriptorType        = USB_DTYPE_ENDPOINT,
        .bEndpointAddress       = EPADDR_OUT,
        .bmAttributes           = USB_EP_TYPE_INTERRUPT,
        .wMaxPacketSize         = EP_SIZE,
        .bInterval              = 4,
    },
    .ep_in = {
        .bLength                = sizeof(usb_desc_endpoint_t),
        .bDescriptorType        = USB_DTYPE_ENDPOINT,
        .bEndpointAddress       = EPADDR_IN,
        .bmAttributes           = USB_EP_TYPE_INTERRUPT,
        .wMaxPacketSize         = EP_SIZE,
        .bInterval              = 4,
    },
};

static const usb_desc_string_t DescStr0 = USB_ARRAY_DESC(0x0409);
static const usb_desc_string_t DescStr1 = USB_STRING_DESC("OpenOX");
static const usb_desc_string_t DescStr2 = USB_STRING_DESC("OpenOX USB test gamepad");
static const usb_desc_string_t *DescStr[] = {&DescStr0, &DescStr1, &DescStr2};

typedef struct __attribute__((packed)) {
    uint16_t buttons;
    uint8_t  hat;
    uint8_t  x, y, z, rz;
    uint8_t  vendor;
} report_t;

static report_t Report;
static uint8_t IdleRate;
static uint8_t Protocol = 1;
static int64_t LastUs;
static uint32_t Reports;
static uint32_t OutPackets;

static void reset_state(void)
{
    IdleRate = 0;
    Protocol = 1;
    Report.buttons = 0;
    Report.hat = 0x08;
    Report.x = Report.y = Report.z = Report.rz = 0x80;
    Report.vendor = 0;
}

static void init_cb(usbd_handle_t *handle)
{
    (void)handle;
    reset_state();
}

static void deinit_cb(usbd_handle_t *handle)
{
    (void)handle;
}

static bool set_config_cb(usbd_handle_t *handle, uint8_t config)
{
    (void)config;
    return usbd_configure_all_eps(handle, &DescConfig);
}

static void configured_cb(usbd_handle_t *handle, uint8_t config)
{
    (void)handle;
    (void)config;
}

static bool get_desc_cb(usbd_handle_t *handle, const usb_ctrl_req_t *req)
{
    switch (USB_DESC_TYPE(req->wValue)) {
    case USB_DTYPE_DEVICE:
        return usbd_send_ctrl_resp(handle, &DescDevice, DescDevice.bLength, NULL);
    case USB_DTYPE_CONFIGURATION:
        return usbd_send_ctrl_resp(handle, &DescConfig, sizeof(DescConfig), NULL);
    case USB_DTYPE_STRING: {
        const uint8_t idx = req->wValue & 0xFF;
        if (idx < ARRAY_SIZE(DescStr)) {
            return usbd_send_ctrl_resp(handle, DescStr[idx], DescStr[idx]->bLength, NULL);
        }
        if (idx == DescDevice.iSerialNumber) {
            const usb_desc_string_t *serial = usbd_get_desc_string_serial(handle);
            return usbd_send_ctrl_resp(handle, serial, serial->bLength, NULL);
        }
        return false;
    }
    case USB_DTYPE_HID_REPORT:
        return usbd_send_ctrl_resp(handle, DescReport, sizeof(DescReport), NULL);
    case USB_DTYPE_HID:
        return usbd_send_ctrl_resp(handle, &DescConfig.hid, DescConfig.hid.bLength, NULL);
    default:
        return false;
    }
}

static bool ctrl_xfer_cb(usbd_handle_t *handle, const usb_ctrl_req_t *req)
{
    if ((req->bmRequestType & (USB_REQ_TYPE_Msk | USB_REQ_RECIP_Msk)) !=
        (USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE)) {
        return false;
    }
    switch (req->bRequest) {
    case USB_REQ_HID_SET_REPORT:
        return true;
    case USB_REQ_HID_GET_REPORT:
        return usbd_send_ctrl_resp(handle, &Report, sizeof(Report), NULL);
    case USB_REQ_HID_SET_IDLE:
        IdleRate = (req->wValue >> 8) & 0xFF;
        return true;
    case USB_REQ_HID_GET_IDLE:
        return usbd_send_ctrl_resp(handle, &IdleRate, sizeof(IdleRate), NULL);
    case USB_REQ_HID_SET_PROTOCOL:
        if (req->wValue <= 1) {
            Protocol = (uint8_t)req->wValue;
            return true;
        }
        return false;
    case USB_REQ_HID_GET_PROTOCOL:
        return usbd_send_ctrl_resp(handle, &Protocol, sizeof(Protocol), NULL);
    default:
        return false;
    }
}

static void ep_xfer_cb(usbd_handle_t *handle, uint8_t epaddr)
{
    if (USB_EP_DIR(epaddr) == USB_EP_DIR_IN) {
        Reports++;
    } else {
        uint8_t buf[EP_SIZE];
        if (usbd_ep_read(handle, epaddr, buf, sizeof(buf)) >= 0) {
            OutPackets++;
        }
    }
}

usbd_handle_t *hid_test_init(void)
{
    const usbd_driver_t driver = {
        .init_cb        = init_cb,
        .deinit_cb      = deinit_cb,
        .get_desc_cb    = get_desc_cb,
        .set_config_cb  = set_config_cb,
        .configured_cb  = configured_cb,
        .ctrl_xfer_cb   = ctrl_xfer_cb,
        .ep_xfer_cb     = ep_xfer_cb,
    };
    Reports = 0;
    OutPackets = 0;
    LastUs = esp_timer_get_time();
    return usbd_init(USBD_HW_FPGA, &driver, EP0_SIZE);
}

void hid_test_task(usbd_handle_t *handle)
{
    if (handle == NULL || handle->state != USBD_STATE_CONFIGURED) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - LastUs >= TOGGLE_US && usbd_ep_ready(handle, EPADDR_IN)) {
        LastUs = now;
        Report.buttons ^= 0x0001;
        usbd_ep_write(handle, EPADDR_IN, &Report, sizeof(Report));
    }
}

uint32_t hid_test_reports(void)
{
    return Reports;
}

uint32_t hid_test_out_packets(void)
{
    return OutPackets;
}
