#include "chip_update.hpp"

#include <cstdlib>
#include <cstring>

#include "cfg_bus.hpp"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fpga_link.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nor.hpp"

namespace ChipUpdate {

namespace {

using namespace OxProg;

const char *Tag = "update";

constexpr int64_t IdleExpireUs = 30 * 1000 * 1000;     // an abandoned upload frees itself
constexpr uint32_t Sector = 4096;

// ------------------------------------------------------------ bitstream copies
// The `golden` partition (docs/03-esp32.md §4) holds two slots: the last good bitstream
// and the one being uploaded. Each is a 4 KB header sector, then the image; the header
// is written last, so a valid header means a complete, checked image.
constexpr uint8_t GoldenSubtype = 0x40;
constexpr uint32_t SlotSize = 0x20000;
constexpr uint32_t HeaderSize = Sector;
constexpr uint32_t ImageMax = SlotSize - HeaderSize;
constexpr uint32_t Magic = 0x5342584F;      // "OXBS"

enum class Slot : uint32_t { Good = 0, Staged = 1 };

struct SlotHeader {
    uint32_t Magic;
    uint32_t Size;
    uint32_t Crc32;
    uint32_t Check;             // ~Magic ^ Size ^ Crc32
};

const esp_partition_t *Golden;

uint32_t Base(Slot s) { return uint32_t(s) * SlotSize; }

bool ReadHeader(Slot s, SlotHeader &h)
{
    return Golden != nullptr && esp_partition_read(Golden, Base(s), &h, sizeof(h)) == ESP_OK &&
           h.Magic == Magic && h.Check == (~Magic ^ h.Size ^ h.Crc32) && h.Size != 0 &&
           h.Size <= ImageMax;
}

bool WriteHeader(Slot s, uint32_t size, uint32_t crc)
{
    const SlotHeader h = {Magic, size, crc, ~Magic ^ size ^ crc};
    return esp_partition_write(Golden, Base(s), &h, sizeof(h)) == ESP_OK;
}

// A 4 KB buffer for the length of one operation (DRAM is short)
struct Buffer {
    uint8_t *Data = static_cast<uint8_t *>(malloc(Sector));
    ~Buffer() { free(Data); }
};

bool SlotCrc(Slot s, uint32_t size, uint32_t &crc)
{
    Buffer b;
    if (b.Data == nullptr) {
        return false;
    }
    crc = 0;
    for (uint32_t off = 0; off < size; off += Sector) {
        const uint32_t n = size - off < Sector ? size - off : Sector;
        if (esp_partition_read(Golden, Base(s) + HeaderSize + off, b.Data, n) != ESP_OK) {
            return false;
        }
        crc = Crc32(b.Data, n, crc);
    }
    return true;
}

// The image in a slot to the FPGA's flash, checked: borrowed from the running FPGA, or
// with it held
bool SlotToFpga(Slot s, uint32_t size, uint32_t crc)
{
    Buffer b;
    if (b.Data == nullptr || !Nor::EraseBitstream((size + Sector - 1) & ~(Sector - 1))) {
        return false;
    }
    for (uint32_t off = 0; off < size; off += Sector) {
        const uint32_t n = size - off < Sector ? size - off : Sector;
        if (esp_partition_read(Golden, Base(s) + HeaderSize + off, b.Data, n) != ESP_OK ||
            !Nor::WriteBitstream(off, b.Data, n)) {
            return false;
        }
    }
    uint32_t got;
    return Nor::Crc32(0, size, &got) && got == crc;
}

// The bitstream the FPGA is running, as the last good one: the first update has no copy
// yet. Its length isn't known, so the whole image area is kept.
bool FpgaToGood()
{
    Buffer b;
    if (b.Data == nullptr ||
        esp_partition_erase_range(Golden, Base(Slot::Good), SlotSize) != ESP_OK) {
        return false;
    }
    uint32_t crc = 0;
    for (uint32_t off = 0; off < ImageMax; off += Sector) {
        if (!Nor::Read(off, b.Data, Sector) ||
            esp_partition_write(Golden, Base(Slot::Good) + HeaderSize + off, b.Data, Sector) !=
                ESP_OK) {
            return false;
        }
        crc = Crc32(b.Data, Sector, crc);
    }
    return WriteHeader(Slot::Good, ImageMax, crc);
}

bool StagedToGood(uint32_t size, uint32_t crc)
{
    Buffer b;
    if (b.Data == nullptr ||
        esp_partition_erase_range(Golden, Base(Slot::Good), SlotSize) != ESP_OK) {
        return false;
    }
    for (uint32_t off = 0; off < size; off += Sector) {
        const uint32_t n = size - off < Sector ? size - off : Sector;
        if (esp_partition_read(Golden, Base(Slot::Staged) + HeaderSize + off, b.Data, n) !=
                ESP_OK ||
            esp_partition_write(Golden, Base(Slot::Good) + HeaderSize + off, b.Data, n) !=
                ESP_OK) {
            return false;
        }
    }
    return WriteHeader(Slot::Good, size, crc);
}

// Reset and reload the FPGA; true once its design answers on the link
bool Reload()
{
    if (!CfgBus::Hold() || CfgBus::Release(1000) < 0) {
        return false;
    }
    for (int i = 0; i < 50; i++) {
        uint8_t id[4];
        if (FpgaLink::Read(FpgaLink::Space::Regs, 0, id, sizeof(id)) && !memcmp(id, "OXFG", 4)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

// The last good bitstream back, with the FPGA held, then reloaded
bool RestoreGood()
{
    SlotHeader h;
    if (!ReadHeader(Slot::Good, h)) {
        ESP_LOGE(Tag, "no good bitstream to restore");
        return false;
    }
    if (!CfgBus::Hold()) {
        return false;
    }
    const bool written = SlotToFpga(Slot::Good, h.Size, h.Crc32);
    CfgBus::Release(1000);
    if (!written) {
        ESP_LOGE(Tag, "restoring the good bitstream failed");
        return false;
    }
    ESP_LOGW(Tag, "good bitstream restored");
    return Reload();
}

// An iCE40 bitstream has its sync word near the start
bool LooksLikeBitstream(Slot s)
{
    uint8_t head[64];
    if (esp_partition_read(Golden, Base(s) + HeaderSize, head, sizeof(head)) != ESP_OK) {
        return false;
    }
    static const uint8_t Sync[] = {0x7E, 0xAA, 0x99, 0x7E};
    for (size_t i = 0; i + sizeof(Sync) <= sizeof(head); i++) {
        if (!memcmp(head + i, Sync, sizeof(Sync))) {
            return true;
        }
    }
    return false;
}

Status CommitBitstream(uint32_t size, uint32_t crc)
{
    uint32_t got;
    if (!SlotCrc(Slot::Staged, size, got)) {
        return Status::FlashError;
    }
    if (got != crc) {
        ESP_LOGW(Tag, "bitstream CRC %08lX, expected %08lX", (unsigned long)got,
                 (unsigned long)crc);
        return Status::CrcMismatch;
    }
    if (!LooksLikeBitstream(Slot::Staged)) {
        ESP_LOGW(Tag, "not an iCE40 bitstream");
        return Status::BadCommand;
    }
    if (!WriteHeader(Slot::Staged, size, crc)) {
        return Status::FlashError;
    }
    SlotHeader good;
    if (!ReadHeader(Slot::Good, good)) {
        if (!CfgBus::Cdone() || !FpgaToGood()) {
            ESP_LOGE(Tag, "no copy of the running bitstream: not updating");
            return Status::FlashError;
        }
        ESP_LOGI(Tag, "running bitstream kept as the good one");
    }
    if (!SlotToFpga(Slot::Staged, size, crc)) {
        ESP_LOGE(Tag, "writing the bitstream failed");
        RestoreGood();
        return Status::FlashError;
    }
    if (!Reload()) {
        ESP_LOGE(Tag, "the new bitstream doesn't come up");
        RestoreGood();
        return Status::FlashError;
    }
    if (!StagedToGood(size, crc)) {
        ESP_LOGW(Tag, "the new bitstream runs, but isn't kept as the good one");
    }
    ESP_LOGI(Tag, "bitstream updated (%lu bytes, CRC %08lX)", (unsigned long)size,
             (unsigned long)crc);
    return Status::Ok;
}

// ------------------------------------------------------------ ESP32 firmware
// Written to the OTA partition not running, in order (erased as it goes), then booted. The
// new firmware is on trial: it confirms itself once it has run for a while
// (ConfirmFirmware), and the bootloader goes back to this one if it resets before then.
constexpr uint64_t ConfirmAfterUs = 30ull * 1000 * 1000;

const esp_partition_t *OtaTarget;
esp_ota_handle_t Ota;
esp_timer_handle_t ConfirmTimer;

void ConfirmFirmware(void *)
{
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(Tag, "firmware confirmed");
    }
}

void RestartTask(void *)
{
    vTaskDelay(pdMS_TO_TICKS(500));     // the reply goes out first
    esp_restart();
}

Status CommitFirmware(uint32_t size, uint32_t crc)
{
    const esp_err_t end = esp_ota_end(Ota);
    if (end != ESP_OK) {
        ESP_LOGW(Tag, "firmware image rejected: %s", esp_err_to_name(end));
        return end == ESP_ERR_OTA_VALIDATE_FAILED ? Status::CrcMismatch : Status::FlashError;
    }
    Buffer b;
    if (b.Data == nullptr) {
        return Status::FlashError;
    }
    uint32_t got = 0;
    for (uint32_t off = 0; off < size; off += Sector) {
        const uint32_t n = size - off < Sector ? size - off : Sector;
        if (esp_partition_read(OtaTarget, off, b.Data, n) != ESP_OK) {
            return Status::FlashError;
        }
        got = Crc32(b.Data, n, got);
    }
    if (got != crc) {
        ESP_LOGW(Tag, "firmware CRC %08lX, expected %08lX", (unsigned long)got,
                 (unsigned long)crc);
        return Status::CrcMismatch;
    }
    if (esp_ota_set_boot_partition(OtaTarget) != ESP_OK) {
        return Status::FlashError;
    }
    ESP_LOGI(Tag, "firmware updated (%lu bytes): restarting into %s", (unsigned long)size,
             OtaTarget->label);
    xTaskCreate(RestartTask, "update_restart", 2048, nullptr, 1, nullptr);
    return Status::Ok;
}

// ------------------------------------------------------------ transfer
struct {
    bool Active;
    UploadType Type;
    uint32_t Size;
    uint32_t Next;              // firmware: the next offset (written in order)
    uint32_t Erased;            // bitstream: sectors of the staged slot erased
    int64_t Last;
} Up;

bool Live()
{
    if (Up.Active && esp_timer_get_time() - Up.Last > IdleExpireUs) {
        ESP_LOGW(Tag, "upload abandoned");
        Abort();
    }
    return Up.Active;
}

}

void Start()
{
    Golden = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      esp_partition_subtype_t(GoldenSubtype), "golden");
    if (Golden == nullptr || Golden->size < 2 * SlotSize) {
        ESP_LOGE(Tag, "no golden partition");
        Golden = nullptr;
    }
    // A firmware on trial confirms itself once it has run for a while
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_timer_create_args_t args = {
            .callback = ConfirmFirmware,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "fw_confirm",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &ConfirmTimer) == ESP_OK) {
            esp_timer_start_once(ConfirmTimer, ConfirmAfterUs);
            ESP_LOGI(Tag, "new firmware: confirmed in %llu s", ConfirmAfterUs / 1000000);
        }
    }
    // An FPGA that doesn't configure has a damaged bitstream (an update cut off)
    for (int i = 0; i < 100 && !CfgBus::Cdone(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!CfgBus::Cdone() && !CfgBus::Held()) {
        ESP_LOGW(Tag, "FPGA not configured: restoring the good bitstream");
        RestoreGood();
    }
}

bool Handles(UploadType type)
{
    return type == UploadType::FpgaBitstream || type == UploadType::Esp32Firmware;
}

bool Active()
{
    return Live();
}

Status Begin(UploadType type, uint32_t size)
{
    if (!Handles(type)) {
        return Status::BadCommand;
    }
    if (Live()) {
        return Status::ProgrammerBusy;
    }
    Up = {};
    if (type == UploadType::FpgaBitstream) {
        if (Golden == nullptr) {
            return Status::LayoutInvalid;
        }
        if (size == 0 || size > ImageMax) {
            return Status::BadSize;
        }
        // The staged slot's header first: it's invalid until the upload is committed
        if (esp_partition_erase_range(Golden, Base(Slot::Staged), Sector) != ESP_OK) {
            return Status::FlashError;
        }
    } else {
        OtaTarget = esp_ota_get_next_update_partition(nullptr);
        if (OtaTarget == nullptr) {
            return Status::LayoutInvalid;
        }
        if (size == 0 || size > OtaTarget->size) {
            return Status::BadSize;
        }
        if (esp_ota_begin(OtaTarget, OTA_WITH_SEQUENTIAL_WRITES, &Ota) != ESP_OK) {
            return Status::FlashError;
        }
    }
    Up.Active = true;
    Up.Type = type;
    Up.Size = size;
    Up.Last = esp_timer_get_time();
    ESP_LOGI(Tag, "%s upload, %lu bytes",
             type == UploadType::FpgaBitstream ? "bitstream" : "firmware", (unsigned long)size);
    return Status::Ok;
}

Status Write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (!Live()) {
        return Status::NotStarted;
    }
    if (len == 0 || offset >= Up.Size || len > Up.Size - offset) {
        return Status::BadOffset;
    }
    if (Up.Type == UploadType::Esp32Firmware) {
        if (offset != Up.Next) {
            return Status::BadOffset;
        }
        if (esp_ota_write(Ota, data, len) != ESP_OK) {
            return Status::FlashError;
        }
        Up.Next += len;
    } else {
        for (uint32_t s = offset / Sector; s <= (offset + len - 1) / Sector; s++) {
            if (!(Up.Erased & (1u << s))) {
                if (esp_partition_erase_range(Golden, Base(Slot::Staged) + HeaderSize + s * Sector,
                                              Sector) != ESP_OK) {
                    return Status::FlashError;
                }
                Up.Erased |= 1u << s;
            }
        }
        if (esp_partition_write(Golden, Base(Slot::Staged) + HeaderSize + offset, data, len) !=
            ESP_OK) {
            return Status::FlashError;
        }
    }
    Up.Last = esp_timer_get_time();
    return Status::Ok;
}

Status Commit(uint32_t crc)
{
    if (!Live()) {
        return Status::NotStarted;
    }
    Up.Active = false;
    if (Up.Type == UploadType::Esp32Firmware) {
        if (Up.Next != Up.Size) {
            esp_ota_abort(Ota);
            return Status::BadSize;
        }
        return CommitFirmware(Up.Size, crc);
    }
    // Sectors never written were never erased: the whole image is CRC'd as stored
    return CommitBitstream(Up.Size, crc);
}

void Abort()
{
    if (Up.Active && Up.Type == UploadType::Esp32Firmware) {
        esp_ota_abort(Ota);
    }
    Up.Active = false;
}

}
