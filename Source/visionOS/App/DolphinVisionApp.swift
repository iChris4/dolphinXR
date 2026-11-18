// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

@main
struct DolphinVisionApp: App {
    @State private var emulator = EmulatorModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environment(emulator)
        }
        // No glass of its own: the game screen hides the window while the immersive space is
        // open, and the library draws its glass itself.
        .windowStyle(.plain)
        .defaultSize(width: 1280, height: 860)
        .onChange(of: scenePhase) { _, phase in
            // Pause while the window is not active (the game keeps its state).
            emulator.setPaused(phase != .active)
        }

        // VR: the game in stereo around the player, in mixed immersion. The game window steps
        // aside while it is open; the Digital Crown closes it.
        ImmersiveSpace(id: ImmersiveGame.spaceID) {
            ImmersiveGame.layer(for: emulator)
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}
