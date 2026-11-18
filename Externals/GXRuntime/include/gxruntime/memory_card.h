// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_MEMORY_CARD_H
#define GXRUNTIME_MEMORY_CARD_H

#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// GameCube memory-card geometry and SDK-compatible result values. A 4-Mbit
// card has 64 physical 8 KiB blocks, five of which are filesystem metadata,
// leaving the familiar 59 user blocks.
#define DOL_CARD_SECTOR_SIZE 8192u
#define DOL_CARD_SYSTEM_BLOCKS 5u
#define DOL_CARD_MAX_FILES 127u
#define DOL_CARD_FILENAME_MAX 32u

typedef enum DolCardResult {
    DOL_CARD_RESULT_UNLOCKED = 1,
    DOL_CARD_RESULT_READY = 0,
    DOL_CARD_RESULT_BUSY = -1,
    DOL_CARD_RESULT_WRONG_DEVICE = -2,
    DOL_CARD_RESULT_NO_CARD = -3,
    DOL_CARD_RESULT_NO_FILE = -4,
    DOL_CARD_RESULT_IO_ERROR = -5,
    DOL_CARD_RESULT_BROKEN = -6,
    DOL_CARD_RESULT_EXISTS = -7,
    DOL_CARD_RESULT_NO_ENTRY = -8,
    DOL_CARD_RESULT_INSUFFICIENT_SPACE = -9,
    DOL_CARD_RESULT_NO_PERMISSION = -10,
    DOL_CARD_RESULT_LIMIT = -11,
    DOL_CARD_RESULT_NAME_TOO_LONG = -12,
    DOL_CARD_RESULT_ENCODING = -13,
    DOL_CARD_RESULT_CANCELED = -14,
    DOL_CARD_RESULT_FATAL = -128,
} DolCardResult;

typedef struct DolMemoryCard DolMemoryCard;

typedef struct DolMemoryCardConfig {
    // Explicit host path for the portable GXRuntime card container. NULL or
    // empty creates a non-persistent in-memory card. Path/default policy
    // belongs to the game executable.
    const char* path;

    // Valid GameCube sizes are 4, 8, 16, 32, 64, and 128 Mbits. Zero selects
    // the standard 4-Mbit (59 user block) card.
    u16 size_mbits;
    u16 encoding;

    // Identity assigned to newly created files and used to scope name lookup.
    u8 game_code[4];
    u8 company[2];

    // Stable card serial. Zero generates one when a new container is created;
    // an existing container always retains its serialized serial.
    u64 serial;
} DolMemoryCardConfig;

typedef struct DolMemoryCardStat {
    char file_name[DOL_CARD_FILENAME_MAX + 1u];
    u32 length;
    u32 time;
    u8 game_code[4];
    u8 company[2];
    u8 banner_format;
    u8 permission;
    u32 icon_address;
    u16 icon_format;
    u16 icon_speed;
    u32 comment_address;
} DolMemoryCardStat;

// Opens an existing container or creates a formatted empty card. Existing
// malformed containers are rejected rather than overwritten.
DolMemoryCard* dol_card_open(const DolMemoryCardConfig* config);
void dol_card_close(DolMemoryCard* card);

DolCardResult dol_card_probe(DolMemoryCard* card, u16* size_mbits,
                             u32* sector_size);
DolCardResult dol_card_mount(DolMemoryCard* card);
DolCardResult dol_card_unmount(DolMemoryCard* card);
DolCardResult dol_card_check(DolMemoryCard* card);
DolCardResult dol_card_format(DolMemoryCard* card);

DolCardResult dol_card_free_space(DolMemoryCard* card, u32* bytes_free,
                                  u32* files_free);
DolCardResult dol_card_open_file(DolMemoryCard* card, const char* name,
                                 s32* file_no, u32* length);
DolCardResult dol_card_create_file(DolMemoryCard* card, const char* name,
                                   u32 length, s32* file_no);
DolCardResult dol_card_close_file(DolMemoryCard* card, s32 file_no);
DolCardResult dol_card_delete_file(DolMemoryCard* card, const char* name);
DolCardResult dol_card_read_file(DolMemoryCard* card, s32 file_no, u32 offset,
                                 void* data, u32 length);
DolCardResult dol_card_write_file(DolMemoryCard* card, s32 file_no, u32 offset,
                                  const void* data, u32 length);
DolCardResult dol_card_get_status(DolMemoryCard* card, s32 file_no,
                                  DolMemoryCardStat* stat);
DolCardResult dol_card_set_status(DolMemoryCard* card, s32 file_no,
                                  const DolMemoryCardStat* stat);

u64 dol_card_serial(const DolMemoryCard* card);
s32 dol_card_last_result(const DolMemoryCard* card);
u32 dol_card_transferred_bytes(const DolMemoryCard* card);

#ifdef __cplusplus
}
#endif

#endif
