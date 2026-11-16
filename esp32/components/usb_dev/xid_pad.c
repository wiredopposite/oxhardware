// An Xbox gamepad: one XID interface with a 32-byte interrupt IN (input reports) and OUT
// (rumble) endpoint. Besides the standard requests the console asks for the XID
// descriptor (vendor GET_DESCRIPTOR, type 0x42), the capabilities (vendor request 1:
// which report bits exist), GET_REPORT for the current input, and SET_REPORT for rumble.
#include "xid_pad.h"

#include <string.h>

#include "class/hid_def.h"
#include "freertos/FreeRTOS.h"
#include "usb_def.h"
#include "usb_util.h"

#define EP0_SIZE        8U
#define EPADDR_IN       (1U | USB_EP_DIR_IN)
#define EPADDR_OUT      (2U | USB_EP_DIR_OUT)
#define EP_SIZE         32U

#define DTYPE_XID       0x42U
#define REQ_GET_CAPS    0x01U
#define REPORT_IN       0x01U
#define REPORT_OUT      0x02U

typedef struct __attribute__((packed)) {
    uint8_t  id;
    uint8_t  len;
    xid_in_t in;
} report_in_t;

typedef struct __attribute__((packed)) {
    uint8_t   id;
    uint8_t   len;
    xid_out_t out;
} report_out_t;

_Static_assert(sizeof(report_in_t) == 20, "XID input report is 20 bytes");
_Static_assert(sizeof(report_out_t) == 6, "XID output report is 6 bytes");

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdXid;
    uint8_t  bType;
    uint8_t  bSubType;
    uint8_t  bMaxInputReportSize;
    uint8_t  bMaxOutputReportSize;
    uint16_t wAlternateProductIds[4];
} desc_xid_t;

static const usb_desc_device_t DescDevice = {
    .bLength            = sizeof(usb_desc_device_t),
    .bDescriptorType    = USB_DTYPE_DEVICE,
    .bcdUSB             = USB_BCD_VERSION_1_1,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = EP0_SIZE,
    .idVendor           = 0x1209,   // pid.codes test VID/PID
    .idProduct          = 0x0003,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0,
    .iProduct           = 0,
    .iSerialNumber      = 0,
    .bNumConfigurations = 1,
};

typedef struct __attribute__((packed)) {
    usb_desc_config_t   config;
    usb_desc_itf_t      itf;
    usb_desc_endpoint_t ep_in;
    usb_desc_endpoint_t ep_out;
} desc_config_t;

static const desc_config_t DescConfig = {
    .config = {
        .bLength                = sizeof(usb_desc_config_t),
        .bDescriptorType        = USB_DTYPE_CONFIGURATION,
        .wTotalLength           = sizeof(desc_config_t),
        .bNumInterfaces         = 1,
        .bConfigurationValue    = 1,
        .iConfiguration         = 0,
        .bmAttributes           = USB_ATTR_RESERVED,
        .bMaxPower              = 50,
    },
    .itf = {
        .bLength                = sizeof(usb_desc_itf_t),
        .bDescriptorType        = USB_DTYPE_INTERFACE,
        .bInterfaceNumber       = 0,
        .bAlternateSetting      = 0,
        .bNumEndpoints          = 2,
        .bInterfaceClass        = 0x58,
        .bInterfaceSubClass     = 0x42,
        .bInterfaceProtocol     = 0x00,
        .iInterface             = 0,
    },
    .ep_in = {
        .bLength                = sizeof(usb_desc_endpoint_t),
        .bDescriptorType        = USB_DTYPE_ENDPOINT,
        .bEndpointAddress       = EPADDR_IN,
        .bmAttributes           = USB_EP_TYPE_INTERRUPT,
        .wMaxPacketSize         = EP_SIZE,
        .bInterval              = 4,
    },
    .ep_out = {
        .bLength                = sizeof(usb_desc_endpoint_t),
        .bDescriptorType        = USB_DTYPE_ENDPOINT,
        .bEndpointAddress       = EPADDR_OUT,
        .bmAttributes           = USB_EP_TYPE_INTERRUPT,
        .wMaxPacketSize         = EP_SIZE,
        .bInterval              = 4,
    },
};

static const desc_xid_t DescXid = {
    .bLength                = sizeof(desc_xid_t),
    .bDescriptorType        = DTYPE_XID,
    .bcdXid                 = 0x0100,
    .bType                  = 0x01,     // gamepad
    .bSubType               = 0x02,     // controller S
    .bMaxInputReportSize    = sizeof(report_in_t),
    .bMaxOutputReportSize   = sizeof(report_out_t),
    .wAlternateProductIds   = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
};

static portMUX_TYPE Mux = portMUX_INITIALIZER_UNLOCKED;
static xid_in_t State;
static bool Changed;
static xid_out_t Rumble;
static uint32_t Reports;
static uint32_t Rumbles;
static volatile xid_pad_rumble_cb_t RumbleCb;

static void take_state(report_in_t *r)
{
    r->id = 0;
    r->len = sizeof(report_in_t);
    taskENTER_CRITICAL(&Mux);
    r->in = State;
    Changed = false;
    taskEXIT_CRITICAL(&Mux);
}

static void got_rumble(const uint8_t *data, int32_t len)
{
    if (len < (int32_t)sizeof(report_out_t) || data[1] < sizeof(report_out_t)) {
        return;
    }
    xid_out_t out;
    memcpy(&out, data + 2, sizeof(out));
    taskENTER_CRITICAL(&Mux);
    Rumble = out;
    taskEXIT_CRITICAL(&Mux);
    Rumbles++;
    const xid_pad_rumble_cb_t cb = RumbleCb;
    if (cb != NULL) {
        cb(&out);
    }
}

static void init_cb(usbd_handle_t *handle)
{
    (void)handle;
    taskENTER_CRITICAL(&Mux);
    Changed = true;             // the first report goes out once configured
    taskEXIT_CRITICAL(&Mux);
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
    case USB_DTYPE_STRING:
        if ((req->wValue & 0xFF) == 0) {
            static const usb_desc_string_t lang = USB_ARRAY_DESC(0x0409);
            return usbd_send_ctrl_resp(handle, &lang, lang.bLength, NULL);
        }
        return false;
    case DTYPE_XID:
        return usbd_send_ctrl_resp(handle, &DescXid, sizeof(DescXid), NULL);
    default:
        return false;
    }
}

static bool ctrl_xfer_cb(usbd_handle_t *handle, const usb_ctrl_req_t *req)
{
    const uint8_t kind = req->bmRequestType & (USB_REQ_TYPE_Msk | USB_REQ_RECIP_Msk);
    if (kind == (USB_REQ_TYPE_VENDOR | USB_REQ_RECIP_INTERFACE) &&
        req->bRequest == REQ_GET_CAPS) {
        // Every bit the reports have is implemented: all ones after the header
        uint8_t caps[sizeof(report_in_t)];
        memset(caps, 0xFF, sizeof(caps));
        caps[0] = 0;
        switch (req->wValue >> 8) {
        case REPORT_IN:
            caps[1] = sizeof(report_in_t);
            return usbd_send_ctrl_resp(handle, caps, sizeof(report_in_t), NULL);
        case REPORT_OUT:
            caps[1] = sizeof(report_out_t);
            return usbd_send_ctrl_resp(handle, caps, sizeof(report_out_t), NULL);
        default:
            return false;
        }
    }
    if (kind != (USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE)) {
        return false;
    }
    switch (req->bRequest) {
    case USB_REQ_HID_GET_REPORT: {
        report_in_t r;
        take_state(&r);
        return usbd_send_ctrl_resp(handle, &r, sizeof(r), NULL);
    }
    case USB_REQ_HID_SET_REPORT:
        got_rumble(req->data, req->wLength);
        return true;
    default:
        return false;
    }
}

static void ep_xfer_cb(usbd_handle_t *handle, uint8_t epaddr)
{
    if (USB_EP_DIR(epaddr) == USB_EP_DIR_IN) {
        Reports++;
        return;
    }
    uint8_t buf[EP_SIZE];
    const int32_t n = usbd_ep_read(handle, epaddr, buf, sizeof(buf));
    got_rumble(buf, n);
}

usbd_handle_t *xid_pad_init(void)
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
    taskENTER_CRITICAL(&Mux);
    memset(&State, 0, sizeof(State));
    memset(&Rumble, 0, sizeof(Rumble));
    Changed = true;
    taskEXIT_CRITICAL(&Mux);
    Reports = 0;
    Rumbles = 0;
    return usbd_init(USBD_HW_FPGA, &driver, EP0_SIZE);
}

void xid_pad_task(usbd_handle_t *handle)
{
    if (handle == NULL || handle->state != USBD_STATE_CONFIGURED ||
        !usbd_ep_ready(handle, EPADDR_IN)) {
        return;
    }
    taskENTER_CRITICAL(&Mux);
    const bool send = Changed;
    taskEXIT_CRITICAL(&Mux);
    if (send) {
        report_in_t r;
        take_state(&r);
        usbd_ep_write(handle, EPADDR_IN, &r, sizeof(r));
    }
}

void xid_pad_set(const xid_in_t *state)
{
    taskENTER_CRITICAL(&Mux);
    if (memcmp(&State, state, sizeof(State)) != 0) {
        State = *state;
        Changed = true;
    }
    taskEXIT_CRITICAL(&Mux);
}

void xid_pad_rumble(xid_out_t *out)
{
    taskENTER_CRITICAL(&Mux);
    *out = Rumble;
    taskEXIT_CRITICAL(&Mux);
}

void xid_pad_on_rumble(xid_pad_rumble_cb_t cb)
{
    RumbleCb = cb;
}

uint32_t xid_pad_reports(void)
{
    return Reports;
}

uint32_t xid_pad_rumbles(void)
{
    return Rumbles;
}
