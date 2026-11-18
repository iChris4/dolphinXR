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
            LibraryView()
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
                        Button(game.name) { emulator.select(game) }
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

/// The running game in the window (in VR, its mirror), with a way back to the library.
struct GameScreen: View {
    @Environment(EmulatorModel.self) private var emulator
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

    var body: some View {
        GameLayerView()
            .aspectRatio(4.0 / 3.0, contentMode: .fit)
            .background(.black)
            .ornament(attachmentAnchor: .scene(.bottom)) {
                HStack {
                    Text(emulator.runningGame?.name ?? "")
                    Button("Save State", systemImage: "square.and.arrow.down") { emulator.saveState() }
                    Button("Load State", systemImage: "square.and.arrow.up") { emulator.loadState() }
                    Button("Stop", systemImage: "stop.fill") {
                        let wasVR = emulator.vrEnabled
                        emulator.stop()
                        if wasVR {
                            Task { await dismissImmersiveSpace() }
                        }
                    }
                }
                .padding()
                .glassBackgroundEffect()
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
}
