#include "cfg_bus.hpp"
#include "esp_log.h"
#include "fpga_link.hpp"
#include "host_link.hpp"
#include "nor.hpp"
#include "prog_server.hpp"
#include "smbus.hpp"
#include "sys_cmds.hpp"

static const char *Tag = "main";

extern "C" void app_main(void)
{
    HostLink::Start();
    SysCmds::Register();
    CfgBus::Init();
    CfgBus::RegisterCommands();
    FpgaLink::Init();
    FpgaLink::RegisterCommands();
    Nor::Init();
    Nor::RegisterCommands();
    Smbus::Init();
    Smbus::RegisterCommands();
    ProgServer::RegisterCommands();
    ProgServer::StartMailbox();
    ESP_LOGI(Tag, "OpenOX ESP32 up, FPGA CDONE=%d", CfgBus::Cdone());
}
