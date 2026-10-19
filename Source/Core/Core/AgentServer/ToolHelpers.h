// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace AgentServer
{
using namespace std::chrono_literals;

constexpr auto VIDEO_JOB_TIMEOUT = 10s;
constexpr auto FRAME_STEP_TIMEOUT = 3s;

Core::System& GetSystem();

// Throw ToolError with a message the agent can act on.
void RequireGame();
void RequireVRSession();
void RequireStereoSession();

// Runs `job` on the video thread (VR::AgentVideoJobs) and waits for it. Throws ToolError on timeout
// or when emulation stops. Never call it while holding a Core::CPUThreadGuard.
void RunOnVideoThread(std::function<void()> job,
                      std::chrono::milliseconds timeout = VIDEO_JOB_TIMEOUT);

// Runs `job` on the host (UI) thread with Core::QueueHostJob and waits for it.
void RunOnHostThread(std::function<void()> job, std::chrono::milliseconds timeout = 10s);

// Polls `predicate` until it is true. False on timeout or when the server stops.
bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout,
               std::chrono::milliseconds poll = 5ms);

// A case-insensitive ECMAScript regex from a tool argument; nullopt when the pattern is empty. A
// leading "(?i)", which ECMAScript lacks, is accepted. Throws ToolError for a bad pattern.
std::optional<std::regex> MakeToolRegex(std::string_view pattern);

bool IsPaused();
void PauseEmulation();
void ResumeEmulation();
// Steps `frames` presented frames, pausing first. Returns the frames stepped; throws ToolError when
// a step does not finish in time.
u32 AdvanceFrames(u32 frames, std::chrono::milliseconds per_frame_timeout = FRAME_STEP_TIMEOUT);

// The emulated frame state shown to the agent after most actions: state, game, counters.
std::string DescribeEmulationState();

std::string FormatHex32(u32 value);
std::string FormatHash(u64 value);
}  // namespace AgentServer
