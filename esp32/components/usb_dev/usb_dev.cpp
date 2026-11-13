#include "usb_dev.hpp"

#include "dcd_fpga.h"
#include "fpga_link.hpp"
#include "fpga_svc.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_test.h"
#include "host_link.hpp"
#include "hub.h"

namespace UsbDev {

namespace {

using FpgaLink::Space;

constexpr uint32_t RegFeatures = 0x0008;
constexpr uint16_t FeatureUsb = 1u << 4;

// Commands (oxctl usb)
constexpr uint16_t CmdStart = 0x0700;      // u8 profile
constexpr uint16_t CmdStop = 0x0701;
constexpr uint16_t CmdStatus = 0x0702;
constexpr uint16_t CmdPlug = 0x0703;       // u8 plugged: the test gamepad on hub port 1

// Without an interrupt the task still looks this often: the functions have timed work
constexpr TickType_t IdleTicks = pdMS_TO_TICKS(2);
// Polls per wakeup at most, while events keep coming
constexpr int Burst = 16;
constexpr uint8_t HubPorts = 4;

TaskHandle_t Task;
volatile bool StartReq, StopReq, Running;
volatile int8_t PlugReq = -1;       // -1 none, 0 unplug, 1 plug
constexpr uint8_t PadPort = 1;
Profile Want;
usbd_handle_t *Hub, *Pad;

// Device 0 first: with a hub, everything else sits behind it
bool Start(Profile profile)
{
    if (profile == Profile::HubTest) {
        Hub = hub_init(HubPorts);
        if (Hub == nullptr) {
            return false;
        }
    }
    Pad = hid_test_init();
    if (Pad == nullptr || (Hub != nullptr && !hub_attach(PadPort, Pad))) {
        usbd_deinit(USBD_HW_FPGA);
        Hub = Pad = nullptr;
        return false;
    }
    usbd_connect(USBD_HW_FPGA);
    return true;
}

void Run(void *)
{
    FpgaSvc::Notify(FpgaSvc::Irq::Usb, xTaskGetCurrentTaskHandle());
    for (;;) {
        ulTaskNotifyTake(pdTRUE, Running ? IdleTicks : portMAX_DELAY);
        if (StopReq) {
            StopReq = false;
            if (Running) {
                usbd_deinit(USBD_HW_FPGA);      // detaches: the pull-up goes off
                Running = false;
                Hub = Pad = nullptr;
            }
        }
        if (StartReq) {
            StartReq = false;
            if (!Running) {
                Running = Start(Want);
            }
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
            hid_test_task(Pad);
            if (!ev && !dcd_fpga_pending()) {
                break;
            }
        }
    }
}

bool HasUsb()
{
    uint8_t f[2];
    return FpgaLink::Read(Space::Regs, RegFeatures, f, sizeof(f)) && f[0] != 0xFF &&
           (f[0] | f[1] << 8) & FeatureUsb;
}

void Wake()
{
    if (Task == nullptr) {
        xTaskCreate(Run, "usb_dev", 4096, nullptr, 9, &Task);
    }
    xTaskNotifyGive(Task);
}

}

void RegisterCommands()
{
    // u8 profile
    HostLink::Register(CmdStart, [](HostLink::Args &args, HostLink::Reply &reply) {
        const uint8_t profile = args.U8();
        if (args.Bad() || profile > uint8_t(Profile::HubTest)) {
            reply.Fail(HostLink::Error::BadArgs);
            return;
        }
        if (Running) {
            reply.Fail(HostLink::Error::Busy, "already running");
            return;
        }
        if (!HasUsb()) {
            reply.Fail(HostLink::Error::Failed, "FPGA design has no USB");
            return;
        }
        Want = Profile(profile);
        StartReq = true;
        Wake();
        for (int i = 0; i < 50 && !Running; i++) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (!Running) {
            reply.Fail(HostLink::Error::Failed, "init failed");
        }
    });
    HostLink::Register(CmdStop, [](HostLink::Args &, HostLink::Reply &) {
        if (Task != nullptr) {
            StopReq = true;
            xTaskNotifyGive(Task);
            for (int i = 0; i < 50 && Running; i++) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
    });
    // u8 plugged; hub profile only
    HostLink::Register(CmdPlug, [](HostLink::Args &args, HostLink::Reply &reply) {
        const uint8_t plugged = args.U8();
        if (args.Bad() || plugged > 1) {
            reply.Fail(HostLink::Error::BadArgs);
            return;
        }
        if (!Running || Hub == nullptr) {
            reply.Fail(HostLink::Error::Failed, "hub profile not running");
            return;
        }
        PlugReq = int8_t(plugged);
        xTaskNotifyGive(Task);
        for (int i = 0; i < 50 && PlugReq >= 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    });
    // -> u8 running, u8 gamepad state, u32 reports, u32 out packets, u32 x7 driver counts,
    //    u8 x8 device addresses, u8 hub state (0xFF: no hub)
    HostLink::Register(CmdStatus, [](HostLink::Args &, HostLink::Reply &reply) {
        dcd_fpga_stats_t s;
        dcd_fpga_get_stats(&s);
        usbd_handle_t *dev = Pad;
        reply.PutU8(Running ? 1 : 0);
        reply.PutU8(dev != nullptr ? uint8_t(dev->state) : 0);
        reply.PutU32(hid_test_reports());
        reply.PutU32(hid_test_out_packets());
        reply.PutU32(s.polls);
        reply.PutU32(s.bus_resets);
        reply.PutU32(s.setups);
        reply.PutU32(s.in_done);
        reply.PutU32(s.out_done);
        reply.PutU32(s.suspends);
        reply.PutU32(s.link_errors);
        reply.Put(s.addr, sizeof(s.addr));
        usbd_handle_t *hub = Hub;
        reply.PutU8(hub != nullptr ? uint8_t(hub->state) : 0xFF);
    });
}

}
