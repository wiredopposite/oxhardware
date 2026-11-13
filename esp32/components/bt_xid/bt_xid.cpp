#include "bt_xid.hpp"

#include <cstring>

#include "blueretro.h"
#include "esp_log.h"
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

// Output 0 drives the console's gamepad
constexpr uint8_t PadOutput = 0;

bool Started;
uint32_t WasConnected;

void Updated(uint8_t index)
{
    if (index != PadOutput) {
        return;
    }
    const wired_data &d = wired_adapter.data[index];
    UsbDev::SetPadState(d.output);
    if (d.connected != WasConnected) {
        WasConnected = d.connected;
        UsbDev::SetPadPresent(d.connected != 0);
        ESP_LOGI(Tag, "controller %s", d.connected ? "connected" : "gone");
    }
}

void Rumble(uint16_t left, uint16_t right)
{
    xid_map_rumble(PadOutput, left, right);
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

}

void Start()
{
    blueretro_set_log(Log);
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
