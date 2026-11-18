// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

import CompositorServices
import Foundation
import Observation
import QuartzCore
import SwiftUI

/// A disc image or executable in the library folder.
struct GameFile: Identifiable, Hashable {
    let url: URL
    var id: URL { url }
    var name: String { url.deletingPathExtension().lastPathComponent }
}

/// The CPU core a game runs on. visionOS allows no JIT.
enum CPUCoreChoice: Int32, CaseIterable, Identifiable {
    case cachedInterpreter = 5
    case staticRecomp = 6

    var id: Int32 { rawValue }
    var label: String {
        switch self {
        case .cachedInterpreter: "Cached Interpreter"
        case .staticRecomp: "Recompiled module"
        }
    }
}

/// App-side state of the Dolphin core: the library, the running game and errors.
@Observable
final class EmulatorModel {
    static let gameExtensions: Set<String> = [
        "iso", "gcm", "ciso", "gcz", "rvz", "wia", "wbfs", "nkit", "dol", "elf", "tgc",
    ]

    private(set) var games: [GameFile] = []
    private(set) var runningGame: GameFile?
    var cpuCore: CPUCoreChoice = .cachedInterpreter
    var errorMessage: String?

    /// Play in VR (stereo in the immersive space) rather than in the window only.
    var vrEnabled: Bool = dvx_vr_available()
    /// Internal resolution, a multiple of the console's 640×528. 3x is closest to the headset's
    /// eye buffers (1888×1792) and keeps Sunshine at full speed. Kept between launches.
    var resolution: Int32 = EmulatorModel.storedResolution {
        didSet { UserDefaults.standard.set(Int(resolution), forKey: Self.resolutionKey) }
    }
    static let resolutionChoices: [Int32] = [1, 2, 3, 4]
    private static let resolutionKey = "InternalResolution"
    private static var storedResolution: Int32 {
        let stored = UserDefaults.standard.integer(forKey: resolutionKey)
        return stored > 0 ? Int32(stored) : 3
    }
    var vrAvailable: Bool { dvx_vr_available() }
    /// The immersive space's layer renderer reached the provider: a VR boot can start.
    private(set) var immersiveReady = false
    private var gameLayer: CAMetalLayer?

    /// Documents/Games: visible in the Files app, where disc images are dropped.
    let gamesDirectory: URL
    /// Dolphin's user directory: configuration, saves, logs, recompiled modules.
    let userDirectory: URL

    init() {
        let fm = FileManager.default
        let documents = fm.urls(for: .documentDirectory, in: .userDomainMask)[0]
        gamesDirectory = documents.appendingPathComponent("Games", isDirectory: true)
        userDirectory = documents.appendingPathComponent("Dolphin", isDirectory: true)
        try? fm.createDirectory(at: gamesDirectory, withIntermediateDirectories: true)
        try? fm.createDirectory(at: userDirectory, withIntermediateDirectories: true)

        if !userDirectory.path.withCString({ dvx_initialize($0) }) {
            errorMessage = "Dolphin could not start: \(String(cString: dvx_last_error()))"
        }
        refreshLibrary()
        applyLaunchArguments()
    }

    /// Development and test hooks: `-autoboot <name>` starts the library game whose file name
    /// starts with <name>; `-cpu cached|recomp` picks the CPU core; `-vr on|off` plays in VR or
    /// in the window; `-resolution <1-8>` sets the internal resolution; `-loadstate <slot>` loads
    /// that save state once the game runs.
    private func applyLaunchArguments() {
        let arguments = ProcessInfo.processInfo.arguments
        func value(after flag: String) -> String? {
            guard let index = arguments.firstIndex(of: flag), index + 1 < arguments.count else {
                return nil
            }
            return arguments[index + 1]
        }
        if let cpu = value(after: "-cpu") {
            cpuCore = cpu == "recomp" ? .staticRecomp : .cachedInterpreter
        }
        if let vr = value(after: "-vr") {
            vrEnabled = vrAvailable && vr != "off"
        }
        if let scale = value(after: "-resolution").flatMap({ Int32($0) }), (1...8).contains(scale) {
            resolution = scale
        }
        if let slot = value(after: "-loadstate").flatMap({ Int32($0) }) {
            dvx_load_state_after_boot(slot)
        }
        if let name = value(after: "-autoboot"),
           let game = games.first(where: { $0.url.lastPathComponent.hasPrefix(name) }) {
            select(game)
        }
    }

    func refreshLibrary() {
        let files = (try? FileManager.default.contentsOfDirectory(
            at: gamesDirectory, includingPropertiesForKeys: nil)) ?? []
        games = files
            .filter { Self.gameExtensions.contains($0.pathExtension.lowercased()) }
            .map(GameFile.init(url:))
            .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    /// Copies a file chosen in the Files picker into the library.
    func importGame(from source: URL) {
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        let destination = gamesDirectory.appendingPathComponent(source.lastPathComponent)
        do {
            if FileManager.default.fileExists(atPath: destination.path) {
                try FileManager.default.removeItem(at: destination)
            }
            try FileManager.default.copyItem(at: source, to: destination)
            refreshLibrary()
        } catch {
            errorMessage = "Could not import \(source.lastPathComponent): \(error.localizedDescription)"
        }
    }

    func select(_ game: GameFile) {
        errorMessage = nil
        runningGame = game
    }

    /// Called by the game view once its Metal layer has a size.
    func boot(into layer: CAMetalLayer) {
        gameLayer = layer
        bootIfReady()
    }

    /// Boots once the window's layer exists and, in VR, once the immersive space is ready: the
    /// OpenXR session is created at boot and runs on the space's layer renderer.
    private func bootIfReady() {
        guard let game = runningGame, let layer = gameLayer, !dvx_is_running() else { return }
        if vrEnabled && !immersiveReady { return }
        dvx_set_cpu_core(cpuCore.rawValue)
        dvx_set_vr_enabled(vrEnabled)
        dvx_set_internal_resolution(resolution)
        let layerPointer = Unmanaged.passUnretained(layer).toOpaque()
        let booted = game.url.path.withCString { dvx_boot($0, layerPointer) }
        if !booted {
            errorMessage = String(cString: dvx_last_error())
            runningGame = nil
        }
    }

    /// The immersive space could not open: play in the window.
    func immersiveSpaceUnavailable() {
        vrEnabled = false
        bootIfReady()
    }

    /// From the CompositorLayer closure: the immersive space has a layer renderer.
    func attach(_ layerRenderer: LayerRenderer) {
        // An OS object: the provider bridges the pointer back to cp_layer_renderer_t and retains it.
        dvx_set_layer_renderer(Unmanaged.passUnretained(layerRenderer).toOpaque())
        DispatchQueue.main.async {
            // Look-and-pinch: each pinch carries the gaze ray, which the provider turns into the
            // game's pointer and its press.
            layerRenderer.onSpatialEvent = { events in
                for event in events { Self.forward(event) }
            }
            self.immersiveReady = true
            self.bootIfReady()
        }
    }

    private static func forward(_ event: SpatialEventCollection.Event) {
        let phase: Int32
        switch event.phase {
        case .active: phase = 0
        case .ended: phase = 1
        default: phase = 2
        }
        let chirality: Int32
        switch event.chirality {
        case .left?: chirality = 1
        case .right?: chirality = 2
        default: chirality = 0
        }
        let id = UInt64(bitPattern: Int64(event.id.hashValue))
        let ray = event.selectionRay
        let pose = event.inputDevicePose?.pose3D.position
        dvx_spatial_event(id, phase, chirality, ray != nil,
                          Float(ray?.origin.x ?? 0), Float(ray?.origin.y ?? 0), Float(ray?.origin.z ?? 0),
                          Float(ray?.direction.x ?? 0), Float(ray?.direction.y ?? 0),
                          Float(ray?.direction.z ?? 0),
                          pose != nil, Float(pose?.x ?? 0), Float(pose?.y ?? 0), Float(pose?.z ?? 0))
    }

    func stop() {
        dvx_stop()
        runningGame = nil
        gameLayer = nil
        immersiveReady = false
    }

    func setPaused(_ paused: Bool) {
        dvx_set_paused(paused)
    }

    /// The window's quick save state: slot 1 (slots 2-10 are in the VR menu).
    static let quickStateSlot: Int32 = 1

    func saveState() {
        dvx_save_state(Self.quickStateSlot)
    }

    func loadState() {
        dvx_load_state(Self.quickStateSlot)
    }

    func surfaceResized() {
        dvx_surface_resized()
    }
}
