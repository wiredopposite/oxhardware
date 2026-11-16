#include "sys_cmds.hpp"

#include <cstring>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_link.hpp"

namespace SysCmds {
namespace {
using HostLink::Args;
using HostLink::Reply;

void Ping(Args &args, Reply &reply)
{
    const size_t n = args.Remaining();
    reply.Put(args.Bytes(n), n);
}

void Version(Args &, Reply &reply)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char built[40];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    reply.PutStr(app->version);
    reply.PutStr(app->idf_ver);
    reply.PutStr(built);
    reply.PutU16(chip.revision);
}

void Status(Args &, Reply &reply)
{
    reply.PutU64(uint64_t(esp_timer_get_time()));
    reply.PutU32(esp_get_free_heap_size());
    reply.PutU32(esp_get_minimum_free_heap_size());
    reply.PutU8(uint8_t(esp_reset_reason()));
}

void RebootTask(void *)
{
    vTaskDelay(pdMS_TO_TICKS(100));          // let the OK response go out
    esp_restart();
}

void Reboot(Args &, Reply &)
{
    xTaskCreate(
        RebootTask, 
        "reboot", 
        2048, 
        nullptr, 
        1, 
        nullptr);
}

void LogLevel(Args &args, Reply &reply)
{
    const uint8_t level = args.U8();
    const size_t n = args.Remaining();
    const uint8_t *tag = args.Bytes(n);
    if (args.Bad() || level > ESP_LOG_VERBOSE || n > 31) {
        reply.Fail(HostLink::Error::BadArgs);
        return;
    }
    char t[32] = "*";
    if (n) {
        memcpy(t, tag, n);
        t[n] = 0;
    }
    esp_log_level_set(t, esp_log_level_t(level));
}

}

void Register()
{
    HostLink::Register(HostLink::Cmd::SysPing, Ping);
    HostLink::Register(HostLink::Cmd::SysVersion, Version);
    HostLink::Register(HostLink::Cmd::SysStatus, Status);
    HostLink::Register(HostLink::Cmd::SysReboot, Reboot);
    HostLink::Register(HostLink::Cmd::LogLevel, LogLevel);
}

}
