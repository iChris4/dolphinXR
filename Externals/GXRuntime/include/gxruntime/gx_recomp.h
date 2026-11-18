// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_GX_RECOMP_H
#define GXRUNTIME_GX_RECOMP_H

#include "gxruntime/guest_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOL_GX_RECOMP_MAX_FIFO_BYTES 262144u
#define DOL_GX_RECOMP_CP_ARRAY_COUNT 16u
#define DOL_GX_RECOMP_TEXTURE_SLOTS 8u
#define DOL_GX_RECOMP_TLUT_SLOTS 16u
#define DOL_GX_RECOMP_TMEM_TLUT_SLOTS 1024u
#define DOL_GX_RECOMP_MAX_TRACE_EVENTS 8192u
#define DOL_GX_RECOMP_VERTEX_FORMATS 8u
#define DOL_GX_RECOMP_MAX_INDEXED_ATTR_SPECS 24u
#define DOL_GX_RECOMP_POSITION_MATRIX_COUNT 10u
#define DOL_GX_RECOMP_POSITION_MATRIX_WORDS 12u
// Normal matrices (XF 0x400..0x45F): 3x3 stored as 3 rows of 3, indexed 9 words
// per matrix (Dolphin normalMatrices[3*(PosNormalMtxIdx&31)]). COUNT 11 covers
// the 96-word region (0x460-0x400) with room for the last partial slot.
#define DOL_GX_RECOMP_NORMAL_MATRIX_COUNT 11u
#define DOL_GX_RECOMP_NORMAL_MATRIX_WORDS 9u
#define DOL_GX_RECOMP_LIGHT_COUNT 8u
#define DOL_GX_RECOMP_LIGHT_WORDS 16u
#define DOL_GX_RECOMP_CHAN_REG_COUNT 9u
#define DOL_GX_RECOMP_TEX_MATRIX_COUNT 11u
#define DOL_GX_RECOMP_TEX_MATRIX_WORDS 12u
#define DOL_GX_RECOMP_XF_REG_COUNT 0x40u
#define DOL_GX_PHYSICAL_ADDRESS_MASK 0x1FFFFFFFu

#define DOL_GX_CMD_LOAD_CP_REG 0x08u
#define DOL_GX_CMD_LOAD_XF_REG 0x10u
#define DOL_GX_CMD_LOAD_BP_REG 0x61u
#define DOL_GX_CMD_CALL_DL 0x40u
#define DOL_GX_CMD_INVL_VC 0x48u
#define DOL_GX_CP_REG_MATRIX_INDEX_A 0x30u
#define DOL_GX_CP_REG_MATRIX_INDEX_B 0x40u
#define DOL_GX_CP_REG_VCD_LO 0x50u
#define DOL_GX_CP_REG_VCD_HI 0x60u
#define DOL_GX_CP_REG_VAT_GRP0 0x70u
#define DOL_GX_CP_REG_VAT_GRP1 0x80u
#define DOL_GX_CP_REG_VAT_GRP2 0x90u
#define DOL_GX_CP_REG_ARRAYBASE 0xA0u
#define DOL_GX_CP_REG_ARRAYSTRIDE 0xB0u
#define DOL_GX_BP_REG_GENMODE 0x00u
/* PE control (GXSetPixelFmt/GXSetZCompLoc). Bits 0-2 = EFB pixel format; the
 * value Z24 (3) marks the frame's EFB as a depth surface, so a copy trigger
 * while this is set is a Z-source copy (Dolphin BPMemory.h PEControl @ 0x43,
 * BPStructs.cpp: is_depth_copy = zcontrol.pixel_format == PixelFormat::Z24). */
#define DOL_GX_BP_REG_PE_CONTROL 0x43u
#define DOL_GX_PE_PIXEL_FORMAT_Z24 0x3u
#define DOL_GX_BP_REG_EFB_TL 0x49u
#define DOL_GX_BP_REG_EFB_WH 0x4Au
#define DOL_GX_BP_REG_EFB_ADDR 0x4Bu
#define DOL_GX_BP_REG_TRIGGER_EFB_COPY 0x52u
#define DOL_GX_BP_REG_LOAD_TLUT0 0x64u
#define DOL_GX_BP_REG_LOAD_TLUT1 0x65u
#define DOL_GX_BP_REG_TX_SETIMAGE0 0x88u
#define DOL_GX_BP_REG_TX_SETIMAGE3 0x94u
#define DOL_GX_BP_REG_TX_SETTLUT 0x98u
#define DOL_GX_BP_REG_TX_SETIMAGE0_4 0xA8u
#define DOL_GX_BP_REG_TX_SETIMAGE3_4 0xB4u
#define DOL_GX_BP_REG_TX_SETTLUT_4 0xB8u

typedef enum DolGxRecompEventKind {
    DOL_GX_RECOMP_EVENT_FIFO_BYTES = 1,
    DOL_GX_RECOMP_EVENT_DISPLAY_LIST,
    DOL_GX_RECOMP_EVENT_CP_ARRAY_BASE,
    DOL_GX_RECOMP_EVENT_CP_ARRAY_STRIDE,
    DOL_GX_RECOMP_EVENT_INDEXED_SPAN,
    DOL_GX_RECOMP_EVENT_TEXTURE,
    DOL_GX_RECOMP_EVENT_TLUT,
    DOL_GX_RECOMP_EVENT_COPY_DESTINATION,
    DOL_GX_RECOMP_EVENT_BP_REG,
    DOL_GX_RECOMP_EVENT_XF_LOAD,
    DOL_GX_RECOMP_EVENT_CULL_ALL,
    DOL_GX_RECOMP_EVENT_DRAW,
    DOL_GX_RECOMP_EVENT_INDEXED_XF_LOAD,
    DOL_GX_RECOMP_EVENT_CP_VCD,
    DOL_GX_RECOMP_EVENT_CP_VAT,
    DOL_GX_RECOMP_EVENT_VERTEX_LAYOUT,
    DOL_GX_RECOMP_EVENT_INVALIDATE_VTX_CACHE,
} DolGxRecompEventKind;

typedef struct DolGxRecompTraceEvent {
    DolGxRecompEventKind kind;
    u32 a;
    u32 b;
    u32 c;
    u32 d;
    /* Extra payload fields. INDEXED_SPAN uses them to carry the per-attr decode
     * params an issuing sink needs to assemble vertices: e=vertex_offset (index
     * position within the per-vertex stride), f=index_size (1/2), g=element_size
     * (bytes fetched from the resolved array at base+index*stride). Other events
     * leave them zero. */
    u32 e;
    u32 f;
    u32 g;
    /* TEXTURE event, CI formats (C4/C8/C14X2) only: the resolved TLUT this
     * texture indexes. tlut_address = palette physical base, tlut_format = TLUT
     * pixel format (IA8=0/RGB565=1/RGB5A3=2), tlut_entries = palette entry count.
     * All zero for non-CI textures and CI textures whose TLUT is not yet
     * resolved. In-memory only (never serialized into .dolt) A2. */
    u32 tlut_address;
    u32 tlut_format;
    u32 tlut_entries;
} DolGxRecompTraceEvent;

typedef struct DolGxRecompFifo {
    u8 bytes[DOL_GX_RECOMP_MAX_FIFO_BYTES];
    u32 size;
} DolGxRecompFifo;

typedef struct DolGxRecompCpArray {
    bool base_valid;
    bool stride_valid;
    u32 physical_base;
    u32 stride;
} DolGxRecompCpArray;

typedef struct DolGxRecompResolvedArray {
    DolGuestResolvedRange range;
    u8 attr;
    u32 stride;
    u32 byte_span;
} DolGxRecompResolvedArray;

typedef struct DolGxRecompTexture {
    bool valid;
    u8 slot;
    u16 width;
    u16 height;
    u32 format;
    u32 physical_base;
    u32 byte_size;
    DolGuestResolvedRange range;
} DolGxRecompTexture;

typedef struct DolGxRecompTlut {
    bool valid;
    u8 slot;
    u16 tmem_offset;
    u32 format;
    u16 entries;
    u32 physical_base;
    u32 byte_size;
    DolGuestResolvedRange range;
} DolGxRecompTlut;

typedef struct DolGxRecompIndexedAttr {
    bool valid;
    u8 attr;
    u32 vertex_offset;
    u8 index_size;
    u32 element_size;
    u32 element_bias;
} DolGxRecompIndexedAttr;

typedef struct DolGxRecompVertexLayout {
    bool valid;
    u32 vertex_size;
    DolGxRecompIndexedAttr attrs[DOL_GX_RECOMP_CP_ARRAY_COUNT];
    DolGxRecompIndexedAttr indexed_attrs[DOL_GX_RECOMP_MAX_INDEXED_ATTR_SPECS];
    u32 indexed_attr_count;
} DolGxRecompVertexLayout;

typedef struct DolGxRecompCopyState {
    u32 src_x;
    u32 src_y;
    u32 width;
    u32 height;
    u32 physical_base;
    bool physical_base_valid;
    u32 format;
    u32 byte_size;
    u32 clear; /* BP 0x52 trigger bit 11: EFB cleared after the copy */
    bool is_depth; /* PE_CONTROL pixel format == Z24 at trigger: Z-source copy */
    DolGuestResolvedRange range;
    bool range_valid;
} DolGxRecompCopyState;

typedef struct DolGxRecompState {
    DolGuestAddressResolver resolver;
    DolGxRecompFifo fifo;
    DolGxRecompCpArray arrays[DOL_GX_RECOMP_CP_ARRAY_COUNT];
    u32 vcd_lo;
    u32 vcd_hi;
    bool vcd_lo_valid;
    bool vcd_hi_valid;
    u32 vat_group0[DOL_GX_RECOMP_VERTEX_FORMATS];
    u32 vat_group1[DOL_GX_RECOMP_VERTEX_FORMATS];
    u32 vat_group2[DOL_GX_RECOMP_VERTEX_FORMATS];
    bool vat_group0_valid[DOL_GX_RECOMP_VERTEX_FORMATS];
    bool vat_group1_valid[DOL_GX_RECOMP_VERTEX_FORMATS];
    bool vat_group2_valid[DOL_GX_RECOMP_VERTEX_FORMATS];
    DolGxRecompTexture textures[DOL_GX_RECOMP_TEXTURE_SLOTS];
    DolGxRecompTlut tluts[DOL_GX_RECOMP_TLUT_SLOTS];
    DolGxRecompTlut tmem_tluts[DOL_GX_RECOMP_TMEM_TLUT_SLOTS];
    bool texture_tlut_valid[DOL_GX_RECOMP_TEXTURE_SLOTS];
    u16 texture_tlut_tmem_offset[DOL_GX_RECOMP_TEXTURE_SLOTS];
    u32 texture_tlut_format[DOL_GX_RECOMP_TEXTURE_SLOTS];
    DolGxRecompVertexLayout vertex_layouts[DOL_GX_RECOMP_VERTEX_FORMATS];
    DolGxRecompCopyState copy;
    u32 bp_regs[256];
    bool bp_valid[256];
    bool cull_all;
    u32 matrix_index_a;
    u32 current_pn_matrix;
    f32 position_matrices[DOL_GX_RECOMP_POSITION_MATRIX_COUNT]
                         [DOL_GX_RECOMP_POSITION_MATRIX_WORDS];
    u16 position_matrix_word_mask[DOL_GX_RECOMP_POSITION_MATRIX_COUNT];
    bool position_matrix_valid[DOL_GX_RECOMP_POSITION_MATRIX_COUNT];
    // Normal matrices (XF 0x400): 3 rows of 3, one 9-word slot per matrix. The
    // gxcore lighting path reads matrix M for a draw whose current
    // PN matrix is M; snapshot-only (FIFO-reconstructed), never in the .dolt wire.
    f32 normal_matrices[DOL_GX_RECOMP_NORMAL_MATRIX_COUNT]
                       [DOL_GX_RECOMP_NORMAL_MATRIX_WORDS];
    u16 normal_matrix_word_mask[DOL_GX_RECOMP_NORMAL_MATRIX_COUNT];
    u16 last_xf_base;
    u8 last_xf_count;
    // Decoded XF projection register (XF 0x1020): 6 params + type word. Captured
    // from direct XF_LOAD commands whose range covers 0x1020 so the transform the
    // recomp feeds Aurora is inspectable (a wrong projection puts geometry
    // off-screen -- the cheapest discriminator for the missing-geometry diff vs
    // Dolphin XF memory). Stays invalid until a covering load is seen.
    bool projection_valid;
    f32 projection[6];
    u32 projection_type;
    // Decoded XF viewport register (XF 0x101A..0x101F): wd, ht, zrange, xorig,
    // yorig, farz. A wrong viewport scale/offset also moves geometry off-screen,
    // so it is the second transform input for the Dolphin diff. Captured from any
    // direct XF_LOAD whose range covers 0x101A.
    bool viewport_valid;
    f32 viewport[6];
    // XF lighting state, captured for the lighting-attribution probe and
    // the M6 lighting module: the 8 light objects (XF 0x600..0x67F, 16 words
    // each, laid out exactly like Dolphin's XFMemory `Light`: 3 reserved words,
    // RGBA8 color, cosatt a0-a2, distatt k0-k2, pos xyz, dir xyz) plus the 9
    // channel registers 0x1009..0x1011 (numChans, amb0/1, mat0/1, color
    // ctrl0/1, alpha ctrl0/1). Words stay raw u32: color/ctrl words are packed
    // integers while attenuation/pos/dir are f32 bit patterns, so the consumer
    // reinterprets per word. The masks track which words have been written.
    u32 light_words[DOL_GX_RECOMP_LIGHT_COUNT][DOL_GX_RECOMP_LIGHT_WORDS];
    u16 light_word_mask[DOL_GX_RECOMP_LIGHT_COUNT];
    u32 chan_regs[DOL_GX_RECOMP_CHAN_REG_COUNT];
    u16 chan_reg_mask;
    // Texture matrix memory (XF words 0x78..0xFB): 11 twelve-word slots
    // aligned with the SDK's GX_TEXMTX0..9 + GX_IDENTITY (matrix rows 30..62,
    // 3 rows per slot). Raw-capture pattern mirrors position_matrices; the
    // gxcore texgen path is the consumer.
    f32 tex_matrices[DOL_GX_RECOMP_TEX_MATRIX_COUNT]
                    [DOL_GX_RECOMP_TEX_MATRIX_WORDS];
    u16 tex_matrix_word_mask[DOL_GX_RECOMP_TEX_MATRIX_COUNT];
    // Raw XF register window 0x1018..0x1057 (matrix indices A/B, viewport,
    // projection, numTexGens 0x103F, texgen specs 0x1040.., dual-tex mtx info
    // 0x1050..): one raw word per register + a written-bit per register. The
    // decoded viewport/projection fields above stay authoritative for their
    // existing consumers; this window feeds gxcore's texgen decode.
    u32 xf_regs[DOL_GX_RECOMP_XF_REG_COUNT];
    u64 xf_reg_mask;
    DolGxRecompTraceEvent trace[DOL_GX_RECOMP_MAX_TRACE_EVENTS];
    u32 trace_count;
} DolGxRecompState;

void dol_gx_recomp_init(DolGxRecompState* gx,
                        const DolGuestAddressResolver* resolver);
bool dol_gx_recomp_push_fifo(DolGxRecompState* gx, const void* data, u32 size);
const u8* dol_gx_recomp_fifo_bytes(const DolGxRecompState* gx);
u32 dol_gx_recomp_fifo_size(const DolGxRecompState* gx);
bool dol_gx_recomp_replay_fifo(DolGxRecompState* gx, const void* data,
                               u32 size);
u32 dol_gx_recomp_guest_to_physical(u32 address);

bool dol_gx_recomp_resolve_display_list(DolGxRecompState* gx, u32 address,
                                        u32 size,
                                        DolGuestResolvedRange* out);
bool dol_gx_recomp_call_display_list(DolGxRecompState* gx, u32 address,
                                     u32 size, bool append_fifo);

bool dol_gx_recomp_load_cp_reg(DolGxRecompState* gx, u8 reg, u32 value);
bool dol_gx_recomp_resolve_cp_array(DolGxRecompState* gx, u8 attr,
                                    u32 byte_span,
                                    DolGxRecompResolvedArray* out);

bool dol_gx_recomp_indexed_span(const void* vertices, u32 vertex_size,
                                u16 vertex_count, u32 index_offset,
                                u8 index_size, u32 array_stride,
                                u32 element_size, u32 element_bias,
                                u32* out_span);

bool dol_gx_recomp_texture_size(u16 width, u16 height, u32 format,
                                u32* out_size);
bool dol_gx_recomp_resolve_texture_image(DolGxRecompState* gx, u8 slot,
                                         u32 image0, u32 image3,
                                         DolGxRecompTexture* out);
bool dol_gx_recomp_resolve_tlut(DolGxRecompState* gx, u8 slot,
                                u32 load_tlut0, u32 format, u16 entries,
                                DolGxRecompTlut* out);
bool dol_gx_recomp_resolve_tmem_tlut(DolGxRecompState* gx, u16 tmem_offset,
                                     u32 load_tlut0, u32 format, u16 entries,
                                     DolGxRecompTlut* out);
bool dol_gx_recomp_resolve_copy_destination(DolGxRecompState* gx,
                                            u32 physical_base, u32 byte_size,
                                            DolGuestResolvedRange* out);
/* GXCopyDisp (BP 0x52 trigger with bit 14): an EFB->XFB display copy. There is
 * no texture destination, but the copy-clear it requests is the clear every
 * frame's EFB starts from (e.g. Strikers glxSwap clears {0,0,0,0} here — the
 * shadow-grab alpha background depends on it). Emitted with format 0xF
 * (Dolphin EFBCopyFormat::XFB). */
bool dol_gx_recomp_note_display_copy(DolGxRecompState* gx, u32 clear);

bool dol_gx_recomp_note_bp_reg(DolGxRecompState* gx, u8 reg, u32 value);
bool dol_gx_recomp_note_xf_load(DolGxRecompState* gx, u16 base, u8 count);

// XF register addresses for the viewport + projection register blocks.
#define DOL_GX_XF_MATRIX_INDEX_A 0x1018u /* matIdxA: PN + tex0..3 mtx ids */
#define DOL_GX_XF_VIEWPORT_BASE 0x101Au /* 6 f32, 0x101A..0x101F */
#define DOL_GX_XF_PROJECTION_BASE 0x1020u /* 6 f32 params, 0x1020..0x1025 */
#define DOL_GX_XF_PROJECTION_TYPE 0x1026u /* u32 projection type */
// XF lighting blocks (addresses match Dolphin XFMemory.h).
#define DOL_GX_XF_LIGHT_BASE 0x0600u /* 8 lights x 16 words, 0x600..0x67F */
#define DOL_GX_XF_LIGHT_END 0x0680u /* exclusive */
#define DOL_GX_XF_CHAN_REG_BASE 0x1009u /* numChans..alpha ctrl1, 9 regs */
#define DOL_GX_XF_TEX_MATRIX_BASE 0x0078u /* tex matrix memory, 0x78..0xFB */
#define DOL_GX_XF_REG_WINDOW_BASE 0x1018u /* raw reg window, 0x40 regs */
#define DOL_GX_XF_NORMAL_MATRIX_BASE 0x0400u /* normal matrices, 0x400..0x45F */
#define DOL_GX_XF_NORMAL_MATRIX_END 0x0460u  /* exclusive */

// Decode the XF viewport (0x101A) and/or projection (0x1020) registers from a
// direct XF_LOAD's big-endian data words, for whichever the loaded range
// [base, base+count) covers. `data_be` points at the `count` 32-bit words that
// follow the XF_LOAD header. Sets gx->viewport*/projection* + the matching valid
// flag for each register touched; returns true if either was captured. No-op
// (returns false) when the load touches neither or data_be is NULL. Indexed-XF
// loads carry no inline words, so callers pass data_be=NULL there.
bool dol_gx_recomp_capture_xf_transform(DolGxRecompState* gx, u16 base, u8 count,
                                        const u8* data_be);
bool dol_gx_recomp_note_invalidate_vtx_cache(DolGxRecompState* gx);
bool dol_gx_recomp_set_vertex_layout(DolGxRecompState* gx, u8 vtx_fmt,
                                     u32 vertex_size);
bool dol_gx_recomp_set_indexed_attr(DolGxRecompState* gx, u8 vtx_fmt,
                                    u8 attr, u32 vertex_offset,
                                    u8 index_size, u32 element_size,
                                    u32 element_bias);
bool dol_gx_recomp_derive_vertex_layout(DolGxRecompState* gx, u8 vtx_fmt);

const DolGxRecompTraceEvent*
dol_gx_recomp_trace_events(const DolGxRecompState* gx, u32* count);

#ifdef __cplusplus
}
#endif

#endif
