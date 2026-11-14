// The C API (oxprog/oxprog.h) over OxProg::Client

#define OXPROG_BUILDING
#include "oxprog/oxprog.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "oxprog/client.hpp"

using namespace OxProg;

// The C structs are the protocol's, byte for byte
static_assert(sizeof(oxprog_info_t) == sizeof(Info));
static_assert(offsetof(oxprog_info_t, retail_tables_source_crc32) ==
              offsetof(Info, RetailTablesSourceCrc32));
static_assert(sizeof(oxprog_rom_entry_t) == sizeof(RomEntry));
static_assert(offsetof(oxprog_rom_entry_t, name) == offsetof(RomEntry, Name));
static_assert(OXPROG_NAME_MAX == NameMax);
static_assert(OXPROG_NO_ENTRY == NoEntry);
static_assert(OXPROG_FRAGMENTED == uint8_t(Status::Fragmented));
static_assert(OXPROG_TIMEOUT == uint8_t(Status::Timeout));
static_assert(OXPROG_UPLOAD_FPGA_BITSTREAM == uint8_t(UploadType::FpgaBitstream));
static_assert(OXPROG_UPLOAD_FPGA_BITSTREAM + 1 == uint8_t(UploadType::Count));
static_assert(OXPROG_DOWNLOAD_ACTIVE_ROM + 1 == uint8_t(DownloadType::Count));
static_assert(OXPROG_COMMIT_BOOT == CommitBoot && OXPROG_ROM_USER == RomUser);
static_assert(OXPROG_BOOT_RESET == BootReset && OXPROG_FPGA_CONFIRMED == FpgaConfirmed);

namespace {

class CallbackTransport : public Transport {
public:
    explicit CallbackTransport(const oxprog_transport_t &t) : T(t) { }

    bool Write(const void *Data, size_t Length) override
    {
        return T.write != nullptr && T.write(T.ctx, Data, Length) != 0;
    }
    size_t Read(void *Data, size_t Length, uint32_t TimeoutMs) override
    {
        return T.read != nullptr ? T.read(T.ctx, Data, Length, TimeoutMs) : 0;
    }
    void Discard() override
    {
        if (T.discard != nullptr) {
            T.discard(T.ctx);
        }
    }

private:
    oxprog_transport_t T;
};

Progress MakeProgress(oxprog_progress_fn fn, void *ctx)
{
    if (fn == nullptr) {
        return {};
    }
    return [fn, ctx](uint32_t Done, uint32_t Total) { fn(ctx, Done, Total); };
}

oxprog_status_t S(Status s) { return oxprog_status_t(s); }

}

struct oxprog_client {
    CallbackTransport Link;
    Client C;

    oxprog_client(const oxprog_transport_t &t, uint32_t timeout_ms)
        : Link(t), C(Link, timeout_ms) { }
};

extern "C" {

oxprog_client_t *oxprog_client_new(const oxprog_transport_t *transport, uint32_t timeout_ms)
{
    if (transport == nullptr) {
        return nullptr;
    }
    return new (std::nothrow) oxprog_client(*transport, timeout_ms ? timeout_ms : 5000);
}

void oxprog_client_free(oxprog_client_t *client)
{
    delete client;
}

void oxprog_set_timeout(oxprog_client_t *client, uint32_t timeout_ms)
{
    client->C.SetTimeout(timeout_ms);
}

oxprog_status_t oxprog_get_info(oxprog_client_t *client, oxprog_info_t *out)
{
    Info info{};
    const Status s = client->C.GetInfo(info);
    if (s == Status::Ok) {
        memcpy(out, &info, sizeof(info));
    }
    return S(s);
}

oxprog_status_t oxprog_reboot(oxprog_client_t *client)
{
    return S(client->C.Reboot());
}

oxprog_status_t oxprog_smbus_read(oxprog_client_t *client, uint8_t address, uint8_t command,
                                  uint8_t *value)
{
    return S(client->C.SmbusRead(address, command, *value));
}

oxprog_status_t oxprog_smbus_write(oxprog_client_t *client, uint8_t address, uint8_t command,
                                   uint8_t value)
{
    return S(client->C.SmbusWrite(address, command, value));
}

oxprog_status_t oxprog_boot(oxprog_client_t *client, uint8_t entry, uint8_t flags)
{
    return S(client->C.Boot(entry, flags));
}

oxprog_status_t oxprog_confirm_boot(oxprog_client_t *client)
{
    return S(client->C.ConfirmBoot());
}

oxprog_status_t oxprog_list_roms(oxprog_client_t *client, oxprog_rom_entry_t *out, size_t max,
                                 size_t *count)
{
    std::vector<RomEntry> roms;
    const Status s = client->C.ListRoms(roms);
    const size_t n = roms.size() < max ? roms.size() : max;
    if (s == Status::Ok && n) {
        memcpy(out, roms.data(), n * sizeof(RomEntry));
    }
    if (count != nullptr) {
        *count = s == Status::Ok ? n : 0;
    }
    return S(s);
}

oxprog_status_t oxprog_get_rom(oxprog_client_t *client, uint8_t entry, oxprog_rom_entry_t *out)
{
    RomEntry e{};
    const Status s = client->C.GetRom(entry, e);
    if (s == Status::Ok) {
        memcpy(out, &e, sizeof(e));
    }
    return S(s);
}

oxprog_status_t oxprog_delete_rom(oxprog_client_t *client, uint8_t entry)
{
    return S(client->C.DeleteRom(entry));
}

oxprog_status_t oxprog_rename_rom(oxprog_client_t *client, uint8_t entry, const char *name)
{
    return S(client->C.RenameRom(entry, name));
}

oxprog_status_t oxprog_set_user_rom(oxprog_client_t *client, uint8_t entry)
{
    return S(client->C.SetUserRom(entry));
}

oxprog_status_t oxprog_compact_roms(oxprog_client_t *client, uint32_t image_size)
{
    return S(client->C.CompactRoms(image_size));
}

oxprog_status_t oxprog_upload(oxprog_client_t *client, uint8_t type, const uint8_t *image,
                              size_t len, const oxprog_upload_options_t *options,
                              oxprog_progress_fn progress, void *progress_ctx,
                              oxprog_rom_entry_t *entry)
{
    if (type >= uint8_t(UploadType::Count) || (image == nullptr && len != 0)) {
        return OXPROG_BAD_COMMAND;
    }
    UploadOptions o;
    if (options != nullptr) {
        memcpy(o.Name, options->name, sizeof(o.Name));
        o.Name[NameMax] = '\0';
        o.Version = options->version;
        o.SourceCrc32 = options->source_crc32;
        o.Flags = options->flags;
        o.Force = options->force != 0;
    }
    RomEntry e{};
    const Status s = client->C.Upload(UploadType(type), std::span<const uint8_t>(image, len), o,
                                      MakeProgress(progress, progress_ctx),
                                      entry != nullptr ? &e : nullptr);
    if (s == Status::Ok && entry != nullptr) {
        memcpy(entry, &e, sizeof(e));
    }
    return S(s);
}

oxprog_status_t oxprog_abort_upload(oxprog_client_t *client)
{
    return S(client->C.AbortUpload());
}

oxprog_status_t oxprog_download(oxprog_client_t *client, uint8_t type, uint8_t entry,
                                uint8_t **out, size_t *len, oxprog_progress_fn progress,
                                void *progress_ctx)
{
    *out = nullptr;
    *len = 0;
    if (type >= uint8_t(DownloadType::Count)) {
        return OXPROG_BAD_COMMAND;
    }
    std::vector<uint8_t> data;
    const Status s = client->C.Download(DownloadType(type), data, entry,
                                        MakeProgress(progress, progress_ctx));
    if (s != Status::Ok) {
        return S(s);
    }
    *out = static_cast<uint8_t *>(malloc(data.empty() ? 1 : data.size()));
    if (*out == nullptr) {
        return OXPROG_TRANSPORT_ERROR;
    }
    memcpy(*out, data.data(), data.size());
    *len = data.size();
    return OXPROG_OK;
}

void oxprog_free(void *data)
{
    free(data);
}

uint32_t oxprog_crc32(const void *data, size_t len, uint32_t seed)
{
    return Crc32(data, len, seed);
}

const char *oxprog_status_name(oxprog_status_t status)
{
    return StatusName(Status(status));
}

}
