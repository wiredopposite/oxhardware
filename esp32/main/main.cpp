#include "bt_xid.hpp"
#include "cfg_bus.hpp"
#include "console.hpp"
#include "esp_log.h"
#include "fpga_link.hpp"
#include "fpga_svc.hpp"
#include "host_link.hpp"
#include "nor.hpp"
#include "prog_server.hpp"
#include "rom_mgr.hpp"
#include "smbus.hpp"
#include "sys_cmds.hpp"
#include "usb_dev.hpp"

static const char *Tag = "main";

extern "C" void app_main(void)
{
    HostLink::Start();
    SysCmds::Register();
    CfgBus::Init();
    CfgBus::RegisterCommands();
    FpgaLink::Init();
    FpgaLink::RegisterCommands();
    FpgaSvc::Start();
    Console::Start();
    Console::RegisterCommands();
    Nor::Init();
    Nor::RegisterCommands();
    Smbus::Init();
    Smbus::RegisterCommands();
    RomMgr::Start();
    ProgServer::RegisterCommands();
    ProgServer::StartMailbox();
    UsbDev::RegisterCommands();
    BtXid::RegisterCommands();
    BtXid::Start();
    UsbDev::Start();            // after BtXid: the pad absent until a controller connects
    ESP_LOGI(Tag, "OpenOX ESP32 up, FPGA CDONE=%d", CfgBus::Cdone());
}
