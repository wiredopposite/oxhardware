// A full-speed hub with per-port power switching. Port state follows the host's
// SET/CLEAR_FEATURE requests; a port reset resets the device behind it at once, and
// changes go to the host on the status change endpoint.
#include "hub.h"

#include <string.h>

#include "class/hub_def.h"
#include "dcd_fpga.h"
#include "usb_def.h"
#include "usb_util.h"

#define EPADDR_STATUS   (1U | USB_EP_DIR_IN)

#define BIT(f)          (1UL << (f))
#define PORT_STATUS_MSK (BIT(USB_FEATURE_PORT_CONNECTION) | BIT(USB_FEATURE_PORT_ENABLE) | \
                         BIT(USB_FEATURE_PORT_SUSPEND) | BIT(USB_FEATURE_PORT_RESET))

static usb_desc_device_t DescDevice = {
    .bLength            = sizeof(usb_desc_device_t),
    .bDescriptorType    = USB_DTYPE_DEVICE,
    .bcdUSB             = USB_BCD_VERSION_1_1,
    .bDeviceClass       = USB_CLASS_HUB,
    .bDeviceSubClass    = USB_SUBCLASS_HUB,
    .bDeviceProtocol    = USB_PROTOCOL_HUB_FULL_SPEED,
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x1209,   // pid.codes test VID/PID
    .idProduct          = 0x0002,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

typedef struct __attribute__((packed)) {
    usb_desc_config_t   config;
    usb_desc_itf_t      itf;
    usb_desc_endpoint_t ep;
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
        .bNumEndpoints          = 1,
        .bInterfaceClass        = USB_CLASS_HUB,
        .bInterfaceSubClass     = 0x00,
        .bInterfaceProtocol     = 0x00,
        .iInterface             = 0,
    },
    .ep = {
        .bLength                = sizeof(usb_desc_endpoint_t),
        .bDescriptorType        = USB_DTYPE_ENDPOINT,
        .bEndpointAddress       = EPADDR_STATUS,
        .bmAttributes           = USB_EP_TYPE_INTERRUPT,
        .wMaxPacketSize         = 1,
        .bInterval              = 12,
    },
};

// The USB 1.1 layout for up to 7 ports: DeviceRemovable, then PortPwrCtrlMask (all ones)
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bNumberOfPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPowerOnToPowerGood;
    uint8_t  bHubControlCurrent;
    uint8_t  DeviceRemovable;
    uint8_t  PortPwrCtrlMask;
} desc_hub_t;

static const usb_desc_string_t DescStr0 = USB_ARRAY_DESC(0x0409);
static const usb_desc_string_t DescStr1 = USB_STRING_DESC("OpenOX");
static const usb_desc_string_t DescStr2 = USB_STRING_DESC("OpenOX USB hub");
static const usb_desc_string_t *DescStr[] = {&DescStr0, &DescStr1, &DescStr2};

typedef struct {
    usbd_handle_t *dev;     // NULL: nothing attached
    bool powered;
    uint32_t status;        // wPortStatus | wPortChange << 16
} port_t;

static uint8_t Ports;
static port_t Port[HUB_PORTS_MAX + 1];      // [0] unused, ports count from 1
static uint8_t Changed;                     // bit n: port n has changes not yet sent

static void set_change(uint8_t p, uint8_t c_feature)
{
    Port[p].status |= BIT(c_feature);
    Changed |= (uint8_t)(1U << p);
}

static void reset_ports(void)
{
    for (uint8_t p = 1; p <= Ports; p++) {
        Port[p].powered = false;
        Port[p].status = 0;
    }
    Changed = 0;
}

static void power(uint8_t p, bool on)
{
    port_t *port = &Port[p];
    if (on == port->powered) {
        return;
    }
    port->powered = on;
    if (on) {
        if (port->dev != NULL) {
            port->status |= BIT(USB_FEATURE_PORT_CONNECTION);
            set_change(p, USB_FEATURE_C_PORT_CONNECTION);
        }
    } else {
        if (port->status & BIT(USB_FEATURE_PORT_CONNECTION)) {
            set_change(p, USB_FEATURE_C_PORT_CONNECTION);
        }
        port->status &= ~PORT_STATUS_MSK;
    }
}

static bool port_feature(const usb_ctrl_req_t *req)
{
    const uint8_t p = req->wIndex & 0xFF;
    const uint16_t feature = req->wValue;
    if (p == 0 || p > Ports) {
        return false;
    }
    port_t *port = &Port[p];
    const bool connected = port->status & BIT(USB_FEATURE_PORT_CONNECTION);
    if (req->bRequest == USB_REQ_STD_SET_FEATURE) {
        switch (feature) {
        case USB_FEATURE_PORT_POWER:
            power(p, true);
            return true;
        case USB_FEATURE_PORT_RESET:
            if (connected) {
                // Done at once: the device answers at address 0, the port is enabled
                usbd_reset_device(port->dev);
                port->status &= ~BIT(USB_FEATURE_PORT_SUSPEND);
                port->status |= BIT(USB_FEATURE_PORT_ENABLE);
                set_change(p, USB_FEATURE_C_PORT_RESET);
            }
            return true;
        case USB_FEATURE_PORT_SUSPEND:
            if (port->status & BIT(USB_FEATURE_PORT_ENABLE)) {
                port->status |= BIT(USB_FEATURE_PORT_SUSPEND);
            }
            return true;
        case USB_FEATURE_PORT_ENABLE:
            return true;
        default:
            return feature < 32;
        }
    }
    switch (feature) {
    case USB_FEATURE_PORT_POWER:
        power(p, false);
        return true;
    case USB_FEATURE_PORT_ENABLE:
        port->status &= ~(BIT(USB_FEATURE_PORT_ENABLE) | BIT(USB_FEATURE_PORT_SUSPEND));
        return true;
    case USB_FEATURE_PORT_SUSPEND:
        if (port->status & BIT(USB_FEATURE_PORT_SUSPEND)) {
            port->status &= ~BIT(USB_FEATURE_PORT_SUSPEND);
            set_change(p, USB_FEATURE_C_PORT_SUSPEND);
        }
        return true;
    case USB_FEATURE_C_PORT_CONNECTION:
    case USB_FEATURE_C_PORT_ENABLE:
    case USB_FEATURE_C_PORT_SUSPEND:
    case USB_FEATURE_C_PORT_OVERCURRENT:
    case USB_FEATURE_C_PORT_RESET:
        port->status &= ~BIT(feature);
        return true;
    default:
        return feature < 32;
    }
}

static void init_cb(usbd_handle_t *handle)
{
    (void)handle;
    reset_ports();
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
    case USB_DTYPE_HUB: {
        const desc_hub_t desc = {
            .bLength                = sizeof(desc_hub_t),
            .bDescriptorType        = USB_DTYPE_HUB,
            .bNumberOfPorts         = Ports,
            .wHubCharacteristics    = USB_HUB_CHAR_POWER_SWITCH_INDIVIDUAL |
                                      USB_HUB_CHAR_THINKTIME_8_FS_BITS,
            .bPowerOnToPowerGood    = 1,        // 2 ms units
            .bHubControlCurrent     = 0,
            .DeviceRemovable        = 0,
            .PortPwrCtrlMask        = 0xFF,
        };
        return usbd_send_ctrl_resp(handle, &desc, sizeof(desc), NULL);
    }
    default:
        return false;
    }
}

static bool ctrl_xfer_cb(usbd_handle_t *handle, const usb_ctrl_req_t *req)
{
    const uint8_t type = req->bmRequestType & USB_REQ_TYPE_Msk;
    const uint8_t recip = req->bmRequestType & USB_REQ_RECIP_Msk;
    if (type == USB_REQ_TYPE_STANDARD) {
        if (recip == USB_REQ_RECIP_DEVICE && req->bRequest == USB_REQ_STD_GET_STATUS) {
            const uint16_t status = 1;      // self-powered
            return usbd_send_ctrl_resp(handle, &status, sizeof(status), NULL);
        }
        return false;
    }
    if (type != USB_REQ_TYPE_CLASS) {
        return false;
    }
    switch (req->bRequest) {
    case USB_REQ_STD_GET_STATUS:
        if (recip == USB_REQ_RECIP_DEVICE) {
            const uint32_t status = 0;      // local power good, no over-current
            return usbd_send_ctrl_resp(handle, &status, sizeof(status), NULL);
        }
        if (recip == USB_REQ_RECIP_OTHER) {
            const uint8_t p = req->wIndex & 0xFF;
            if (p == 0 || p > Ports) {
                return false;
            }
            uint32_t status = Port[p].status;
            if (Port[p].powered) {
                status |= BIT(USB_FEATURE_PORT_POWER);
            }
            return usbd_send_ctrl_resp(handle, &status, sizeof(status), NULL);
        }
        return false;
    case USB_REQ_STD_SET_FEATURE:
    case USB_REQ_STD_CLEAR_FEATURE:
        if (recip == USB_REQ_RECIP_DEVICE) {
            return true;    // C_HUB_LOCAL_POWER, C_HUB_OVER_CURRENT: never set
        }
        if (recip == USB_REQ_RECIP_OTHER) {
            return port_feature(req);
        }
        return false;
    default:
        return false;
    }
}

static void ep_xfer_cb(usbd_handle_t *handle, uint8_t epaddr)
{
    (void)handle;
    (void)epaddr;
}

usbd_handle_t *hub_init(uint8_t ports, uint8_t ep0_size)
{
    if (ports == 0 || ports > HUB_PORTS_MAX || (ep0_size != 8 && ep0_size != 64)) {
        return NULL;
    }
    const usbd_driver_t driver = {
        .init_cb        = init_cb,
        .deinit_cb      = deinit_cb,
        .get_desc_cb    = get_desc_cb,
        .set_config_cb  = set_config_cb,
        .configured_cb  = configured_cb,
        .ctrl_xfer_cb   = ctrl_xfer_cb,
        .ep_xfer_cb     = ep_xfer_cb,
    };
    memset(Port, 0, sizeof(Port));
    DescDevice.bMaxPacketSize0 = ep0_size;
    Ports = ports;
    Changed = 0;
    usbd_handle_t *handle = usbd_init(USBD_HW_FPGA, &driver, ep0_size);
    if (handle != NULL && handle->port != 0) {
        usbd_deinit(USBD_HW_FPGA);      // a hub must be device 0
        return NULL;
    }
    return handle;
}

bool hub_attach(uint8_t port, usbd_handle_t *dev)
{
    if (port == 0 || port > Ports || dev == NULL || Port[port].dev != NULL) {
        return false;
    }
    Port[port].dev = dev;
    if (Port[port].powered) {
        Port[port].status |= BIT(USB_FEATURE_PORT_CONNECTION);
        set_change(port, USB_FEATURE_C_PORT_CONNECTION);
    }
    return true;
}

usbd_handle_t *hub_detach(uint8_t port)
{
    if (port == 0 || port > Ports || Port[port].dev == NULL) {
        return NULL;
    }
    usbd_handle_t *dev = Port[port].dev;
    Port[port].dev = NULL;
    if (Port[port].status & BIT(USB_FEATURE_PORT_CONNECTION)) {
        Port[port].status &= ~PORT_STATUS_MSK;
        set_change(port, USB_FEATURE_C_PORT_CONNECTION);
    }
    // Off the bus now; usbd sets it up again when its port is next reset
    dcd_fpga_detach(dev->port);
    if (dev->state == USBD_STATE_CONFIGURED) {
        dev->app_driver.deinit_cb(dev);
    }
    dev->state = USBD_STATE_DEFAULT;
    dev->config_num = 0;
    return dev;
}

void hub_task(usbd_handle_t *handle)
{
    if (handle == NULL || handle->state != USBD_STATE_CONFIGURED || Changed == 0 ||
        !usbd_ep_ready(handle, EPADDR_STATUS)) {
        return;
    }
    const uint8_t bitmap = Changed;
    if (usbd_ep_write(handle, EPADDR_STATUS, &bitmap, sizeof(bitmap)) >= 0) {
        Changed = 0;
    }
}
