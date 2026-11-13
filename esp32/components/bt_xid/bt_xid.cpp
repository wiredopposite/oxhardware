#include "bt_xid.hpp"

#include <cstring>

#include "blueretro.h"
#include "console.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "host_link.hpp"
#include "usb_dev.hpp"
#include "xid_map.h"

namespace BtXid {

namespace {

const char *Tag = "bt";

// Commands (oxctl bt)
constexpr uint16_t CmdStatus = 0x0800;     // -> u8 started, u8 pairing, u8 controllers, u8 config
constexpr uint16_t CmdPair = 0x0801;       // u8 on
constexpr uint16_t CmdConfig = 0x0802;     // u8 on: BlueRetro's BLE configuration page
constexpr uint16_t CmdForget = 0x0803;     // all controllers: disconnect, clear pairing keys

// The console's pad follows one output: BlueRetro gives each controller the output of
// its Bluetooth slot, so the first controller isn't always output 0. The pad takes the
// first output with a controller, and the next one when that controller goes.
constexpr int NoOutput = -1;

// Home + Start held this long on any controller presses the console's power button
constexpr uint64_t PowerComboUs = 4 * 1000 * 1000;

bool Started;
volatile int PadOutput = NoOutput;      // written by the Bluetooth tasks only
uint32_t ComboHeld;                     // outputs holding Home + Start (Bluetooth tasks)
esp_timer_handle_t ComboTimer;

void Adopt(int index)
{
    PadOutput = index;
    if (index != NoOutput) {
        UsbDev::SetPadState(wired_adapter.data[index].output);
    }
    UsbDev::SetPadPresent(index != NoOutput);
    if (index != NoOutput) {
        ESP_LOGI(Tag, "pad: controller on output %d", index);
        Console::WakeIfOff();       // with standby power: the controller turns the console on
    } else {
        ESP_LOGI(Tag, "pad: no controller");
    }
}

void ComboExpired(void *)
{
    ESP_LOGI(Tag, "Home + Start: power button");
    Console::Press(Console::Button::Power);
}

// The timer runs while any output holds the combo, from the first to hold it
void Combo(uint8_t index, bool held)
{
    const uint32_t was = ComboHeld;
    ComboHeld = held ? was | (1u << index) : was & ~(1u << index);
    if (!was && ComboHeld) {
        esp_timer_start_once(ComboTimer, PowerComboUs);
    } else if (was && !ComboHeld) {
        esp_timer_stop(ComboTimer);
    }
}

void Updated(uint8_t index)
{
    const wired_data &d = wired_adapter.data[index];
    Combo(index, d.connected && xid_map_power_combo(index));
    if (PadOutput == NoOutput) {
        if (d.connected) {
            Adopt(index);
        }
        return;
    }
    if (index != PadOutput) {
        return;
    }
    if (d.connected) {
        UsbDev::SetPadState(d.output);
        return;
    }
    // Gone: another connected controller takes the pad over, else it is unplugged
    for (int i = 0; i < WIRED_MAX_DEV; i++) {
        if (i != index && wired_adapter.data[i].connected) {
            Adopt(i);
            return;
        }
    }
    Adopt(NoOutput);
}

void Rumble(uint16_t left, uint16_t right)
{
    const int out = PadOutput;
    if (out != NoOutput) {
        xid_map_rumble(uint8_t(out), left, right);
    }
}

int Log(const char *fmt, va_list ap)
{
    if (esp_log_level_get(Tag) < ESP_LOG_INFO) {
        return 0;
    }
    esp_log_writev(ESP_LOG_INFO, Tag, fmt, ap);
    return 0;
}

const blueretro_output_t Output = {
    .name = "Xbox",
    .meta_init = xid_map_meta_init,
    .init_buffer = xid_map_init_buffer,
    .from_generic = xid_map_from_generic,
    .fb_to_generic = xid_map_fb_to_generic,
    .updated = Updated,
};

// BlueRetro's reset macros restart the ESP32: not while the console is on and using the
// pad (and the BIOS, the mailbox)
bool RestartOk()
{
    const Console::State s = Console::Get();
    return s == Console::State::Off || s == Console::State::Unknown;
}

}

void Start()
{
    const esp_timer_create_args_t timer = {
        .callback = ComboExpired,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bt_combo",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer, &ComboTimer);
    blueretro_set_log(Log);
    blueretro_set_restart_hook(RestartOk);
    // Controllers off with the console: they reconnect, and wake it, on their Home button
    Console::OnShutdown(blueretro_disconnect_all);
    UsbDev::OnRumble(Rumble);
    UsbDev::SetPadPresent(false);      // until a controller connects
    // The configuration page has no authentication: closed until asked for. Firmware
    // updates are ours, never BlueRetro's.
    blueretro_config_access(false);
    blueretro_allow_ota(false);
    Started = blueretro_start(&Output);
    if (!Started) {
        ESP_LOGE(Tag, "BlueRetro didn't start");
    }
}

void RegisterCommands()
{
    HostLink::Register(CmdStatus, [](HostLink::Args &, HostLink::Reply &reply) {
        reply.PutU8(Started ? 1 : 0);
        reply.PutU8(Started && blueretro_is_pairing() ? 1 : 0);
        reply.PutU8(Started ? uint8_t(blueretro_connected()) : 0);
        reply.PutU8(blueretro_config_access_on() ? 1 : 0);
    });
    HostLink::Register(CmdConfig, [](HostLink::Args &args, HostLink::Reply &reply) {
        const uint8_t on = args.U8();
        if (args.Bad() || on > 1) {
            reply.Fail(HostLink::Error::BadArgs);
            return;
        }
        blueretro_config_access(on != 0);
    });
    HostLink::Register(CmdForget, [](HostLink::Args &, HostLink::Reply &reply) {
        if (!Started) {
            reply.Fail(HostLink::Error::Failed, "Bluetooth not started");
            return;
        }
        blueretro_forget_all();
    });
    HostLink::Register(CmdPair, [](HostLink::Args &args, HostLink::Reply &reply) {
        const uint8_t on = args.U8();
        if (args.Bad() || on > 1) {
            reply.Fail(HostLink::Error::BadArgs);
            return;
        }
        if (!Started) {
            reply.Fail(HostLink::Error::Failed, "Bluetooth not started");
            return;
        }
        blueretro_pairing(on != 0);
    });
}

}
