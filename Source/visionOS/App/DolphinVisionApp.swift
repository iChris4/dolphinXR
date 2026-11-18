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
        .defaultSize(width: 1280, height: 860)
        .onChange(of: scenePhase) { _, phase in
            // Pause while the window is not active (the game keeps its state).
            emulator.setPaused(phase != .active)
        }

        // VR: the game in stereo around the player. Mixed immersion keeps the room and the game
        // window visible.
        ImmersiveSpace(id: ImmersiveGame.spaceID) {
            ImmersiveGame.layer(for: emulator)
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}
