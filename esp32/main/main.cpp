#include "esp_log.h"
#include "host_link.hpp"
#include "sys_cmds.hpp"

static const char *Tag = "main";

extern "C" void app_main(void)
{
    HostLink::Start();
    SysCmds::Register();
    ESP_LOGI(Tag, "OpenOX ESP32 up");
}
