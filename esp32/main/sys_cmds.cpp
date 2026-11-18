#include "sys_cmds.hpp"

#include <cstring>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
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

// Made once: a restart allocates nothing, so it works however short the heap is
esp_timer_handle_t RestartTimer;

// Free heap through the uptime, kept here so it can be read without a log running
constexpr uint64_t SampleUs = 5ull * 60 * 1000 * 1000;
constexpr int SampleCount = 96;             // 8 hours
struct Sample {
    uint16_t Minutes;
    uint16_t Free16;                        // in 16 byte units
    uint16_t Largest16;
};
Sample Samples[SampleCount];
int SampleNext;
int SamplesTaken;
esp_timer_handle_t SampleTimer;

// What last couldn't be allocated, written from the allocator so nothing is logged there
volatile uint32_t FailedAllocs;
volatile uint32_t FailedSize;
volatile uint32_t FailedCaps;
const char *volatile FailedFunction;

// Allocations still held since the trace started, with who made them
heap_trace_record_t *TraceRecords;
size_t TraceCapacity;
bool Tracing;

uint32_t Largest()
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

void TakeSample(void *)
{
    Sample &s = Samples[SampleNext];
    s.Minutes = uint16_t(esp_timer_get_time() / 60000000);
    s.Free16 = uint16_t(esp_get_free_heap_size() / 16);
    s.Largest16 = uint16_t(Largest() / 16);
    SampleNext = (SampleNext + 1) % SampleCount;
    if (SamplesTaken < SampleCount) {
        SamplesTaken++;
    }
}

void AllocFailed(size_t size, uint32_t caps, const char *function)
{
    FailedAllocs = FailedAllocs + 1;
    FailedSize = uint32_t(size);
    FailedCaps = caps;
    FailedFunction = function;
}

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
    reply.PutU32(Largest());
}

void Restart(void *)
{
    esp_restart();
}

void Reboot(Args &, Reply &)
{
    // After the OK response has gone out
    esp_timer_start_once(RestartTimer, 100 * 1000);
}

// The heap now, its samples oldest first, the allocations that failed, then every task
void Heap(Args &, Reply &reply)
{
    reply.PutU32(esp_get_free_heap_size());
    reply.PutU32(esp_get_minimum_free_heap_size());
    reply.PutU32(Largest());
    reply.PutU32(heap_caps_get_total_size(MALLOC_CAP_8BIT));
    reply.PutU32(FailedAllocs);
    reply.PutU32(FailedSize);
    reply.PutU32(FailedCaps);
    const char *function = FailedFunction;
    reply.PutStr(function ? function : "");

    reply.PutU8(uint8_t(SamplesTaken));
    for (int i = 0; i < SamplesTaken; i++) {
        const Sample &s = Samples[(SampleNext - SamplesTaken + i + SampleCount) % SampleCount];
        reply.PutU16(s.Minutes);
        reply.PutU16(s.Free16);
        reply.PutU16(s.Largest16);
    }

    TaskStatus_t tasks[32];
    const UBaseType_t n = uxTaskGetSystemState(tasks, 32, nullptr);
    reply.PutU8(uint8_t(uxTaskGetNumberOfTasks()));
    reply.PutU8(uint8_t(n));
    for (UBaseType_t i = 0; i < n; i++) {
        reply.PutStr(tasks[i].pcTaskName);
        reply.PutU8(uint8_t(tasks[i].eCurrentState));
        reply.PutU8(uint8_t(tasks[i].uxCurrentPriority));
        reply.PutU32(uint32_t(tasks[i].usStackHighWaterMark));
    }
}

// Op 0 starts a trace of what's allocated and not freed, with Count records (taken from the
// heap before it starts), 1 stops it, 2 reads the records back
void Trace(Args &args, Reply &reply)
{
    const uint8_t op = args.U8();
    if (args.Bad() || op > 2) {
        reply.Fail(HostLink::Error::BadArgs);
        return;
    }
    if (op == 0) {
        const uint16_t count = args.Remaining() >= 2 ? args.U16() : 64;
        if (Tracing) {
            heap_trace_stop();
            Tracing = false;
        }
        if (TraceCapacity != count) {
            free(TraceRecords);
            TraceRecords = static_cast<heap_trace_record_t *>(
                heap_caps_malloc(sizeof(heap_trace_record_t) * count, MALLOC_CAP_8BIT));
            TraceCapacity = TraceRecords ? count : 0;
        }
        if (!TraceRecords ||
            heap_trace_init_standalone(TraceRecords, TraceCapacity) != ESP_OK ||
            heap_trace_start(HEAP_TRACE_LEAKS) != ESP_OK) {
            reply.Fail(HostLink::Error::Failed, "no room for the trace records");
            return;
        }
        Tracing = true;
        return;
    }
    if (op == 1) {
        if (Tracing) {
            heap_trace_stop();
            Tracing = false;
        }
        return;
    }
    const size_t count = TraceRecords ? heap_trace_get_count() : 0;
    reply.PutU8(Tracing);
    reply.PutU16(uint16_t(count));
    for (size_t i = 0; i < count; i++) {
        heap_trace_record_t r;
        if (heap_trace_get(i, &r) != ESP_OK) {
            break;
        }
        reply.PutU32(r.ccount);
        reply.PutU32(uint32_t(uintptr_t(r.address)));
        reply.PutU32(uint32_t(r.size));
        for (int d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH; d++) {
            reply.PutU32(uint32_t(uintptr_t(r.alloced_by[d])));
        }
    }
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
    const esp_timer_create_args_t restart = {
        .callback = Restart,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "restart",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&restart, &RestartTimer));
    const esp_timer_create_args_t sample = {
        .callback = TakeSample,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "heap_sample",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&sample, &SampleTimer));
    TakeSample(nullptr);
    esp_timer_start_periodic(SampleTimer, SampleUs);
    heap_caps_register_failed_alloc_callback(AllocFailed);

    HostLink::Register(HostLink::Cmd::SysPing, Ping);
    HostLink::Register(HostLink::Cmd::SysVersion, Version);
    HostLink::Register(HostLink::Cmd::SysStatus, Status);
    HostLink::Register(HostLink::Cmd::SysReboot, Reboot);
    HostLink::Register(HostLink::Cmd::SysHeap, Heap);
    HostLink::Register(HostLink::Cmd::SysTrace, Trace);
    HostLink::Register(HostLink::Cmd::LogLevel, LogLevel);
}

}
