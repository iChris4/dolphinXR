// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Crypto/SHA1.h"

static constexpr std::string_view ACHIEVEMENT_APPROVED_LIST_FILENAME = "ApprovedInis.json";
// After building tests, find the new hash with:
// ./Binaries/Tests/tests --gtest_filter=PatchAllowlist.VerifyHashes
static const inline Common::SHA1::Digest ACHIEVEMENT_APPROVED_LIST_HASH = {
    0xE6, 0xCD, 0xD7, 0x85, 0x7A, 0xBA, 0x72, 0xEC, 0x34, 0x11,
    0x2B, 0x16, 0xB1, 0x31, 0xD0, 0x0A, 0x0A, 0xD7, 0xFC, 0xCC};
