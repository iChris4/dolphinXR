// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/AgentServer.h"

namespace AgentServer
{
std::string GetServerInstructions()
{
  return R"INSTRUCTIONS(DolphinXR is the Dolphin GameCube/Wii emulator with OpenXR VR. This server lets you help the user make a game comfortable in VR: fix visuals with VR overrides and settings, and find codes such as no-culling or 60fps.

General rules
- The user wears the headset; you see screenshots. Depth, scale and comfort are the user's call: propose, then ask them to confirm in the headset before saving.
- Nothing is saved until a save_* tool (or add_code / save_patch_as_ar_code) runs. Overrides and VR settings you set are for this session only.
- Most tools need a running game. Start with get_status. If no game runs, ask the user to start it with VR enabled. Outside a stereo VR session (flat Dolphin, or "Launch games in VR" off) screenshots are mono and draws carry no VR route.
- Make agent savestates (save_state label=...) as reference points. Never overwrite the user's slots unless asked. Savestates from another Dolphin version do not load.
- In a VR session, screenshots and frame stepping work while paused. Outside VR, resume before tools that need the video thread.
- Unknown tool arguments are ignored and reported: read the note.
- Keep results small: 640 px screenshots (smaller for comparisons), paged tables, filters on list_draws.

Workflow 1: VR settings
- list_vr_settings shows what a game profile can set (UnitsPerMeter, ScreenDistance, RemoveCinematicBars...). Try values with set_vr_setting, compare screenshots, save with save_vr_settings once the user agrees.
- set_look_offset turns the virtual head (yaw/pitch, VR only) to look around without the headset; clear_look_offset afterwards.

Workflow 2: visual fixes with overrides (HUD depth, effects in one eye, black bars, 2D at the wrong place)
1. screenshot eyes=both, and ask the user what looks wrong in the headset. To check other screens, menus or text boxes, navigate there with press_input (see Controller input).
2. capture_frame records every draw of one frame. frame_summary gives counts; list_draws filters them (projection=orthographic finds HUD and 2D, perspective the 3D scene; handling, shader, texture, viewport_class). get_draw index=N shows one draw, the overrides that already match it, and an INI stub to start from.
   2D draws routed "2D per eye, head-locked (VirtualScreen is off)" float in front of the user. Turning VirtualScreen on (set_vr_setting, then save_vr_settings for this game) moves all of them to the screen, but in some games it also takes elements that must stay 3D: ask the user. The alternative is a screen override per element, usually a texture override with the text font or sprite texture.
3. Identify draws with set_preview (mode pink, skip or isolate; indices, shaders, families or textures), take a screenshot, then clear_preview. Draw indices are valid for the captured frame only; hashes stay valid.
4. list_overrides shows the game's Sys and User overrides; describe_override_format kind=... gives the keys and a real example. Kinds: shader (by shader hash or family: broad), elements (draw signature: one element), texture (by texture hash: HUD sprites), hideobject (vertex prefix from capture_hide_object_prefixes: removes geometry everywhere it appears).
5. Handlings: screen (draw on the virtual screen), fullscreen (native per eye, e.g. bloom and blur), fullscreen_mono, headlocked, screen_pane (keeps a sub-region at its depth on the screen), skip, camera_anchor and controller_anchor (follow an element / a controller), passthrough, units_per_meter, flag.
6. set_override kind=... ini_text=... applies one entry live; screenshot; iterate with remove_override / enable_override. When the user confirms, save_overrides (writes User/GameSettingsVR/<GameID>.ini); discard_overrides drops session edits.

Workflow 3: no-culling code (objects pop in or vanish at the edge of the view)
1. Ask the user for a place where it happens and make a reference: save_state label=ref.
2. profile_functions (frames 30-60) lists the most called functions (symbols are built on first use; signature-database names can be wrong for small functions). Culling tests are usually small, called about once per object per frame, with float compares (fcmp) and one or two returns.
3. disassemble and trace_function on candidates: a bool-like r3 at return (0/1) and callers in the object or model draw loop point to a visibility test.
4. test_patches reference=ref with candidates {address, template}: li_r3_1 (always visible), li_r3_0, nop_blr. A patch that removes culling raises draw calls against the baseline even without turning (objects behind the camera are drawn again). In VR add look_yaw_deg (e.g. 90 or 180) so the screenshots show it. Hung = the patch froze the game; the state is reloaded after each run.
5. save_patch_as_ar_code saves the best one; cheats must be on (set_cheats_enabled). Ask the user to check it in the headset while playing.
- Removing culling draws more: on Quest it can overload or hang the GPU. Prefer patches that only widen the test, and ask the user to measure performance.

Workflow 4: 60fps code
1. First ask the user (or search the web) for a known 60fps code for this game and region: much faster and safer.
2. measure_frame_rate: about 0.5 unique frames per field = 30 fps, 1.0 = 60 fps.
3. Find the frame interval: cheat_search action=new data_type=u32 value=2 filter=value, play or advance, then next with filter=previous compare=equal (and value=2 again) until few remain. watch_memory confirms a constant; trace_memory read=true shows the code that reads it. If symbols name VIWaitForRetrace, trace_function on it shows the retrace waits per frame and their caller.
4. test_memory_write reference=ref writes={address, type, value=1} (rewritten at every field like an AR 04 code); frames/field should go to about 1.0. Then check the game speed: timers, animations and physics must not run twice as fast (watch_memory a timer, or ask the user). Many games need more than one write.
5. add_code type=ar with the AR lines (04xxxxxx 00000001 writes a u32 every frame), enabled, then ask the user to play-test.
- A 60fps code doubles the GPU work: check performance on Quest.

Controller input
- press_input plays steps on an emulated controller (GameCube pad, Wii Remote or its attachment): each step holds buttons, sticks, analog triggers or the Wii pointer for some VI fields, a step with only frames waits. Hold a button 3 or 4 fields and put a wait step between two presses of the same button. It ends with a screenshot.
- list_input_controls gives the exact names (write Group/Control when a name is ambiguous, e.g. D-Pad/Up).
- Tell the user before you navigate: the input goes to their game. Prefer an agent savestate before going through menus that change the game (saving, deleting a file).

Debugger tools
- set_breakpoint / wait_for_break / get_registers / get_callstack / step_instruction / remove_breakpoint, set_watchpoint / remove_watchpoint, read_memory / write_memory. Debugging mode is enabled when needed and released when no agent breakpoint remains.
- read_log reads Dolphin's log; set_draw_debug_logging logs every draw (VR_DRAW lines), but capture_frame is usually better.)INSTRUCTIONS";
}
}  // namespace AgentServer
