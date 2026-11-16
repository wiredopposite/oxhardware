/*
 * The programmer protocol's client for C and anything that binds to C: the
 * same calls as OxProg::Client, over a transport given as callbacks. Link
 * oxprogc.
 *
 * Every call returns an oxprog_status_t: OXPROG_OK, a status from the chip,
 * or OXPROG_TIMEOUT / OXPROG_TRANSPORT_ERROR from the client. Commands are sent
 * one at a time; a client is used from one thread.
 */
#ifndef OXPROG_H
#define OXPROG_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(OXPROG_SHARED)
#  if defined(OXPROG_BUILDING)
#    define OXPROG_API __declspec(dllexport)
#  else
#    define OXPROG_API __declspec(dllimport)
#  endif
#elif defined(OXPROG_SHARED) && defined(OXPROG_BUILDING)
#  define OXPROG_API __attribute__((visibility("default")))
#else
#  define OXPROG_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define OXPROG_NAME_MAX 32
#define OXPROG_NO_ENTRY 0xFF

typedef uint8_t oxprog_status_t;
enum {
    OXPROG_OK = 0,
    OXPROG_CONSOLE_ON,
    OXPROG_PROGRAMMER_BUSY,
    OXPROG_LAYOUT_INVALID,
    OXPROG_NOT_STARTED,
    OXPROG_BAD_SIZE,
    OXPROG_BAD_OFFSET,
    OXPROG_BAD_COMMAND,
    OXPROG_FLASH_ERROR,
    OXPROG_CRC_MISMATCH,
    OXPROG_NOT_FOUND,
    OXPROG_PROTECTED,
    OXPROG_NO_SPACE,
    OXPROG_CONSOLE_OFF,
    OXPROG_SMBUS_ERROR,
    OXPROG_FRAGMENTED,
    OXPROG_TIMEOUT = 0xFE,
    OXPROG_TRANSPORT_ERROR = 0xFF
};

enum {
    OXPROG_ROLE_USER = 0,
    OXPROG_ROLE_LOADER,
    OXPROG_ROLE_SYSTEM,
    OXPROG_ROLE_DIAG
};

enum {
    OXPROG_UPLOAD_RETAIL_TABLES = 0,
    OXPROG_UPLOAD_USER_ROM,
    OXPROG_UPLOAD_LOADER_ROM,
    OXPROG_UPLOAD_SYSTEM_ROM,
    OXPROG_UPLOAD_DIAG_ROM,
    OXPROG_UPLOAD_ESP32_FIRMWARE,   /* the chip restarts after the commit */
    OXPROG_UPLOAD_FPGA_BITSTREAM    /* the FPGA reloads during the commit */
};

enum {
    OXPROG_DOWNLOAD_RETAIL_TABLES = 0,
    OXPROG_DOWNLOAD_ROM,
    OXPROG_DOWNLOAD_ACTIVE_ROM
};

#define OXPROG_COMMIT_TRIAL 0x01    /* Loader, System: current only once it confirms */
#define OXPROG_COMMIT_USER  0x02    /* user ROM: make it the power-button ROM */
#define OXPROG_COMMIT_BOOT  0x04    /* boot it from the console's next reset */

#define OXPROG_ROM_CURRENT     0x01
#define OXPROG_ROM_TRIAL       0x02
#define OXPROG_ROM_FAILED_BOOT 0x04
#define OXPROG_ROM_USER        0x08

#define OXPROG_BOOT_RESET 0x01      /* reset the console through the SMC once armed */

#define OXPROG_FPGA_ARMED      0x01
#define OXPROG_FPGA_ARMED_BOOT 0x02
#define OXPROG_FPGA_TRIAL      0x04
#define OXPROG_FPGA_LAST_FAILED 0x08
#define OXPROG_FPGA_CONFIRMED  0x10

#pragma pack(push, 1)

typedef struct {
    uint16_t version;
    uint8_t  firmware_version[3];
    uint8_t  fpga_version[3];
    uint8_t  board_id;
    uint8_t  console_powered;
    uint8_t  boot_flags;
    uint8_t  active_entry;
    uint8_t  armed_entry;
    uint8_t  user_entry;
    uint8_t  loader_entry;
    uint8_t  system_entry;
    uint8_t  diag_entry;
    uint8_t  entry_count;
    uint8_t  slot_count;
    uint8_t  free_slots;
    uint32_t flash_size;
    uint32_t retail_tables_size;    /* 0 if none stored */
    uint32_t retail_tables_source_crc32;
} oxprog_info_t;

typedef struct {
    uint8_t  id;
    uint8_t  role;
    uint8_t  bank;                  /* bits 5:0 slot, 7:6 size */
    uint8_t  flags;
    uint32_t size;
    uint32_t crc32;
    uint32_t source_crc32;
    uint16_t version;
    uint16_t reserved;
    char     name[OXPROG_NAME_MAX + 1];
} oxprog_rom_entry_t;

#pragma pack(pop)

typedef struct {
    char     name[OXPROG_NAME_MAX + 1];
    uint16_t version;
    uint32_t source_crc32;
    uint8_t  flags;
    uint8_t  force;                 /* write with the console running another BIOS */
} oxprog_upload_options_t;

/*
 * A byte stream to the chip. write: everything or nothing, returns nonzero on
 * success. read: whatever arrives within timeout_ms, up to len; 0 on timeout.
 * discard (may be NULL): drop anything received but not read.
 */
typedef struct {
    void *ctx;
    int (*write)(void *ctx, const void *data, size_t len);
    size_t (*read)(void *ctx, void *data, size_t len, uint32_t timeout_ms);
    void (*discard)(void *ctx);
} oxprog_transport_t;

/* Done and total bytes */
typedef void (*oxprog_progress_fn)(void *ctx, uint32_t done, uint32_t total);

typedef struct oxprog_client oxprog_client_t;

/* The transport is copied; its ctx must stay valid. NULL if out of memory. */
OXPROG_API oxprog_client_t *oxprog_client_new(const oxprog_transport_t *transport,
                                              uint32_t timeout_ms);
OXPROG_API void oxprog_client_free(oxprog_client_t *client);
/* How long a command waits for its reply (commits always get longer) */
OXPROG_API void oxprog_set_timeout(oxprog_client_t *client, uint32_t timeout_ms);

OXPROG_API oxprog_status_t oxprog_get_info(oxprog_client_t *client, oxprog_info_t *out);
OXPROG_API oxprog_status_t oxprog_reboot(oxprog_client_t *client);
OXPROG_API oxprog_status_t oxprog_smbus_read(oxprog_client_t *client, uint8_t address,
                                             uint8_t command, uint8_t *value);
OXPROG_API oxprog_status_t oxprog_smbus_write(oxprog_client_t *client, uint8_t address,
                                              uint8_t command, uint8_t value);
OXPROG_API oxprog_status_t oxprog_boot(oxprog_client_t *client, uint8_t entry, uint8_t flags);
OXPROG_API oxprog_status_t oxprog_confirm_boot(oxprog_client_t *client);

OXPROG_API oxprog_status_t oxprog_list_roms(oxprog_client_t *client, oxprog_rom_entry_t *out,
                                            size_t max, size_t *count);
OXPROG_API oxprog_status_t oxprog_get_rom(oxprog_client_t *client, uint8_t entry,
                                          oxprog_rom_entry_t *out);
OXPROG_API oxprog_status_t oxprog_delete_rom(oxprog_client_t *client, uint8_t entry);
OXPROG_API oxprog_status_t oxprog_rename_rom(oxprog_client_t *client, uint8_t entry,
                                             const char *name);
OXPROG_API oxprog_status_t oxprog_set_user_rom(oxprog_client_t *client, uint8_t entry);
OXPROG_API oxprog_status_t oxprog_compact_roms(oxprog_client_t *client, uint32_t image_size);

/*
 * Begin, the image in chunks, then commit with its CRC; aborted on the chip if
 * anything fails part way. options, progress and entry may be NULL. For the ROM
 * types, entry gets the new catalogue entry.
 */
OXPROG_API oxprog_status_t oxprog_upload(oxprog_client_t *client, uint8_t type,
                                         const uint8_t *image, size_t len,
                                         const oxprog_upload_options_t *options,
                                         oxprog_progress_fn progress, void *progress_ctx,
                                         oxprog_rom_entry_t *entry);
OXPROG_API oxprog_status_t oxprog_abort_upload(oxprog_client_t *client);

/*
 * The whole image, checked against the chip's CRC, into a buffer the library
 * allocates: free it with oxprog_free(). entry is for OXPROG_DOWNLOAD_ROM.
 */
OXPROG_API oxprog_status_t oxprog_download(oxprog_client_t *client, uint8_t type, uint8_t entry,
                                           uint8_t **out, size_t *len,
                                           oxprog_progress_fn progress, void *progress_ctx);
OXPROG_API void oxprog_free(void *data);

/* CRC-32 IEEE (zlib); seed a previous result to continue, 0 to start */
OXPROG_API uint32_t oxprog_crc32(const void *data, size_t len, uint32_t seed);
OXPROG_API const char *oxprog_status_name(oxprog_status_t status);

#ifdef __cplusplus
}
#endif

#endif
