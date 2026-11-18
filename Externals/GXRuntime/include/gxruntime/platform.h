// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_PLATFORM_H
#define GXRUNTIME_PLATFORM_H

#include "core/types.h"
#include "gxruntime/guest_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DolPadState {
    u16 button;
    s8 stick_x;
    s8 stick_y;
    s8 substick_x;
    s8 substick_y;
    u8 trigger_left;
    u8 trigger_right;
    u8 analog_a;
    u8 analog_b;
    s8 error;
} DolPadState;

typedef bool (*DolPlatformGuestAddressResolverFn)(
    void* user, u32 address, u32 size, DolGuestAddressSpace space,
    DolGuestResourceKind resource, const void** data, u32* available);

typedef struct DolPlatformOps {
    bool (*should_quit)(void);
    void (*present)(void);

    void (*mark_gx_begin)(void);
    void (*gx_write)(u64 value, u8 size);
    void (*call_display_list)(const void* data, u32 size);
    void (*set_array)(u32 attr, const void* data, u32 size, u8 stride);
    void (*set_array_guest)(u32 attr, u32 guest_address, const void* data,
                            u32 size, u8 stride);
    void (*load_texture)(u8 slot, const void* data, u32 width, u32 height,
                         u32 format, u32 tlut, bool mipmap, u32 object_id,
                         u32 data_version);
    void (*load_texture_guest)(u8 slot, u32 guest_address, const void* data,
                               u32 width, u32 height, u32 format, u32 tlut,
                               bool mipmap, u32 object_id, u32 data_version);
    void (*load_tlut)(u8 slot, const void* data, u32 format, u16 entries,
                      u32 object_id, u32 data_version);
    void (*load_tlut_guest)(u8 slot, u32 guest_address, const void* data,
                            u32 format, u16 entries, u32 object_id,
                            u32 data_version);
    void (*set_copy_destination)(const void* data);
    void (*set_copy_destination_guest)(u32 guest_address, const void* data);
    void (*set_guest_address_resolver)(
        DolPlatformGuestAddressResolverFn resolve, void* user);
    void (*configure_vi)(u32 tv_mode, u16 fb_width, u16 efb_height,
                         u16 xfb_height, u16 vi_width, u16 vi_height);

    bool (*pad_init)(void);
    u32 (*pad_read)(DolPadState state[4]);
    bool (*pad_reset)(u32 mask);
    bool (*pad_recalibrate)(u32 mask);
    void (*pad_control_motor)(u32 channel, u32 command);
    void (*pad_set_spec)(u32 spec);

    void (*audio_set_sample_rate)(u32 sample_rate);
    void (*audio_push)(const s16* samples, u32 frames);
} DolPlatformOps;

void dol_platform_install(const DolPlatformOps* ops);
void dol_platform_reset(void);
bool dol_platform_available(void);

bool dol_platform_should_quit(void);
void dol_platform_present(void);
void dol_platform_mark_gx_begin(void);
void dol_platform_gx_write(u64 value, u8 size);
void dol_platform_call_display_list(const void* data, u32 size);
void dol_platform_set_array(u32 attr, const void* data, u32 size, u8 stride);
void dol_platform_set_array_guest(u32 attr, u32 guest_address,
                                  const void* data, u32 size, u8 stride);
void dol_platform_load_texture(u8 slot, const void* data, u32 width, u32 height,
                               u32 format, u32 tlut, bool mipmap,
                               u32 object_id, u32 data_version);
void dol_platform_load_texture_guest(u8 slot, u32 guest_address,
                                     const void* data, u32 width, u32 height,
                                     u32 format, u32 tlut, bool mipmap,
                                     u32 object_id, u32 data_version);
void dol_platform_load_tlut(u8 slot, const void* data, u32 format, u16 entries,
                            u32 object_id, u32 data_version);
void dol_platform_load_tlut_guest(u8 slot, u32 guest_address,
                                  const void* data, u32 format, u16 entries,
                                  u32 object_id, u32 data_version);
void dol_platform_set_copy_destination(const void* data);
void dol_platform_set_copy_destination_guest(u32 guest_address,
                                             const void* data);
void dol_platform_set_guest_address_resolver(
    DolPlatformGuestAddressResolverFn resolve, void* user);
void dol_platform_configure_vi(u32 tv_mode, u16 fb_width, u16 efb_height,
                               u16 xfb_height, u16 vi_width, u16 vi_height);

bool dol_platform_pad_init(void);
u32 dol_platform_pad_read(DolPadState state[4]);
bool dol_platform_pad_reset(u32 mask);
bool dol_platform_pad_recalibrate(u32 mask);
void dol_platform_pad_control_motor(u32 channel, u32 command);
void dol_platform_pad_set_spec(u32 spec);

void dol_platform_audio_set_sample_rate(u32 sample_rate);
void dol_platform_audio_push(const s16* samples, u32 frames);

#ifdef __cplusplus
}
#endif

#endif
