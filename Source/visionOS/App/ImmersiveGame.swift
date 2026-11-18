// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

import CompositorServices
import SwiftUI

/// The immersive space the game renders into in VR. CompositorServices hands the layer renderer
/// to the OpenXR provider (Externals/xr_visionos), which paces frames, places the eyes and
/// composites DolphinXR's layers into the drawable.
enum ImmersiveGame {
    static let spaceID = "game"

    static func layer(for emulator: EmulatorModel) -> CompositorLayer {
        CompositorLayer(configuration: GameLayerConfiguration()) { layerRenderer in
            emulator.attach(layerRenderer)
        }
    }
}

struct GameLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        // One texture per eye; the provider reads each view's texture and viewport.
        configuration.layout = .dedicated
        // The compositor works in sRGB; the provider samples DolphinXR's sRGB swapchain images.
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
        // The provider draws full-view quads of the eye images at the eyes' own resolution.
        configuration.isFoveationEnabled = false
    }
}
