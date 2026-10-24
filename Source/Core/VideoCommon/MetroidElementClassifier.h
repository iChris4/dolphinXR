// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

enum class MetroidElementProfile
{
  None = 0,
  Prime1GC,
  Prime1Wii,
  Prime2GC,
  Prime2Wii,
  Prime3,
  TrilogyAuto,
};

enum class MetroidElementLayer
{
  Unknown = 0,
  Background3D,
  Shadows,
  Shadow2D,
  BodyShadows,
  ScreenOverlay,
  WiiWorld,
  World,
  XRayWorld,
  XRayEffect,
  ThermalEffect,
  ThermalEffectGun,
  ChargeBeamEffect,
  Gun,
  CinematicWorld,
  EFBCopy,
  BlackBars,
  ScreenFade,
  EchoEffect,
  DarkCentral,
  DarkEffect,
  VisorDirt,
  ScanHighlighter,
  ScanBox,
  ThermalGunAndDoor,
  ScanReticle,
  Reticle,
  WiiReticle,
  ScanIcons,
  ScanCross,
  MorphballWorld,
  UnknownWorld,
  ScanCircle,
  ScanDarken,
  Helmet,
  HUD,
  MorphballHUD,
  XRayHUD,
  DarkVisorHUD,
  VisorRadarHint,
  RadarDot,
  MorphballMapOrHint,
  MapOrHint,
  MorphballMap,
  Map,
  Map0,
  Map1,
  Map2,
  Dialog,
  MapMap,
  MapLegend,
  InventorySamus,
  InventorySamusOutline,
  MapNorth,
  ScanVisor,
  ScanText,
  ScanHologram,
  Visor,
  VisorBootup,
  UnknownVisor,
  UnknownHUD,
  MousePointer,
  Unknown2D,
};

struct MetroidProjectionMetrics
{
  bool perspective = false;
  u32 projection_sequence = 0;

  float hfov = 0.0f;
  float vfov = 0.0f;
  float znear = 0.0f;
  float zfar = 0.0f;

  float left = 0.0f;
  float right = 0.0f;
  float top = 0.0f;
  float bottom = 0.0f;
};

class MetroidElementClassifier
{
public:
  struct RoundedMetrics
  {
    bool perspective = false;
    int layer = 0;
    int h = 0;
    int v = 0;
    int a = 0;
    int n = 0;
    int f = 0;
    int l = 0;
    int r = 0;
    int t = 0;
    int b = 0;
  };

  void ResetFrame();
  MetroidElementLayer Classify(MetroidElementProfile profile,
                               const MetroidProjectionMetrics& metrics);

private:
  struct State
  {
    bool scan_visor = false;
    bool scan_visor_active = false;
    bool xray_visor = false;
    bool thermal_visor = false;
    bool has_thermal_effect = false;
    bool map_screen = false;
    bool inventory = false;
    bool dark_visor = false;
    bool echo_visor = false;
    bool morphball_active = false;
    bool is_demo1 = false;
    bool cinematic = false;
    bool menu = false;
    bool after_cursor = false;
    // Prime 2: the scan box was just classified; the next world projection is the reticle.
    bool after_scan_box = false;
    int wide_count = 0;
    int normal_count = 0;
    int helmet_index = 4;
    int vres = 448;

    void ResetFrame();
  };

  MetroidElementLayer ClassifyOne(MetroidElementProfile profile,
                                  const MetroidProjectionMetrics& metrics);
  MetroidElementLayer ClassifyPrime1(bool wii, State& state, const RoundedMetrics& metrics);
  MetroidElementLayer ClassifyPrime1Perspective(bool wii, State& state,
                                                const RoundedMetrics& metrics);
  MetroidElementLayer ClassifyPrime1Ortho(bool wii, State& state,
                                          const RoundedMetrics& metrics);
  MetroidElementLayer ClassifyPrime2(State& state, const RoundedMetrics& metrics);
  MetroidElementLayer ClassifyPrime2Perspective(State& state, const RoundedMetrics& metrics);
  MetroidElementLayer ClassifyPrime2Ortho(State& state, const RoundedMetrics& metrics);
  MetroidElementLayer ClassifyPrime3(State& state, const RoundedMetrics& metrics);
  MetroidElementLayer ClassifyPrime3Perspective(State& state, const RoundedMetrics& metrics);
  MetroidElementLayer ClassifyPrime3Ortho(State& state, const RoundedMetrics& metrics);

  std::array<State, 6> m_states{};
};

std::string_view MetroidElementProfileToININame(MetroidElementProfile profile);
std::string_view MetroidElementProfileToDisplayName(MetroidElementProfile profile);
std::optional<MetroidElementProfile> MetroidElementProfileFromString(std::string_view value);
std::vector<MetroidElementProfile> GetMetroidElementProfiles();

std::string_view MetroidElementLayerToININame(MetroidElementLayer layer);
std::string_view MetroidElementLayerToDisplayName(MetroidElementLayer layer);
std::optional<MetroidElementLayer> MetroidElementLayerFromString(std::string_view value);
std::vector<MetroidElementLayer> GetMetroidElementLayers();
