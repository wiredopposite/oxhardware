#include "console.hpp"

#include "board_pins.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fpga_link.hpp"
#include "fpga_svc.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_link.hpp"

namespace Console {
namespace {
const char *Tag = "console";

using FpgaLink::Space;

constexpr uint16_t RegLpcStatus = 0x0021;   // bit0 LRESET#, bit1 LCLK running
constexpr uint8_t StLreset = 0x01;
constexpr uint8_t StLclk = 0x02;

constexpr uint16_t CmdState = 0x0410;
constexpr uint16_t CmdPress = 0x0411;

constexpr uint32_t PressMs = 200;
constexpr int64_t WakeHoldoffUs = 15 * 1000 * 1000;
// Without an interrupt the task still looks this often (a missed edge, the FPGA reloaded)
constexpr TickType_t CheckTicks = pdMS_TO_TICKS(1000);

constexpr TickType_t ShutdownTicks = pdMS_TO_TICKS(5000);

volatile State Current = State::Unknown;
volatile bool Pressing;
// Made once: a press allocates nothing, so it works however short the heap is
esp_timer_handle_t ReleaseTimer;
volatile gpio_num_t PressedPin;
int64_t LastWakeUs = -WakeHoldoffUs;
TaskHandle_t Watcher;
Handler ShutdownHandler;

State Read()
{
    uint8_t st;
    if (!FpgaLink::Read(Space::Regs, RegLpcStatus, &st, 1) || st == 0xFF) {
        return State::Unknown;
    }
    if (!(st & StLclk)) {
        return State::Off;
    }
    return (st & StLreset) ? State::Running : State::Reset;
}

void Watch(void *)
{
    FpgaSvc::Notify(FpgaSvc::Irq::ConsolePower | FpgaSvc::Irq::LrstAssert |
                    FpgaSvc::Irq::LrstDeassert, xTaskGetCurrentTaskHandle());
    bool ran = false;
    TickType_t off_at = 0;
    for (;;) {
        const State s = Read();
        if (s != Current) {
            ESP_LOGI(Tag, "%s -> %s", Name(Current), Name(s));
            Current = s;
            off_at = xTaskGetTickCount();
        }
        TickType_t wait = CheckTicks;
        if (s == State::Reset || s == State::Running) {
            ran = true;
        } else if (s == State::Off && ran) {
            const TickType_t off_for = xTaskGetTickCount() - off_at;
            if (off_for >= ShutdownTicks) {
                ran = false;
                ESP_LOGI(Tag, "shut down");
                if (const Handler h = ShutdownHandler) {
                    h();
                }
            } else if (ShutdownTicks - off_for < wait) {
                wait = ShutdownTicks - off_for;
            }
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

void Release(void *)
{
    gpio_set_level(PressedPin, 1);
    Pressing = false;
}

void ButtonInit(gpio_num_t pin)
{
    gpio_reset_pin(pin);
    gpio_set_level(pin, 1);                 // released: the console's pull-up holds it
    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
}

}

const char *Name(State s)
{
    switch (s) {
    case State::Off: return "off";
    case State::Reset: return "reset";
    case State::Running: return "running";
    default: return "unknown";
    }
}

void Start()
{
    ButtonInit(Pins::PowerBtn);
    ButtonInit(Pins::EjectBtn);
    const esp_timer_create_args_t release = {
        .callback = Release,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "btn_release",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&release, &ReleaseTimer));
    Current = Read();
    xTaskCreate(Watch, "console", 3072, nullptr, 6, &Watcher);
}

State Get()
{
    return Current;
}

bool Press(Button b)
{
    if (Pressing) {
        return false;
    }
    Pressing = true;
    PressedPin = b == Button::Power ? Pins::PowerBtn : Pins::EjectBtn;
    gpio_set_level(PressedPin, 0);
    if (esp_timer_start_once(ReleaseTimer, uint64_t(PressMs) * 1000) != ESP_OK) {
        gpio_set_level(PressedPin, 1);
        Pressing = false;
        return false;
    }
    ESP_LOGI(Tag, "%s pressed", b == Button::Power ? "power" : "eject");
    return true;
}

void WakeIfOff()
{
    if (Read() != State::Off) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - LastWakeUs < WakeHoldoffUs) {
        return;
    }
    LastWakeUs = now;
    ESP_LOGI(Tag, "controller connected with the console off: turning it on");
    Press(Button::Power);
}

void OnShutdown(Handler h)
{
    ShutdownHandler = h;
}

void RegisterCommands()
{
    HostLink::Register(CmdState, [](HostLink::Args &, HostLink::Reply &reply) {
        reply.PutU8(uint8_t(Read()));
    });
    HostLink::Register(CmdPress, [](HostLink::Args &args, HostLink::Reply &reply) {
        const uint8_t b = args.U8();
        if (args.Bad() || b > uint8_t(Button::Eject)) {
            reply.Fail(HostLink::Error::BadArgs);
            return;
        }
        if (!Press(Button(b))) {
            reply.Fail(HostLink::Error::Busy, "a press is in progress");
        }
    });
}

}
