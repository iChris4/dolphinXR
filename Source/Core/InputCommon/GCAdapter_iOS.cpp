// Copyright 2026 SunPad project
// SPDX-License-Identifier: GPL-2.0-or-later

// iOS stub for the GameCube adapter: the adapter is a USB-host device and
// cannot be used from iOS (no libusb/IOKit on the device SDK). These linkable
// no-ops keep the adapter API surface available to the rest of the core.

#include "InputCommon/GCAdapter.h"

#include <functional>

#include "InputCommon/GCPadStatus.h"

namespace GCAdapter
{
void Init() {}
void ResetRumble() {}
void Shutdown() {}
void SetAdapterCallback(std::function<void(void)> /*func*/) {}
GCPadStatus Input(int /*chan*/)
{
  return {};
}
void Output(int /*chan*/, u8 /*rumble_command*/) {}
bool IsDetected(const char** /*error_message*/)
{
  return false;
}
bool DeviceConnected(int /*chan*/)
{
  return false;
}
void ResetDeviceType(int /*chan*/) {}
double GetCurrentPollRate()
{
  return 0.0;
}
}  // namespace GCAdapter
