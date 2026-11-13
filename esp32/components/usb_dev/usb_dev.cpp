#include "usb_dev.hpp"

#include <cstring>

#include "cfg_bus.hpp"
#include "dcd_fpga.h"
#include "esp_log.h"
#include "fpga_link.hpp"
#include "fpga_svc.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_link.hpp"
#include "hub.h"
#include "xid_pad.h"

namespace UsbDev {

namespace {

using FpgaLink::Space;

const char *Tag = "usb";

constexpr uint32_t RegFeatures = 0x0008;
constexpr uint16_t FeatureUsb = 1u << 4;
constexpr uint32_t RegUsbCtrl = 0x0200;     // bit1 SIE_EN: 0 after the FPGA is reloaded
constexpr uint8_t UsbSieEn = 0x02;

// Commands (oxctl usb)
constexpr uint16_t CmdStart = 0x0700;
constexpr uint16_t CmdStop = 0x0701;
constexpr uint16_t CmdStatus = 0x0702;
constexpr uint16_t CmdPlug = 0x0703;       // u8 plugged: the gamepad on hub port 1
constexpr uint16_t CmdPad = 0x0704;        // xid_in_t: the gamepad's state

// Without an interrupt the task still looks this often: the functions have timed work
constexpr TickType_t IdleTicks = pdMS_TO_TICKS(2);
// Polls per wakeup at most, while events keep coming
constexpr int Burst = 16;
// Wanted but not running (the FPGA not ready yet): retried this often. Running: the
// FPGA's USB is checked as often, to start again after it was reloaded.
constexpr TickType_t RetryTicks = pdMS_TO_TICKS(1000);
// Before the FPGA is reset: unplugged this long first, so the console has seen the
// disconnect before the FPGA's pins float
constexpr TickType_t UnplugTicks = pdMS_TO_TICKS(200);
constexpr uint8_t HubPorts = 3;         // a controller: pad, two MU slots
constexpr uint8_t PadPort = 1;

TaskHandle_t Task;
volatile bool Wanted, Running;      // Wanted: from Start() until oxctl usb stop
volatile bool Paused;               // the FPGA is being reset
volatile int8_t PlugReq = -1;       // -1 none, 0 unplug, 1 plug
volatile bool PadPresent = true;    // the pad is plugged in at start
usbd_handle_t *Hub, *Pad;

bool HasUsb()
{
    uint8_t f[2];
    return FpgaLink::Read(Space::Regs, RegFeatures, f, sizeof(f)) && f[0] != 0xFF &&
           (f[0] | f[1] << 8) & FeatureUsb;
}

// The FPGA's USB still has what Attach() gave it
bool UsbAlive()
{
    uint8_t c;
    return FpgaLink::Read(Space::Regs, RegUsbCtrl, &c, 1) && c != 0xFF && (c & UsbSieEn);
}

// Device 0, the hub, first: the pad sits behind it
bool Attach()
{
    Hub = hub_init(HubPorts, 8);
    if (Hub == nullptr) {
        return false;
    }
    Pad = xid_pad_init();
    if (Pad == nullptr || (PadPresent && !hub_attach(PadPort, Pad))) {
        usbd_deinit(USBD_HW_FPGA);
        Hub = Pad = nullptr;
        return false;
    }
    usbd_connect(USBD_HW_FPGA);
    return true;
}

void Detach()
{
    usbd_deinit(USBD_HW_FPGA);      // the pull-up goes off: the console sees it unplugged
    Running = false;
    Hub = Pad = nullptr;
}

void Run(void *)
{
    FpgaSvc::Notify(FpgaSvc::Irq::Usb, xTaskGetCurrentTaskHandle());
    TickType_t checked = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, Running ? IdleTicks : Wanted ? RetryTicks : portMAX_DELAY);
        if (Running && (!Wanted || Paused)) {
            Detach();
        }
        if (Running && xTaskGetTickCount() - checked >= RetryTicks) {
            checked = xTaskGetTickCount();
            if (!UsbAlive()) {
                ESP_LOGW(Tag, "the FPGA's USB was reset: attaching again");
                Detach();
            }
        }
        if (Wanted && !Paused && !Running && HasUsb()) {
            Running = Attach();
            checked = xTaskGetTickCount();
            ESP_LOGI(Tag, "%s", Running ? "controller attached" : "attach failed");
        }
        if (!Running) {
            continue;
        }
        if (PlugReq >= 0) {
            if (PlugReq != 0) {
                hub_attach(PadPort, Pad);
            } else {
                hub_detach(PadPort);
            }
            PlugReq = -1;
        }
        for (int i = 0; i < Burst; i++) {
            const bool ev = dcd_fpga_poll();
            usbd_task();
            hub_task(Hub);
            xid_pad_task(Pad);
            if (!ev && !dcd_fpga_pending()) {
                break;
            }
        }
    }
}

RumbleHandler Rumble;

void PadRumble(const xid_out_t *out)
{
    const RumbleHandler h = Rumble;
    if (h != nullptr) {
        h(out->left, out->right);
    }
}

void BeforeFpgaReset()
{
    Paused = true;
    if (Task == nullptr || !Running) {
        return;
    }
    xTaskNotifyGive(Task);
    for (int i = 0; i < 50 && Running; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    vTaskDelay(UnplugTicks);
}

void AfterFpgaReset()
{
    Paused = false;
    if (Task != nullptr) {
        xTaskNotifyGive(Task);
    }
}

void Wake()
{
    if (Task == nullptr) {
        xTaskCreate(Run, "usb_dev", 4096, nullptr, 9, &Task);
    }
    xTaskNotifyGive(Task);
}

}

void Start()
{
    CfgBus::OnReset(BeforeFpgaReset, AfterFpgaReset);
    Wanted = true;
    Wake();
}

void SetPadState(const uint8_t state[18])
{
    xid_in_t in;
    static_assert(sizeof(in) == 18);
    memcpy(&in, state, sizeof(in));
    xid_pad_set(&in);
    if (Task != nullptr && Running) {
        xTaskNotifyGive(Task);
    }
}

void SetPadPresent(bool present)
{
    PadPresent = present;
    if (Task == nullptr || !Running) {
        return;
    }
    PlugReq = present ? 1 : 0;
    xTaskNotifyGive(Task);
}

void OnRumble(RumbleHandler handler)
{
    Rumble = handler;
    xid_pad_on_rumble(PadRumble);
}

void RegisterCommands()
{
    // After oxctl usb stop: attach again
    HostLink::Register(CmdStart, [](HostLink::Args &, HostLink::Reply &reply) {
        if (!HasUsb()) {
            reply.Fail(HostLink::Error::Failed, "FPGA design has no USB");
            return;
        }
        Start();
        for (int i = 0; i < 50 && !Running; i++) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (!Running) {
            reply.Fail(HostLink::Error::Failed, "attach failed");
        }
    });
    // Detached until oxctl usb start or the next boot
    HostLink::Register(CmdStop, [](HostLink::Args &, HostLink::Reply &) {
        Wanted = false;
        if (Task != nullptr) {
            xTaskNotifyGive(Task);
            for (int i = 0; i < 50 && Running; i++) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
    });
    // u8 plugged: the gamepad on hub port 1
    HostLink::Register(CmdPlug, [](HostLink::Args &args, HostLink::Reply &reply) {
        const uint8_t plugged = args.U8();
        if (args.Bad() || plugged > 1) {
            reply.Fail(HostLink::Error::BadArgs);
            return;
        }
        if (!Running) {
            reply.Fail(HostLink::Error::Failed, "USB not running");
            return;
        }
        PlugReq = int8_t(plugged);
        xTaskNotifyGive(Task);
        for (int i = 0; i < 50 && PlugReq >= 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    });
    // xid_in_t (18 bytes) -> xid_out_t (4 bytes, the last rumble), u32 rumble reports
    HostLink::Register(CmdPad, [](HostLink::Args &args, HostLink::Reply &reply) {
        xid_in_t in;
        const uint8_t *p = args.Bytes(sizeof(in));
        if (args.Bad()) {
            reply.Fail(HostLink::Error::BadArgs);
            return;
        }
        if (!Running) {
            reply.Fail(HostLink::Error::Failed, "USB not running");
            return;
        }
        memcpy(&in, p, sizeof(in));
        xid_pad_set(&in);
        xTaskNotifyGive(Task);
        xid_out_t out;
        xid_pad_rumble(&out);
        reply.Put(reinterpret_cast<const uint8_t *>(&out), sizeof(out));
        reply.PutU32(xid_pad_rumbles());
    });
    // -> u8 running, u8 gamepad state, u32 reports, u32 rumble reports, u32 x7 driver
    //    counts, u8 x8 device addresses, u8 hub state
    HostLink::Register(CmdStatus, [](HostLink::Args &, HostLink::Reply &reply) {
        dcd_fpga_stats_t s;
        dcd_fpga_get_stats(&s);
        usbd_handle_t *dev = Pad;
        reply.PutU8(Running ? 1 : 0);
        reply.PutU8(dev != nullptr ? uint8_t(dev->state) : 0);
        reply.PutU32(xid_pad_reports());
        reply.PutU32(xid_pad_rumbles());
        reply.PutU32(s.polls);
        reply.PutU32(s.bus_resets);
        reply.PutU32(s.setups);
        reply.PutU32(s.in_done);
        reply.PutU32(s.out_done);
        reply.PutU32(s.suspends);
        reply.PutU32(s.link_errors);
        reply.Put(s.addr, sizeof(s.addr));
        usbd_handle_t *hub = Hub;
        reply.PutU8(hub != nullptr ? uint8_t(hub->state) : 0);
    });
}

}
