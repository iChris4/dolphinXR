// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_GUEST_MEMORY_H
#define GXRUNTIME_GUEST_MEMORY_H

#include "core/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DolGuestMemoryConfig {
    u32 vm_base;
    u32 vm_size;
    u32 locked_cache_base;
    u32 locked_cache_size;
} DolGuestMemoryConfig;

typedef struct DolGuestMemory {
    DolGuestMemoryConfig config;
    u8* vm;
    u8* locked_cache;
} DolGuestMemory;

typedef enum DolGuestAddressSpace {
    DOL_GUEST_ADDRESS_AUTO = 0,
    DOL_GUEST_ADDRESS_VIRTUAL,
    DOL_GUEST_ADDRESS_PHYSICAL,
} DolGuestAddressSpace;

typedef enum DolGuestResourceKind {
    DOL_GUEST_RESOURCE_GENERIC = 0,
    DOL_GUEST_RESOURCE_FIFO,
    DOL_GUEST_RESOURCE_DISPLAY_LIST,
    DOL_GUEST_RESOURCE_VERTEX_ARRAY,
    DOL_GUEST_RESOURCE_TEXTURE,
    DOL_GUEST_RESOURCE_TLUT,
    DOL_GUEST_RESOURCE_COPY_DESTINATION,
} DolGuestResourceKind;

typedef struct DolGuestResolvedRange {
    void* data;
    u32 address;
    u32 size;
    u32 available;
    DolGuestAddressSpace space;
    DolGuestResourceKind resource;
} DolGuestResolvedRange;

typedef bool (*DolGuestAddressResolveFn)(
    void* user, u32 address, u32 size, DolGuestAddressSpace space,
    DolGuestResourceKind resource, DolGuestResolvedRange* out);

typedef struct DolGuestAddressResolver {
    DolGuestMemory* memory;
    CPUState* cpu;
    DolGuestAddressResolveFn resolve;
    void* user;
} DolGuestAddressResolver;

DolGuestMemoryConfig dol_guest_memory_default_config(void);
bool dol_guest_memory_init(DolGuestMemory* memory,
                           const DolGuestMemoryConfig* config);
void dol_guest_memory_shutdown(DolGuestMemory* memory);

bool dol_guest_memory_read(const DolGuestMemory* memory, u32 address, u8 size,
                           u64* value);
bool dol_guest_memory_write(DolGuestMemory* memory, u32 address, u8 size,
                            u64 value);

void* dol_guest_memory_pointer(DolGuestMemory* memory, CPUState* cpu,
                               u32 address, u32* available);
void dol_guest_memory_copy(DolGuestMemory* memory, CPUState* cpu, u32 dest,
                           u32 src, u32 bytes);

void dol_guest_address_resolver_init(DolGuestAddressResolver* resolver,
                                     DolGuestMemory* memory, CPUState* cpu);
void dol_guest_address_resolver_init_callback(
    DolGuestAddressResolver* resolver, DolGuestAddressResolveFn resolve,
    void* user);
bool dol_guest_address_resolver_resolve(
    const DolGuestAddressResolver* resolver, u32 address, u32 size,
    DolGuestAddressSpace space, DolGuestResourceKind resource,
    DolGuestResolvedRange* out);

#ifdef __cplusplus
}
#endif

#endif
