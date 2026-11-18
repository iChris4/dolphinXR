// Copyright 2026 SunPad project
// SPDX-License-Identifier: GPL-2.0-or-later

// iOS stub for the filesystem watcher: the underlying watcher library uses
// macOS-only FSEvents, which is unavailable on the iOS device SDK. Watches are
// accepted and ignored.

#include "Common/FilesystemWatcher.h"

namespace wtr
{
inline namespace watcher
{
// Minimal complete definition so the map<std::string, unique_ptr<watch>>
// member can be instantiated. No watches are ever created on iOS, so the
// real FSEvents-backed implementation is never needed.
class watch
{
};
}  // namespace watcher
}  // namespace wtr

namespace Common
{
FilesystemWatcher::FilesystemWatcher() = default;
FilesystemWatcher::~FilesystemWatcher() = default;

void FilesystemWatcher::Watch(const std::string& /*path*/) {}
void FilesystemWatcher::Unwatch(const std::string& /*path*/) {}
}  // namespace Common
