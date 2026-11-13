#include "usb_dev.hpp"

#include "dcd_fpga.h"
#include "fpga_link.hpp"
#include "fpga_svc.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_test.h"
#include "host_link.hpp"

namespace UsbDev {

namespace {

using FpgaLink::Space;

constexpr uint32_t RegFeatures = 0x0008;
constexpr uint16_t FeatureUsb = 1u << 4;

// Commands (oxctl usb)
constexpr uint16_t CmdStart = 0x0700;      // u8 profile
constexpr uint16_t CmdStop = 0x0701;
constexpr uint16_t CmdStatus = 0x0702;

// Without an interrupt the task still looks this often: the functions have timed work
constexpr TickType_t IdleTicks = pdMS_TO_TICKS(2);
// Polls per wakeup at most, while events keep coming
constexpr int Burst = 16;

TaskHandle_t Task;
volatile bool StartReq, StopReq, Running;
usbd_handle_t *Dev;

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
                Dev = nullptr;
            }
        }
        if (StartReq) {
            StartReq = false;
            if (!Running) {
                Dev = hid_test_init();
                if (Dev != nullptr) {
                    usbd_connect(USBD_HW_FPGA);
                    Running = true;
                }
            }
        }
        if (!Running) {
            continue;
        }
        for (int i = 0; i < Burst; i++) {
            const bool ev = dcd_fpga_poll();
            usbd_task();
            hid_test_task(Dev);
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
        if (args.Bad() || profile != uint8_t(Profile::HidTest)) {
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
    // -> u8 running, u8 device state, u32 reports, u32 out packets, u32 x7 driver counts,
    //    u8 x8 device addresses
    HostLink::Register(CmdStatus, [](HostLink::Args &, HostLink::Reply &reply) {
        dcd_fpga_stats_t s;
        dcd_fpga_get_stats(&s);
        usbd_handle_t *dev = Dev;
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
    });
}

}
