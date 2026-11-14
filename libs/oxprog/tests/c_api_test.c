/*
 * The C API against a fake chip: a transport that answers each packet the way
 * the ESP32 would (info, an SMBus failure, an upload, a download). Plain C, so
 * it also checks oxprog.h compiles as C.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oxprog/oxprog.h"

#define MAGIC 0x5046584Fu
#define PAYLOAD_MAX 4096

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ------------------------------------------------------------ fake chip */
typedef struct {
    uint8_t in[16 + PAYLOAD_MAX];
    size_t in_len;
    uint8_t out[16 + PAYLOAD_MAX];
    size_t out_len, out_pos;
    /* upload state */
    uint8_t image[64 * 1024];
    uint32_t image_size;
    uint8_t upload_type;
} chip_t;

static const uint8_t download_data[10000] = { 1, 2, 3 };

static void reply(chip_t *c, uint8_t cmd, uint8_t status, const void *payload, uint16_t len)
{
    uint8_t *h = c->out;
    put32(h, MAGIC);
    h[4] = cmd;
    h[5] = status;
    h[6] = (uint8_t)len; h[7] = (uint8_t)(len >> 8);
    put32(h + 8, len ? oxprog_crc32(payload, len, 0) : 0);
    put32(h + 12, oxprog_crc32(h, 12, 0));
    if (len) {
        memcpy(h + 16, payload, len);
    }
    c->out_len = 16u + len;
    c->out_pos = 0;
}

static void handle(chip_t *c, uint8_t cmd, const uint8_t *p, uint16_t len)
{
    const uint8_t mode = cmd & 0x0F, action = cmd >> 4;
    if (mode == 1 && action == 0) {                     /* GetInfo */
        oxprog_info_t info;
        memset(&info, 0, sizeof(info));
        info.version = 3;
        info.firmware_version[0] = 1; info.firmware_version[1] = 2; info.firmware_version[2] = 3;
        info.active_entry = OXPROG_NO_ENTRY;
        info.flash_size = 4u << 20;
        reply(c, cmd, OXPROG_OK, &info, sizeof(info));
    } else if (mode == 1 && action == 2) {              /* SmbusRead: nothing acks */
        reply(c, cmd, OXPROG_SMBUS_ERROR, NULL, 0);
    } else if (mode == 2 && action == 0) {              /* UploadBegin */
        c->upload_type = p[0];
        c->image_size = get32(p + 4);
        reply(c, cmd, c->image_size <= sizeof(c->image) ? OXPROG_OK : OXPROG_BAD_SIZE, NULL, 0);
    } else if (mode == 2 && action == 1) {              /* UploadWrite */
        const uint32_t off = get32(p);
        memcpy(c->image + off, p + 4, len - 4u);
        reply(c, cmd, OXPROG_OK, NULL, 0);
    } else if (mode == 2 && action == 2) {              /* UploadCommit */
        const int ok = get32(p) == oxprog_crc32(c->image, c->image_size, 0);
        reply(c, cmd, ok ? OXPROG_OK : OXPROG_CRC_MISMATCH, NULL, 0);
    } else if (mode == 3 && action == 0) {              /* DownloadBegin */
        uint8_t r[8];
        put32(r, sizeof(download_data));
        put32(r + 4, oxprog_crc32(download_data, sizeof(download_data), 0));
        reply(c, cmd, OXPROG_OK, r, sizeof(r));
    } else if (mode == 3 && action == 1) {              /* DownloadRead */
        const uint32_t off = get32(p), n = get32(p + 4);
        reply(c, cmd, OXPROG_OK, download_data + off, (uint16_t)n);
    } else if (mode == 3 && action == 2) {              /* DownloadEnd */
        reply(c, cmd, OXPROG_OK, NULL, 0);
    } else {
        reply(c, cmd, OXPROG_BAD_COMMAND, NULL, 0);
    }
}

static int chip_write(void *ctx, const void *data, size_t len)
{
    chip_t *c = ctx;
    if (c->in_len + len > sizeof(c->in)) {
        return 0;
    }
    memcpy(c->in + c->in_len, data, len);
    c->in_len += len;
    if (c->in_len >= 16) {
        const uint16_t plen = (uint16_t)(c->in[6] | c->in[7] << 8);
        if (c->in_len == 16u + plen) {
            handle(c, c->in[4], c->in + 16, plen);
            c->in_len = 0;
        }
    }
    return 1;
}

static size_t chip_read(void *ctx, void *data, size_t len, uint32_t timeout_ms)
{
    chip_t *c = ctx;
    size_t n = c->out_len - c->out_pos;
    (void)timeout_ms;
    if (n > len) {
        n = len;
    }
    memcpy(data, c->out + c->out_pos, n);
    c->out_pos += n;
    return n;
}

static uint32_t progress_calls, progress_last;

static void on_progress(void *ctx, uint32_t done, uint32_t total)
{
    (void)ctx; (void)total;
    progress_calls++;
    progress_last = done;
}

int main(void)
{
    static chip_t chip;
    oxprog_transport_t t = { &chip, chip_write, chip_read, NULL };
    oxprog_client_t *client = oxprog_client_new(&t, 1000);
    CHECK(client != NULL);

    oxprog_info_t info;
    CHECK(oxprog_get_info(client, &info) == OXPROG_OK);
    CHECK(info.firmware_version[0] == 1 && info.firmware_version[2] == 3);
    CHECK(info.active_entry == OXPROG_NO_ENTRY);
    CHECK(info.flash_size == 4u << 20);

    uint8_t v = 0;
    CHECK(oxprog_smbus_read(client, 0x10, 0x03, &v) == OXPROG_SMBUS_ERROR);
    CHECK(strlen(oxprog_status_name(OXPROG_SMBUS_ERROR)) > 0);

    static uint8_t image[20000];
    for (size_t i = 0; i < sizeof(image); i++) {
        image[i] = (uint8_t)(i * 7);
    }
    CHECK(oxprog_upload(client, OXPROG_UPLOAD_FPGA_BITSTREAM, image, sizeof(image), NULL,
                        on_progress, NULL, NULL) == OXPROG_OK);
    CHECK(chip.upload_type == OXPROG_UPLOAD_FPGA_BITSTREAM);
    CHECK(chip.image_size == sizeof(image) && !memcmp(chip.image, image, sizeof(image)));
    CHECK(progress_calls > 0 && progress_last == sizeof(image));
    CHECK(oxprog_upload(client, 99, image, sizeof(image), NULL, NULL, NULL, NULL) ==
          OXPROG_BAD_COMMAND);

    uint8_t *data = NULL;
    size_t len = 0;
    CHECK(oxprog_download(client, OXPROG_DOWNLOAD_ACTIVE_ROM, OXPROG_NO_ENTRY, &data, &len,
                          NULL, NULL) == OXPROG_OK);
    CHECK(len == sizeof(download_data) && data != NULL && !memcmp(data, download_data, len));
    oxprog_free(data);

    CHECK(oxprog_crc32("123456789", 9, 0) == 0xCBF43926u);

    oxprog_client_free(client);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
