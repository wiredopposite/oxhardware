#include "host_link.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "cobs.hpp"
#include "crc32.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace HostLink {
namespace {
constexpr uart_port_t Port = UART_NUM_0;
constexpr size_t HeaderLen = 4;
constexpr size_t CrcLen = 4;
constexpr size_t FrameMax = HeaderLen + MaxPayload + CrcLen;
constexpr size_t MaxCommands = 64;

struct Entry {
    uint16_t cmd;
    Handler handler;
};
Entry Commands[MaxCommands];
size_t CommandCount = 0;

SemaphoreHandle_t TxLock;
uint8_t TxFrame[FrameMax];
uint8_t TxEncoded[Cobs::EncodedMax(FrameMax) + 2];

void SendFrame(FrameType type, uint8_t seq, const void *prefix, size_t prefix_len,
               const void *body, size_t body_len)
{
    const size_t len = prefix_len + body_len;
    if (len > MaxPayload) {
        return;
    }
    xSemaphoreTake(TxLock, portMAX_DELAY);
    TxFrame[0] = uint8_t(type);
    TxFrame[1] = seq;
    TxFrame[2] = uint8_t(len);
    TxFrame[3] = uint8_t(len >> 8);
    if (prefix_len) {
        memcpy(TxFrame + HeaderLen, prefix, prefix_len);
    }
    if (body_len) {
        memcpy(TxFrame + HeaderLen + prefix_len, body, body_len);
    }
    const uint32_t crc = Crc32(TxFrame, HeaderLen + len);
    uint8_t *c = TxFrame + HeaderLen + len;
    c[0] = uint8_t(crc);
    c[1] = uint8_t(crc >> 8);
    c[2] = uint8_t(crc >> 16);
    c[3] = uint8_t(crc >> 24);
    // Leading 0x00 so stray console text before the frame is cut off as its own chunk.
    TxEncoded[0] = 0;
    const size_t n = Cobs::Encode(TxFrame, HeaderLen + len + CrcLen, TxEncoded + 1);
    TxEncoded[n + 1] = 0;
    uart_write_bytes(Port, TxEncoded, n + 2);
    xSemaphoreGive(TxLock);
}

Handler Find(uint16_t cmd)
{
    for (size_t i = 0; i < CommandCount; i++) {
        if (Commands[i].cmd == cmd) {
            return Commands[i].handler;
        }
    }
    return nullptr;
}

Reply ReplyBuf;

void Dispatch(uint8_t *frame, size_t len)
{
    if (len < HeaderLen + CrcLen) {
        return;
    }
    const size_t plen = size_t(frame[2]) | (size_t(frame[3]) << 8);
    if (plen != len - HeaderLen - CrcLen) {
        return;
    }
    const uint8_t *c = frame + HeaderLen + plen;
    const uint32_t crc = uint32_t(c[0]) | (uint32_t(c[1]) << 8) | (uint32_t(c[2]) << 16) |
                         (uint32_t(c[3]) << 24);
    if (crc != Crc32(frame, HeaderLen + plen)) {
        return;
    }
    if (FrameType(frame[0]) != FrameType::Request || plen < 2) {
        return;
    }
    const uint8_t seq = frame[1];
    const uint8_t *payload = frame + HeaderLen;
    const uint16_t cmd = uint16_t(payload[0] | (payload[1] << 8));
    const uint8_t cmd_le[2] = { payload[0], payload[1] };

    ReplyBuf.Reset();
    Args args(payload + 2, plen - 2);
    if (Handler h = Find(cmd)) {
        h(args, ReplyBuf);
        if (!ReplyBuf.Failed() && args.Bad()) {
            ReplyBuf.Fail(Error::BadArgs);
        }
    }
    else {
        ReplyBuf.Fail(Error::UnknownCommand);
    }
    if (ReplyBuf.Failed()) {
        uint8_t head[3] = { cmd_le[0], cmd_le[1], uint8_t(ReplyBuf.Code()) };
        SendFrame(FrameType::Err, seq, head, 3, ReplyBuf.Data(), ReplyBuf.Size());
    }
    else {
        SendFrame(FrameType::Ok, seq, cmd_le, 2, ReplyBuf.Data(), ReplyBuf.Size());
    }
}

uint8_t RxFrame[Cobs::EncodedMax(FrameMax)];

void RxTask(void *)
{
    size_t len = 0;
    bool overflow = false;
    uint8_t chunk[256];
    for (;;) {
        // Block for the first byte only, then take whatever else is already buffered, so
        // a short request is handled as soon as its delimiter arrives.
        int n = uart_read_bytes(Port, chunk, 1, portMAX_DELAY);
        size_t avail = 0;
        uart_get_buffered_data_len(Port, &avail);
        if (n == 1 && avail) {
            const int more = uart_read_bytes(Port, chunk + 1,
                                             avail < sizeof(chunk) - 1 ? avail : sizeof(chunk) - 1, 0);
            n += more > 0 ? more : 0;
        }
        for (int i = 0; i < n; i++) {
            const uint8_t b = chunk[i];
            if (b == 0) {
                if (len && !overflow) {
                    const size_t dlen = Cobs::Decode(RxFrame, len);
                    if (dlen) {
                        Dispatch(RxFrame, dlen);
                    }
                }
                len = 0;
                overflow = false;
            }
            else if (len < sizeof(RxFrame)) {
                RxFrame[len++] = b;
            }
            else {
                overflow = true;
            }
        }
    }
}

int LogVprintf(const char *fmt, va_list ap)
{
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n < 0) {
        return n;
    }
    const size_t len = (size_t(n) < sizeof(line)) ? size_t(n) : sizeof(line) - 1;
    SendFrame(FrameType::Log, 0, nullptr, 0, line, len);
    return n;
}

}

void Reply::Reset()
{
    Len = 0;
    HasError = false;
    ErrCode = Error::Failed;
}

bool Reply::Put(const void *data, size_t len)
{
    if (Len + len > sizeof(Buf)) {
        Fail(Error::Failed, "reply too long");
        return false;
    }
    memcpy(Buf + Len, data, len);
    Len += len;
    return true;
}

bool Reply::PutU16(uint16_t v)
{
    const uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
    return Put(b, 2);
}

bool Reply::PutU32(uint32_t v)
{
    const uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
    return Put(b, 4);
}

bool Reply::PutU64(uint64_t v)
{
    return PutU32(uint32_t(v)) && PutU32(uint32_t(v >> 32));
}

bool Reply::PutStr(const char *s)
{
    size_t n = strlen(s);
    if (n > 255) {
        n = 255;
    }
    return PutU8(uint8_t(n)) && Put(s, n);
}

void Reply::Fail(Error code, const char *message)
{
    HasError = true;
    ErrCode = code;
    Len = 0;
    if (message) {
        size_t n = strlen(message);
        if (n > sizeof(Buf)) {
            n = sizeof(Buf);
        }
        memcpy(Buf, message, n);
        Len = n;
    }
}

uint8_t Args::U8()
{
    const uint8_t *p = Bytes(1);
    return p ? p[0] : 0;
}

uint16_t Args::U16()
{
    const uint8_t *p = Bytes(2);
    return p ? uint16_t(p[0] | (p[1] << 8)) : 0;
}

uint32_t Args::U32()
{
    const uint8_t *p = Bytes(4);
    return p ? (uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                (uint32_t(p[3]) << 24))
             : 0;
}

const uint8_t *Args::Bytes(size_t n)
{
    if (n > Left) {
        Overrun = true;
        Left = 0;
        return nullptr;
    }
    const uint8_t *p = Ptr;
    Ptr += n;
    Left -= n;
    return p;
}

void Register(uint16_t cmd, Handler handler)
{
    for (size_t i = 0; i < CommandCount; i++) {
        if (Commands[i].cmd == cmd) {
            Commands[i].handler = handler;
            return;
        }
    }
    if (CommandCount < MaxCommands) {
        Commands[CommandCount++] = { cmd, handler };
    }
}

uint8_t *Scratch()
{
    static uint8_t buf[ScratchSize];
    return buf;
}

void SendEvent(uint16_t id, const void *data, size_t len)
{
    const uint8_t head[2] = { uint8_t(id), uint8_t(id >> 8) };
    SendFrame(FrameType::Event, 0, head, 2, data, len);
}

void Start()
{
    TxLock = xSemaphoreCreateMutex();
    const uart_config_t cfg = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };
    uart_driver_install(Port, 8192, 8192, 0, nullptr, 0);
    uart_param_config(Port, &cfg);
    esp_log_set_vprintf(LogVprintf);
    xTaskCreatePinnedToCore(RxTask, "host_link", 6144, nullptr, 5, nullptr, 1);
}

}
