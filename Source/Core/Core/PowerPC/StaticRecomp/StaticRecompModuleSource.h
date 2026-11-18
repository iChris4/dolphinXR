// RecompCore: explicit native-module source for the StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <utility>

#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"

struct StaticRecompModuleSource
{
  using HostCall = bool (*)(CPUState*, u32, void*);
  using HostCallContains = bool (*)(u32, void*);
  using HostCallRangeContains = bool (*)(u32, u32, void*);

  enum class Kind
  {
    None,
    DynamicPath,
    AttachedDescriptor,
  };

  static StaticRecompModuleSource Dynamic(std::string path_)
  {
    StaticRecompModuleSource source;
    source.kind = Kind::DynamicPath;
    source.path = std::move(path_);
    return source;
  }

  static StaticRecompModuleSource Attached(const StaticRecompModuleDesc* descriptor_)
  {
    StaticRecompModuleSource source;
    source.kind = Kind::AttachedDescriptor;
    source.descriptor = descriptor_;
    return source;
  }

  Kind kind = Kind::None;
  std::string path;
  const StaticRecompModuleDesc* descriptor = nullptr;
  HostCall host_call = nullptr;
  HostCallContains host_call_contains = nullptr;
  HostCallRangeContains host_call_range_contains = nullptr;
  void* host_call_user = nullptr;
};
