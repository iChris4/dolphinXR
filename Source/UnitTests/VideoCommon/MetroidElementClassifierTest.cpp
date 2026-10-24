// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/MetroidElementClassifier.h"

namespace
{
MetroidProjectionMetrics MakePerspective(float hfov, float vfov, float zfar, u32 sequence = 0)
{
  MetroidProjectionMetrics metrics;
  metrics.perspective = true;
  metrics.projection_sequence = sequence;
  metrics.hfov = hfov;
  metrics.vfov = vfov;
  metrics.znear = 1.0f;
  metrics.zfar = zfar;
  return metrics;
}

MetroidProjectionMetrics MakeOrtho(float left, float right, float top, float bottom, float znear,
                                   float zfar, u32 sequence)
{
  MetroidProjectionMetrics metrics;
  metrics.projection_sequence = sequence;
  metrics.left = left;
  metrics.right = right;
  metrics.top = top;
  metrics.bottom = bottom;
  metrics.znear = znear;
  metrics.zfar = zfar;
  return metrics;
}

// Echoes NTSC-U's projections, as the agent server captured them (the sequence is the number of
// projection loads before it in the frame).
MetroidProjectionMetrics Prime2World(u32 load)
{
  return MakePerspective(73.27f, 55.0f, 750.0f, load);
}

MetroidProjectionMetrics Prime2Hud(u32 load)
{
  return MakePerspective(82.43f, 65.0f, 4096.0f, load);
}

MetroidProjectionMetrics Prime2FullView(u32 load)
{
  return MakeOrtho(-320.0f, 320.0f, 224.0f, -224.0f, -4096.0f, 4096.0f, load);
}
}  // namespace

TEST(MetroidElementClassifier, ParsesProfileAndLayerNames)
{
  EXPECT_EQ(MetroidElementProfileFromString("metroid_prime_1_gc"),
            MetroidElementProfile::Prime1GC);
  EXPECT_EQ(MetroidElementProfileFromString("Metroid Prime Trilogy Auto"),
            MetroidElementProfile::TrilogyAuto);
  EXPECT_EQ(MetroidElementLayerFromString("METROID_SCAN_HOLOGRAM"),
            MetroidElementLayer::ScanHologram);
  EXPECT_EQ(MetroidElementLayerFromString("Dark Visor HUD"), MetroidElementLayer::DarkVisorHUD);
}

TEST(MetroidElementClassifier, Prime1CompactHudProjectionMatchesHydraHudFamily)
{
  MetroidElementClassifier classifier;

  const MetroidElementLayer layer = classifier.Classify(
      MetroidElementProfile::Prime1GC, MakePerspective(0.08f, 0.06f, 4.096f));

  EXPECT_EQ(layer, MetroidElementLayer::ScanText);
}

TEST(MetroidElementClassifier, Prime2WideHudSequenceIsStateful)
{
  MetroidElementClassifier classifier;

  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime2GC,
                                MakePerspective(82.43f, 65.0f, 4096.0f, 1)),
            MetroidElementLayer::Helmet);
  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime2GC,
                                MakePerspective(82.43f, 65.0f, 4096.0f, 2)),
            MetroidElementLayer::HUD);

  classifier.ResetFrame();
  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime2GC,
                                MakePerspective(82.43f, 65.0f, 4096.0f, 1)),
            MetroidElementLayer::Helmet);
}

TEST(MetroidElementClassifier, Prime3HudAndTrilogyAutoClassifyHudLayers)
{
  MetroidElementClassifier classifier;

  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime3,
                                MakePerspective(80.55f, 65.0f, 4096.0f, 1)),
            MetroidElementLayer::HUD);

  MetroidElementClassifier trilogy_classifier;
  EXPECT_EQ(trilogy_classifier.Classify(MetroidElementProfile::TrilogyAuto,
                                        MakePerspective(80.55f, 65.0f, 4096.0f, 1)),
            MetroidElementLayer::HUD);
}

// One frame of the scan visor: Hydra's rules number the projection loads, so the highlighter, the
// darkening, the scan box and the reticle each get their own layer.
TEST(MetroidElementClassifier, Prime2ScanVisorFrameByProjectionLoad)
{
  MetroidElementClassifier classifier;
  const auto classify = [&classifier](const MetroidProjectionMetrics& metrics) {
    return classifier.Classify(MetroidElementProfile::Prime2GC, metrics);
  };

  EXPECT_EQ(classify(Prime2World(0)), MetroidElementLayer::World);
  EXPECT_EQ(classify(Prime2FullView(1)), MetroidElementLayer::ScanHighlighter);
  EXPECT_EQ(classify(Prime2Hud(2)), MetroidElementLayer::Helmet);
  EXPECT_EQ(classify(Prime2FullView(3)), MetroidElementLayer::ScreenOverlay);
  EXPECT_EQ(classify(MakeOrtho(-320.0f, 320.0f, 224.0f, -224.0f, -1.0f, 1.0f, 4)),
            MetroidElementLayer::ScanBox);
  EXPECT_EQ(classify(Prime2World(5)), MetroidElementLayer::ScanReticle);
  EXPECT_EQ(classify(Prime2Hud(6)), MetroidElementLayer::HUD);
}

// The dark world's purple tint is a full-view overlay after the world and its reticle.
TEST(MetroidElementClassifier, Prime2DarkWorldTintIsAScreenOverlay)
{
  MetroidElementClassifier classifier;
  const auto classify = [&classifier](const MetroidProjectionMetrics& metrics) {
    return classifier.Classify(MetroidElementProfile::Prime2GC, metrics);
  };

  EXPECT_EQ(classify(Prime2World(0)), MetroidElementLayer::World);
  EXPECT_EQ(classify(MakeOrtho(0.0f, 640.0f, 0.0f, 448.0f, 0.0f, 1000.0f, 1)),
            MetroidElementLayer::Unknown2D);
  EXPECT_EQ(classify(Prime2World(2)), MetroidElementLayer::Reticle);
  EXPECT_EQ(classify(Prime2FullView(3)), MetroidElementLayer::ScreenOverlay);
  EXPECT_EQ(classify(Prime2Hud(4)), MetroidElementLayer::Helmet);
}

// The echo visor's two overlays share the first projection loaded after the world.
TEST(MetroidElementClassifier, Prime2EchoVisorOverlayIsTheEchoEffect)
{
  MetroidElementClassifier classifier;

  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime2GC, Prime2World(0)),
            MetroidElementLayer::World);
  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime2GC,
                                MakeOrtho(0.0f, 640.0f, 0.0f, 448.0f, -4096.0f, 4096.0f, 1)),
            MetroidElementLayer::EchoEffect);
  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime2GC, Prime2Hud(2)),
            MetroidElementLayer::Helmet);
}

// The scan visor in the dark world: the dark world's effects come first, so the box and the
// reticle are later loads than in the light world, and the world load before the box is not the
// reticle.
TEST(MetroidElementClassifier, Prime2DarkWorldScanVisorFindsTheBoxAndReticle)
{
  MetroidElementClassifier classifier;
  const auto classify = [&classifier](const MetroidProjectionMetrics& metrics) {
    return classifier.Classify(MetroidElementProfile::Prime2GC, metrics);
  };

  EXPECT_EQ(classify(Prime2World(0)), MetroidElementLayer::World);
  EXPECT_EQ(classify(Prime2FullView(1)), MetroidElementLayer::ScanHighlighter);
  EXPECT_EQ(classify(Prime2World(2)), MetroidElementLayer::Reticle);
  EXPECT_EQ(classify(MakeOrtho(0.0f, 640.0f, 0.0f, 448.0f, 0.0f, 1000.0f, 3)),
            MetroidElementLayer::Unknown2D);
  EXPECT_EQ(classify(Prime2FullView(4)), MetroidElementLayer::ScanDarken);
  EXPECT_EQ(classify(Prime2Hud(5)), MetroidElementLayer::Helmet);
  EXPECT_EQ(classify(Prime2FullView(6)), MetroidElementLayer::ScreenOverlay);
  EXPECT_EQ(classify(MakeOrtho(-320.0f, 320.0f, 224.0f, -224.0f, -1.0f, 1.0f, 7)),
            MetroidElementLayer::ScanBox);
  EXPECT_EQ(classify(Prime2World(8)), MetroidElementLayer::ScanReticle);
  EXPECT_EQ(classify(Prime2Hud(9)), MetroidElementLayer::HUD);

  // A box that ends a frame does not make the next frame's world a reticle.
  EXPECT_EQ(classify(MakeOrtho(-320.0f, 320.0f, 224.0f, -224.0f, -1.0f, 1.0f, 10)),
            MetroidElementLayer::ScanBox);
  classifier.ResetFrame();
  EXPECT_EQ(classify(Prime2World(0)), MetroidElementLayer::World);
}
