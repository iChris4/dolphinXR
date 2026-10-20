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
- Most tools need a running game. Start with get_status. If no game runs, ask the user to start it with VR enabled. Outside a stereo VR session (flat Dolphin, or "Launch games in VR" off) screenshots are mono (two eyes when the flat screen uses Dolphin's Stereoscopy) and draws carry no VR route.
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
   2D draws that sample an EFB copy are routed "fullscreen (auto EFB effect)": right for bloom and blur, wrong for a HUD element drawn from a texture the game rendered (a minimap, a portrait), which then sticks to the user's view. get_draw says so; give the draw handling=screen, preferably with an Elements override (exact projection and render state) rather than a texture override. list_copies shows which draws made a texture. Some are rendered only when a level or race starts: pause just before (for example in the pause menu on "Restart"), press the button, then capture_frame frames=N to record the start, and list_copies texture=<hash>. Loading a savestate may not restore such textures.
   Some games draw their HUD in perspective, a few game units in front of the camera, with the same projection, render state and even textures as world objects (Sonic Adventure 2: an item's popup icon and the same icon inside the item capsules). Only their camera depth differs: list_draws shows it (z column) and get_draw compares it with the other draws using the same textures. Add max_depth=<between the two> to the override (Shader, Elements or Texture) so it only matches the near draw, and min_depth=<half the near plane>: a world object that crosses the camera (the camera inside an item capsule) has a small max depth too, but some of its vertices are behind the camera. min_depth alone does the reverse.
   Screenshots are the raw eye buffers: the headset spreads each one over its field of view, so their proportions are not what the user sees. Judge an element's shape against its neighbours on the virtual screen, not against the image.
3. Identify draws with set_preview (mode pink, skip or isolate; indices, shaders, families or textures), take a screenshot, then clear_preview. Draw indices are valid for the captured frame only; hashes stay valid.
4. list_overrides shows the game's Sys and User overrides; describe_override_format kind=... gives the keys and a real example. Kinds: shader (by shader hash or family: broad), elements (draw signature: one element), texture (by texture hash: HUD sprites), hideobject (vertex prefix from capture_hide_object_prefixes: removes geometry everywhere it appears).
5. Handlings: screen (draw on the virtual screen), fullscreen (native per eye, e.g. bloom and blur), fullscreen_mono, headlocked, screen_pane (keeps a sub-region at its depth on the screen), skip, camera_anchor and controller_anchor (follow an element / a controller), passthrough, units_per_meter, flag.
6. set_override kind=... ini_text=... applies one entry live; screenshot; iterate with remove_override / enable_override. When the user confirms, save_overrides (writes User/GameSettingsVR/<GameID>.ini); discard_overrides drops session edits.

Workflow 3: no-culling code (objects pop in or vanish at the edge of the view)
1. Ask the user for a place where it happens and make a reference: save_state label=ref. A savestate taken with Dolphin's Free Look camera pulled back and up shows which parts of the level the game draws, whatever the head direction: ask the user for one when geometry is missing.
2. profile_functions (frames 30-60) lists the most called functions (symbols are built on first use; signature-database names can be wrong for small functions). Culling tests are usually small, called about once per object per frame, with float compares (fcmp), li r3,0 / li r3,1 (bool) and few returns. Skip the GX and matrix functions at the top.
3. disassemble and trace_function on candidates: few distinct r3 values at return, one of them 0 (0/1, or 0/2 as in I-Ninja), and callers in the object or model draw loop point to a visibility test. A frustum test loops over the planes of the view (a count, 16-byte planes) with a dot product and a compare against the radius, or transforms the centre and compares it with the near and far planes and the four screen edges (Sonic Adventure DX).
4. test_patches reference=ref with candidates {address, template}: li_r3_1 (always visible), li_r3_0 (culls everything: a quick check that the function matters), nop_blr (returns at once). When the visible value is not 1, use words [3860000N, 4E800020] (li r3,N; blr). A patch that removes culling raises draw calls against the baseline even without turning (objects behind the camera are drawn again). The look offset does not change what the game culls: in VR add look_yaw_deg (e.g. 90 or 180) so the screenshots show what the patch adds. Hung = the patch froze the game; the state is reloaded after each run.
5. One test is often not the whole story: compare the patched screenshots with the baseline (the Free Look savestate shows it best) and look for the next one. Seen so far: a sibling test right after the first, with the centre passed as three floats; the same test inlined in the model draw functions the draw loop calls (several fcmp, li r0,0 and a branch to the return: turn the branch that skips the test into an unconditional one, e.g. words 4182xxxx -> 4800xxxx); a draw-distance list built once per frame from a sphere ahead of the camera (distance squared against radius squared: patch the instructions that set the centre and radius, not the float constants, which the compiler shares between functions). When a test is inlined in many places, count the copies with cheat_search action=new value=<one of its instruction words, e.g. the load of a clip bound> (aligned u32): Sonic Adventure DX has 62 copies in 20 model draw functions. If they all compare against the same variables (trace_memory read=true lists every reader), widen those instead: +/- infinity (7F800000 / FF800000) makes every compare pass, and nop the stores of the code that rewrites them.
6. save_patch_as_ar_code saves one patch, add_code type=ar several (04XXXXXX lines); cheats must be on (set_cheats_enabled). Ask the user to check it in the headset while playing. A patch in a REL module (disassemble and list_modules say so) only holds while the game keeps the module at that address.
- Removing culling draws more (Sonic Adventure DX: twice the draw calls): on Quest it can overload or hang the GPU. Prefer patches that only widen the test, and ask the user to measure performance.

Workflow 4: disable a screen effect with a code (reflections, heat haze, blur that use an EFB copy and look wrong per eye)
- An override (skip the draws) fixes it without a code; a code also saves the copy and works for every level. Ask the user which they prefer.
1. capture_frame, then list_copies: the effect copies the EFB to a texture (often the whole screen) and draws sampling it follow (list_draws texture=<hash>). The copy row gives its RAM address.
2. cheat_search action=new value=0x8<address> (u32): the variable holding the copy's buffer. trace_memory read=true on it gives the code: the effect's function (it passes the buffer to the copy) and the code that decides to run it.
3. Prefer a data switch next to that variable over patching the effect's code: an enable flag or the callback pointer the game registers each frame. trace_memory read=true write=true on the candidate shows that nothing else reads it, but a switch can also gate more than the effect: in Sonic Adventure DX, zeroing the water callback (80850CC0) removes the reflection in Station Square, but in Emerald Coast and Mystic Ruins the same callback also draws the moving water layers, and the waterfall froze. The effect's code may sit in a level module that other levels load at the same address (disassemble names it): never write code there unconditionally. When the game rewrites the switch every frame (an AR write comes too late), patch the branch of the main module that tests it instead (Sonic Adventure DX underwater: 044C4194 48000024).
4. Confirm in every level that has the effect, not only where it was found: capture_frame with and without the switch and compare the whole draw lists (list_draws page by page): only the copy and the draws that sample it may disappear. Check that animations still move (two test_patch screenshots 20 frames apart from the same reference). Then add_code type=ar.

Workflow 5: 60fps code
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
- Many games only listen to the controller that started the game, file or race (Mario Kart Wii with a GameCube pad ignores the Wii Remote). If a press changes nothing, ask the user which controller they play with.

Debugger tools
- set_breakpoint / wait_for_break / get_registers / get_callstack / step_instruction / remove_breakpoint, set_watchpoint / remove_watchpoint, read_memory / write_memory. Debugging mode is enabled when needed and released when no agent breakpoint remains.
- read_log reads Dolphin's log; set_draw_debug_logging logs every draw (VR_DRAW lines), but capture_frame is usually better.)INSTRUCTIONS";
}
}  // namespace AgentServer
