// Mailbox transport (docs/01-interface.md §5.6): moves bytes between the FPGA's console
// port FIFOs and the protocol server.

#include <cstring>

#include "esp_log.h"
#include "fpga_link.hpp"
#include "fpga_svc.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "prog_server.hpp"

namespace ProgServer {

namespace {

const char *Tag = "mailbox";

using FpgaLink::Space;

constexpr uint32_t RegCtrl = 0x0100;           // MBX_CTRL .. MBX_TX_FREE, 7 bytes
constexpr uint32_t FifoPort = 0x01;            // SPACE 3
constexpr uint8_t CtrlEnable = 0x01;
constexpr uint8_t CtrlResetReq = 0x02;
constexpr uint8_t CtrlReady = 0x04;
constexpr TickType_t SendTimeout = pdMS_TO_TICKS(2000);
// MBX_RX wakes the task; this is only the fallback if no interrupt comes
constexpr TickType_t IdleCheck = pdMS_TO_TICKS(100);

Parser In;
Packet Reply;
uint8_t Rx[1024];

struct State {
    uint8_t Ctrl;
    uint16_t RxLevel;
    uint16_t TxFree;
};

bool ReadState(State &s)
{
    uint8_t r[7];
    if (!FpgaLink::Read(Space::Regs, RegCtrl, r, sizeof(r))) {
        return false;
    }
    s.Ctrl = r[0];
    s.RxLevel = uint16_t(r[3] | r[4] << 8);
    s.TxFree = uint16_t(r[5] | r[6] << 8);
    return true;
}

void SetCtrl(uint8_t v)
{
    FpgaLink::Write(Space::Regs, RegCtrl, &v, 1);
}

// Clears RESET_REQ (the FPGA drops both FIFOs again) and starts over
void Drain()
{
    In.Reset();
    SetCtrl(CtrlEnable | CtrlReady | CtrlResetReq);
}

// Sends the reply as the console makes room. False if the console reset or stopped
// reading.
bool Send()
{
    size_t done = 0;
    TickType_t last = xTaskGetTickCount();
    while (done < Reply.Size) {
        State s;
        if (!ReadState(s) || (s.Ctrl & CtrlResetReq)) {
            return false;
        }
        size_t n = Reply.Size - done;
        if (n > s.TxFree) {
            n = s.TxFree;
        }
        if (n) {
            FpgaLink::Write(Space::Fifos, FifoPort, Reply.Data + done, n, nullptr, true);
            done += n;
            last = xTaskGetTickCount();
        }
        else if (xTaskGetTickCount() - last > SendTimeout) {
            ESP_LOGW(Tag, "console stopped reading, reply dropped");
            return false;
        }
        else {
            vTaskDelay(1);
        }
    }
    return true;
}

void Task(void *)
{
    Drain();
    ESP_LOGI(Tag, "console port up");
    for (;;) {
        State s;
        if (!ReadState(s)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (s.Ctrl & CtrlResetReq) {
            Drain();
            continue;
        }
        if (!s.RxLevel) {
            ulTaskNotifyTake(pdTRUE, IdleCheck);    // MBX_RX: data into an empty FIFO
            continue;
        }
        const size_t n = s.RxLevel < sizeof(Rx) ? s.RxLevel : sizeof(Rx);
        if (!FpgaLink::Read(Space::Fifos, FifoPort, Rx, n, nullptr, true)) {
            continue;
        }
        size_t off = 0;
        while (off < n) {
            off += In.Feed(Rx + off, n - off);
            if (In.Complete()) {
                Handle(In, Reply);
                In.Reset();
                if (!Send()) {
                    break;                      // the next pass sees RESET_REQ
                }
            }
        }
    }
}

}

void StartMailbox()
{
    TaskHandle_t task;
    xTaskCreate(Task, "mailbox", 4096, nullptr, 5, &task);
    FpgaSvc::Notify(FpgaSvc::Irq::MbxRx, task);
}

}
