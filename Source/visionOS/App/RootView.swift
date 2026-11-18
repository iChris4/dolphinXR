// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import UniformTypeIdentifiers

struct RootView: View {
    @Environment(EmulatorModel.self) private var emulator

    var body: some View {
        if emulator.runningGame != nil {
            GameScreen()
        } else {
            // The window itself is plain (the game's must be able to vanish); the library has
            // the usual glass.
            LibraryView()
                .glassBackgroundEffect()
        }
    }
}

/// The game library: disc images in Documents/Games, and the CPU core to run them on.
struct LibraryView: View {
    @Environment(EmulatorModel.self) private var emulator
    @State private var importing = false

    var body: some View {
        @Bindable var emulator = emulator
        NavigationStack {
            List {
                Section {
                    Picker("CPU", selection: $emulator.cpuCore) {
                        ForEach(CPUCoreChoice.allCases) { Text($0.label).tag($0) }
                    }
                    Picker("Resolution", selection: $emulator.resolution) {
                        ForEach(EmulatorModel.resolutionChoices, id: \.self) { Text("\($0)x").tag($0) }
                    }
                    if emulator.vrAvailable {
                        Toggle("Play in VR", isOn: $emulator.vrEnabled)
                    }
                    // Gamepads connect and disconnect on their own; the core maps them.
                    TimelineView(.periodic(from: .now, by: 1)) { _ in
                        let name = String(cString: dvx_gamepad_name())
                        LabeledContent("Controller",
                                       value: name.isEmpty ? "None: pair one in Settings › Bluetooth" : name)
                    }
                }
                Section("Games") {
                    if emulator.games.isEmpty {
                        Text("Add disc images with Import, or copy them to DolphinXR › Games in the Files app.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(emulator.games) { game in
                        Button { emulator.select(game) } label: {
                            HStack {
                                Text(game.name)
                                Spacer()
                                // Full speed: the app carries this game's recompiled CPU module.
                                if game.hasRecompModule {
                                    Label("Recompiled", systemImage: "bolt.fill")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
                if let error = emulator.errorMessage {
                    Section { Text(error).foregroundStyle(.red) }
                }
            }
            .navigationTitle("DolphinXR")
            .toolbar {
                Button("Import", systemImage: "square.and.arrow.down") { importing = true }
                Button("Refresh", systemImage: "arrow.clockwise") { emulator.refreshLibrary() }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.data],
                          allowsMultipleSelection: true) { result in
                if case .success(let urls) = result {
                    urls.forEach(emulator.importGame(from:))
                }
            }
            .onAppear { emulator.refreshLibrary() }
        }
    }
}

/// The running game in the window, with its controls below. While the immersive space is open
/// the game is around the player and the window steps aside: it stays, because visionOS ends an
/// app left with no scene, but it shows nothing and takes no input. When the space closes (the
/// Digital Crown) the game is back in it, and Enter Immersive opens the space again.
struct GameScreen: View {
    @Environment(EmulatorModel.self) private var emulator
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @State private var openingSpace = false

    var body: some View {
        let immersive = emulator.immersiveOpen
        GameLayerView()
            .aspectRatio(4.0 / 3.0, contentMode: .fit)
            .opacity(immersive ? 0 : 1)
            .allowsHitTesting(!immersive)
            .background(immersive ? Color.clear : Color.black)
            .persistentSystemOverlays(immersive ? .hidden : .automatic)
            .ornament(visibility: immersive ? .hidden : .visible, attachmentAnchor: .scene(.bottom)) {
                controls
            }
            .task {
                // VR boots once the space's layer renderer reaches the provider (attach).
                guard emulator.vrEnabled else { return }
                switch await openImmersiveSpace(id: ImmersiveGame.spaceID) {
                case .opened: break
                default: emulator.immersiveSpaceUnavailable()
                }
            }
    }

    private var controls: some View {
        HStack {
            Text(emulator.runningGame?.name ?? "")
            if let error = emulator.errorMessage {
                Text(error).foregroundStyle(.red)
            }
            if emulator.bootedInVR {
                Button("Enter Immersive", systemImage: "visionpro") { enterImmersive() }
                    .disabled(openingSpace)
            }
            Button("Save State", systemImage: "square.and.arrow.down") { emulator.saveState() }
            Button("Load State", systemImage: "square.and.arrow.up") { emulator.loadState() }
            Button("Stop", systemImage: "stop.fill") {
                let wasVR = emulator.bootedInVR
                emulator.stop()
                if wasVR {
                    Task { await dismissImmersiveSpace() }
                }
            }
        }
        .padding()
        .glassBackgroundEffect()
    }

    /// The game's space again, after the player left it with the Digital Crown: its new layer
    /// renderer reaches the provider (attach), and the OpenXR session starts again on it.
    private func enterImmersive() {
        openingSpace = true
        emulator.reportError(nil)
        Task {
            switch await openImmersiveSpace(id: ImmersiveGame.spaceID) {
            case .opened: break
            default: emulator.reportError("The immersive space could not open.")
            }
            openingSpace = false
        }
    }
}
