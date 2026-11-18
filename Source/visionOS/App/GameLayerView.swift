// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

import GameController
import QuartzCore
import SwiftUI
import UIKit

/// A view backed by the CAMetalLayer Dolphin presents into.
final class MetalLayerView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    /// Called once the layer first has a size, then on every resize.
    var onLayout: ((CAMetalLayer, _ firstLayout: Bool) -> Void)?
    private var laidOut = false
    private var lastSize: CGSize = .zero

    override init(frame: CGRect) {
        super.init(frame: frame)
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        backgroundColor = .black
        // visionOS turns a game controller's buttons into taps on what the player looks at (the
        // buttons below the window, say). The game's view keeps them: they reach the game only,
        // through the Game Controller framework that SDL reads.
        let controllers = GCEventInteraction()
        controllers.handledEventTypes = .gamepad
        addInteraction(controllers)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func layoutSubviews() {
        super.layoutSubviews()
        let scale = traitCollection.displayScale > 0 ? traitCollection.displayScale : 2.0
        metalLayer.contentsScale = scale
        let size = bounds.size
        guard size.width > 0, size.height > 0, size != lastSize else { return }
        lastSize = size
        let first = !laidOut
        laidOut = true
        onLayout?(metalLayer, first)
    }
}

struct GameLayerView: UIViewRepresentable {
    @Environment(EmulatorModel.self) private var emulator

    func makeUIView(context: Context) -> MetalLayerView {
        let view = MetalLayerView(frame: .zero)
        let emulator = self.emulator
        view.onLayout = { layer, firstLayout in
            if firstLayout {
                emulator.boot(into: layer)
            } else {
                emulator.surfaceResized()
            }
        }
        return view
    }

    func updateUIView(_ view: MetalLayerView, context: Context) {}
}
