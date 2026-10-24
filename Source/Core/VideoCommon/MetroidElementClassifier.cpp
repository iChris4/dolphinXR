// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/MetroidElementClassifier.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace
{
struct ProfileName
{
  MetroidElementProfile profile;
  std::string_view ini_name;
  std::string_view display_name;
};

constexpr std::array PROFILE_NAMES{
    ProfileName{MetroidElementProfile::Prime1GC, "metroid_prime_1_gc", "Metroid Prime 1 GC"},
    ProfileName{MetroidElementProfile::Prime1Wii, "metroid_prime_1_wii", "Metroid Prime 1 Wii"},
    ProfileName{MetroidElementProfile::Prime2GC, "metroid_prime_2_gc", "Metroid Prime 2 GC"},
    ProfileName{MetroidElementProfile::Prime2Wii, "metroid_prime_2_wii", "Metroid Prime 2 Wii"},
    ProfileName{MetroidElementProfile::Prime3, "metroid_prime_3", "Metroid Prime 3"},
    ProfileName{MetroidElementProfile::TrilogyAuto, "metroid_prime_trilogy_auto",
                "Metroid Prime Trilogy Auto"},
};

struct LayerName
{
  MetroidElementLayer layer;
  std::string_view ini_name;
  std::string_view display_name;
};

constexpr std::array LAYER_NAMES{
    LayerName{MetroidElementLayer::Unknown, "METROID_UNKNOWN", "Unknown"},
    LayerName{MetroidElementLayer::Background3D, "METROID_BACKGROUND_3D", "Background 3D"},
    LayerName{MetroidElementLayer::Shadows, "METROID_SHADOWS", "Shadows"},
    LayerName{MetroidElementLayer::Shadow2D, "METROID_SHADOW_2D", "Shadow 2D"},
    LayerName{MetroidElementLayer::BodyShadows, "METROID_BODY_SHADOWS", "Body Shadows"},
    LayerName{MetroidElementLayer::ScreenOverlay, "METROID_SCREEN_OVERLAY", "Screen Overlay"},
    LayerName{MetroidElementLayer::WiiWorld, "METROID_WII_WORLD", "Wii World"},
    LayerName{MetroidElementLayer::World, "METROID_WORLD", "World"},
    LayerName{MetroidElementLayer::XRayWorld, "METROID_XRAY_WORLD", "X-Ray World"},
    LayerName{MetroidElementLayer::XRayEffect, "METROID_XRAY_EFFECT", "X-Ray Effect"},
    LayerName{MetroidElementLayer::ThermalEffect, "METROID_THERMAL_EFFECT", "Thermal Effect"},
    LayerName{MetroidElementLayer::ThermalEffectGun, "METROID_THERMAL_EFFECT_GUN",
              "Thermal Effect Gun"},
    LayerName{MetroidElementLayer::ChargeBeamEffect, "METROID_CHARGE_BEAM_EFFECT",
              "Charge Beam Effect"},
    LayerName{MetroidElementLayer::Gun, "METROID_GUN", "Gun"},
    LayerName{MetroidElementLayer::CinematicWorld, "METROID_CINEMATIC_WORLD", "Cinematic World"},
    LayerName{MetroidElementLayer::EFBCopy, "METROID_EFB_COPY", "EFB Copy"},
    LayerName{MetroidElementLayer::BlackBars, "METROID_BLACK_BARS", "Black Bars"},
    LayerName{MetroidElementLayer::ScreenFade, "METROID_SCREEN_FADE", "Screen Fade"},
    LayerName{MetroidElementLayer::EchoEffect, "METROID_ECHO_EFFECT", "Echo Effect"},
    LayerName{MetroidElementLayer::DarkCentral, "METROID_DARK_CENTRAL", "Dark Central"},
    LayerName{MetroidElementLayer::DarkEffect, "METROID_DARK_EFFECT", "Dark Effect"},
    LayerName{MetroidElementLayer::VisorDirt, "METROID_VISOR_DIRT", "Visor Dirt"},
    LayerName{MetroidElementLayer::ScanHighlighter, "METROID_SCAN_HIGHLIGHTER",
              "Scan Highlighter"},
    LayerName{MetroidElementLayer::ScanBox, "METROID_SCAN_BOX", "Scan Box"},
    LayerName{MetroidElementLayer::ThermalGunAndDoor, "METROID_THERMAL_GUN_AND_DOOR",
              "Thermal Gun and Door"},
    LayerName{MetroidElementLayer::ScanReticle, "METROID_SCAN_RETICLE", "Scan Reticle"},
    LayerName{MetroidElementLayer::Reticle, "METROID_RETICLE", "Reticle"},
    LayerName{MetroidElementLayer::WiiReticle, "METROID_WII_RETICLE", "Wii Reticle"},
    LayerName{MetroidElementLayer::ScanIcons, "METROID_SCAN_ICONS", "Scan Icons"},
    LayerName{MetroidElementLayer::ScanCross, "METROID_SCAN_CROSS", "Scan Cross"},
    LayerName{MetroidElementLayer::MorphballWorld, "METROID_MORPHBALL_WORLD",
              "Morphball World"},
    LayerName{MetroidElementLayer::UnknownWorld, "METROID_UNKNOWN_WORLD", "Unknown World"},
    LayerName{MetroidElementLayer::ScanCircle, "METROID_SCAN_CIRCLE", "Scan Circle"},
    LayerName{MetroidElementLayer::ScanDarken, "METROID_SCAN_DARKEN", "Scan Darken"},
    LayerName{MetroidElementLayer::Helmet, "METROID_HELMET", "Helmet"},
    LayerName{MetroidElementLayer::HUD, "METROID_HUD", "HUD"},
    LayerName{MetroidElementLayer::MorphballHUD, "METROID_MORPHBALL_HUD", "Morphball HUD"},
    LayerName{MetroidElementLayer::XRayHUD, "METROID_XRAY_HUD", "X-Ray HUD"},
    LayerName{MetroidElementLayer::DarkVisorHUD, "METROID_DARK_VISOR_HUD", "Dark Visor HUD"},
    LayerName{MetroidElementLayer::VisorRadarHint, "METROID_VISOR_RADAR_HINT",
              "Visor Radar Hint"},
    LayerName{MetroidElementLayer::RadarDot, "METROID_RADAR_DOT", "Radar Dot"},
    LayerName{MetroidElementLayer::MorphballMapOrHint, "METROID_MORPHBALL_MAP_OR_HINT",
              "Morphball Map or Hint"},
    LayerName{MetroidElementLayer::MapOrHint, "METROID_MAP_OR_HINT", "Map or Hint"},
    LayerName{MetroidElementLayer::MorphballMap, "METROID_MORPHBALL_MAP", "Morphball Map"},
    LayerName{MetroidElementLayer::Map, "METROID_MAP", "Map"},
    LayerName{MetroidElementLayer::Map0, "METROID_MAP_0", "Map 0"},
    LayerName{MetroidElementLayer::Map1, "METROID_MAP_1", "Map 1"},
    LayerName{MetroidElementLayer::Map2, "METROID_MAP_2", "Map 2"},
    LayerName{MetroidElementLayer::Dialog, "METROID_DIALOG", "Dialog"},
    LayerName{MetroidElementLayer::MapMap, "METROID_MAP_MAP", "Map Screen Map"},
    LayerName{MetroidElementLayer::MapLegend, "METROID_MAP_LEGEND", "Map Legend"},
    LayerName{MetroidElementLayer::InventorySamus, "METROID_INVENTORY_SAMUS",
              "Inventory Samus"},
    LayerName{MetroidElementLayer::InventorySamusOutline, "METROID_INVENTORY_SAMUS_OUTLINE",
              "Inventory Samus Outline"},
    LayerName{MetroidElementLayer::MapNorth, "METROID_MAP_NORTH", "Map North"},
    LayerName{MetroidElementLayer::ScanVisor, "METROID_SCAN_VISOR", "Scan Visor"},
    LayerName{MetroidElementLayer::ScanText, "METROID_SCAN_TEXT", "Scan Text"},
    LayerName{MetroidElementLayer::ScanHologram, "METROID_SCAN_HOLOGRAM", "Scan Hologram"},
    LayerName{MetroidElementLayer::Visor, "METROID_VISOR", "Visor"},
    LayerName{MetroidElementLayer::VisorBootup, "METROID_VISOR_BOOTUP", "Visor Bootup"},
    LayerName{MetroidElementLayer::UnknownVisor, "METROID_UNKNOWN_VISOR", "Unknown Visor"},
    LayerName{MetroidElementLayer::UnknownHUD, "METROID_UNKNOWN_HUD", "Unknown HUD"},
    LayerName{MetroidElementLayer::MousePointer, "METROID_MOUSE_POINTER", "Mouse Pointer"},
    LayerName{MetroidElementLayer::Unknown2D, "METROID_UNKNOWN_2D", "Unknown 2D"},
};

std::string NormalizeKey(std::string_view value)
{
  std::string normalized;
  normalized.reserve(value.size());
  for (const char c : value)
  {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc))
      normalized.push_back(static_cast<char>(std::toupper(uc)));
  }
  return normalized;
}

int Round100(float x)
{
  return static_cast<int>(std::floor(x * 100.0f + 0.5f));
}

bool Near(int value, int target, int tolerance)
{
  return std::abs(value - target) <= tolerance;
}

bool Far4096(const MetroidElementClassifier::RoundedMetrics& m)
{
  return Near(m.f, 409600, 1400);
}

bool Far750(const MetroidElementClassifier::RoundedMetrics& m)
{
  return Near(m.f, 75000, 800);
}

bool HudV(const MetroidElementClassifier::RoundedMetrics& m)
{
  return Near(m.v, 6500, 700);
}

bool CompactHudProjection(const MetroidElementClassifier::RoundedMetrics& m)
{
  return Far4096(m) && m.h >= 7800 && m.h <= 8400 && m.v >= 5600 && m.v <= 6800;
}

bool Prime1XRayEffectProjection(const MetroidElementClassifier::RoundedMetrics& m)
{
  return m.l == 0 &&
         ((Near(m.n, -100, 20) && Near(m.f, 100, 20)) ||
          (Near(m.r, 6400, 80) && Near(m.t, 4400, 80) && m.b == 0));
}

bool IsWorldLayer(MetroidElementLayer layer)
{
  switch (layer)
  {
  case MetroidElementLayer::Unknown:
  case MetroidElementLayer::Background3D:
  case MetroidElementLayer::Shadows:
  case MetroidElementLayer::Shadow2D:
  case MetroidElementLayer::BodyShadows:
  case MetroidElementLayer::WiiWorld:
  case MetroidElementLayer::World:
  case MetroidElementLayer::XRayWorld:
  case MetroidElementLayer::Gun:
  case MetroidElementLayer::CinematicWorld:
  case MetroidElementLayer::Reticle:
  case MetroidElementLayer::WiiReticle:
  case MetroidElementLayer::ScanIcons:
  case MetroidElementLayer::ScanCross:
  case MetroidElementLayer::MorphballWorld:
  case MetroidElementLayer::UnknownWorld:
  case MetroidElementLayer::MousePointer:
  case MetroidElementLayer::Unknown2D:
    return true;
  default:
    return false;
  }
}

MetroidElementClassifier::RoundedMetrics RoundMetrics(const MetroidProjectionMetrics& metrics)
{
  MetroidElementClassifier::RoundedMetrics rounded;
  rounded.perspective = metrics.perspective;
  rounded.layer = static_cast<int>(metrics.projection_sequence);

  if (metrics.perspective)
  {
    rounded.h = Round100(metrics.hfov);
    rounded.v = Round100(metrics.vfov);
    rounded.a = metrics.vfov != 0.0f ? Round100(metrics.hfov / metrics.vfov) : 0;
    rounded.n = Round100(metrics.znear);
    rounded.f = Round100(metrics.zfar);

    // Some current OpenXR projection paths expose the same Metroid HUD projections at roughly
    // 1/1000th of Hydra's original values. Normalize that compact form so the profile classifier
    // still catches the HUD/visor family while preserving exact Hydra-scale inputs.
    if (std::abs(rounded.h) <= 100 && std::abs(rounded.v) <= 100 &&
        std::abs(rounded.f) <= 1000)
    {
      rounded.h *= 1000;
      rounded.v *= 1000;
      rounded.f *= 1000;
      rounded.n *= 1000;
    }
  }
  else
  {
    rounded.l = Round100(metrics.left);
    rounded.r = Round100(metrics.right);
    rounded.t = Round100(metrics.top);
    rounded.b = Round100(metrics.bottom);
    rounded.n = Round100(metrics.znear);
    rounded.f = Round100(metrics.zfar);
  }

  return rounded;
}

size_t StateIndex(MetroidElementProfile profile)
{
  switch (profile)
  {
  case MetroidElementProfile::Prime1GC:
    return 0;
  case MetroidElementProfile::Prime1Wii:
    return 1;
  case MetroidElementProfile::Prime2GC:
    return 2;
  case MetroidElementProfile::Prime2Wii:
    return 3;
  case MetroidElementProfile::Prime3:
    return 4;
  case MetroidElementProfile::TrilogyAuto:
    return 5;
  case MetroidElementProfile::None:
  default:
    return 0;
  }
}
}  // namespace

void MetroidElementClassifier::State::ResetFrame()
{
  wide_count = 0;
  normal_count = 0;
  after_cursor = false;
  after_scan_box = false;
  has_thermal_effect = false;
  menu = false;
}

void MetroidElementClassifier::ResetFrame()
{
  for (State& state : m_states)
    state.ResetFrame();
}

MetroidElementLayer MetroidElementClassifier::Classify(MetroidElementProfile profile,
                                                       const MetroidProjectionMetrics& metrics)
{
  if (profile != MetroidElementProfile::TrilogyAuto)
    return ClassifyOne(profile, metrics);

  // Trilogy can route MP1, MP2, or MP3 content through the same title ID. Keep independent
  // subprofile state and prefer layers that an override is likely to act on.
  const MetroidElementLayer prime3 = ClassifyOne(MetroidElementProfile::Prime3, metrics);
  if (!IsWorldLayer(prime3))
    return prime3;
  const MetroidElementLayer prime1 = ClassifyOne(MetroidElementProfile::Prime1Wii, metrics);
  if (!IsWorldLayer(prime1))
    return prime1;
  const MetroidElementLayer prime2 = ClassifyOne(MetroidElementProfile::Prime2Wii, metrics);
  if (!IsWorldLayer(prime2))
    return prime2;
  if (prime3 != MetroidElementLayer::Unknown && prime3 != MetroidElementLayer::Unknown2D)
    return prime3;
  if (prime1 != MetroidElementLayer::Unknown && prime1 != MetroidElementLayer::Unknown2D)
    return prime1;
  return prime2;
}

MetroidElementLayer
MetroidElementClassifier::ClassifyOne(MetroidElementProfile profile,
                                      const MetroidProjectionMetrics& metrics)
{
  const RoundedMetrics rounded = RoundMetrics(metrics);
  switch (profile)
  {
  case MetroidElementProfile::Prime1GC:
    return ClassifyPrime1(false, m_states[StateIndex(profile)], rounded);
  case MetroidElementProfile::Prime1Wii:
    return ClassifyPrime1(true, m_states[StateIndex(profile)], rounded);
  case MetroidElementProfile::Prime2GC:
  case MetroidElementProfile::Prime2Wii:
    return ClassifyPrime2(m_states[StateIndex(profile)], rounded);
  case MetroidElementProfile::Prime3:
    return ClassifyPrime3(m_states[StateIndex(profile)], rounded);
  case MetroidElementProfile::None:
  case MetroidElementProfile::TrilogyAuto:
  default:
    return MetroidElementLayer::Unknown;
  }
}

MetroidElementLayer MetroidElementClassifier::ClassifyPrime1(bool wii, State& state,
                                                             const RoundedMetrics& metrics)
{
  return metrics.perspective ? ClassifyPrime1Perspective(wii, state, metrics) :
                               ClassifyPrime1Ortho(wii, state, metrics);
}

MetroidElementLayer
MetroidElementClassifier::ClassifyPrime1Perspective(bool wii, State& state,
                                                    const RoundedMetrics& m)
{
  if (m.layer == 0)
  {
    state.xray_visor = false;
    if (wii)
      state.thermal_visor = false;
  }

  if (m.layer == 2)
    state.map_screen = false;

  if ((Near(m.v, m.h, 0) || (wii && Near(m.a, 125, 2))) && m.v < 100 && m.h < 100 &&
      m.layer == 0)
  {
    return MetroidElementLayer::Shadows;
  }
  if (Near(m.v, m.h, 0) && Near(m.v, 5500, 80) && (m.layer == 1 || (!wii && m.layer == 2)))
    return MetroidElementLayer::BodyShadows;
  if (Near(m.f, 300, 20) && Near(m.v, 5500, 80))
  {
    state.is_demo1 = true;
    return MetroidElementLayer::Gun;
  }
  if (Far4096(m) && HudV(m))
  {
    state.cinematic = false;
    if (Near(m.h, 8055, 180) || CompactHudProjection(m))
    {
      state.morphball_active = false;
      state.map_screen = false;
      state.inventory = false;
      return state.xray_visor || state.thermal_visor ? MetroidElementLayer::XRayHUD :
                                                       MetroidElementLayer::ScanText;
    }

    if (!wii && (m.layer < 5 || !state.has_thermal_effect))
      state.thermal_visor = false;

    ++state.wide_count;
    switch (state.wide_count)
    {
    case 1:
      if (state.map_screen || state.inventory)
      {
        if (state.inventory)
        {
          state.map_screen = false;
          return MetroidElementLayer::Helmet;
        }
        return MetroidElementLayer::MapMap;
      }
      if (state.xray_visor || state.thermal_visor)
        return MetroidElementLayer::VisorRadarHint;
      if (!wii && state.is_demo1 && state.morphball_active)
        return MetroidElementLayer::MorphballMapOrHint;
      if (!wii && state.is_demo1)
        return MetroidElementLayer::Helmet;
      return MetroidElementLayer::HUD;
    case 2:
      if (state.morphball_active)
        return MetroidElementLayer::MorphballMapOrHint;
      if (state.map_screen)
        return MetroidElementLayer::Helmet;
      if (state.xray_visor || state.thermal_visor)
        return MetroidElementLayer::RadarDot;
      if (!wii && state.dark_visor)
        return MetroidElementLayer::Helmet;
      if (state.is_demo1)
      {
        state.morphball_active = false;
        return MetroidElementLayer::HUD;
      }
      return MetroidElementLayer::VisorRadarHint;
    case 3:
      if (state.morphball_active)
      {
        state.scan_visor_active = false;
        return MetroidElementLayer::MorphballMap;
      }
      if (state.xray_visor || state.thermal_visor)
      {
        state.scan_visor_active = false;
        return MetroidElementLayer::Map;
      }
      if (!wii && state.dark_visor)
      {
        state.scan_visor_active = false;
        return MetroidElementLayer::DarkVisorHUD;
      }
      if (state.is_demo1)
        return MetroidElementLayer::VisorRadarHint;
      if (Near(m.h, 8243, 160))
      {
        if (wii && state.scan_visor)
          return MetroidElementLayer::Visor;
        state.scan_visor_active = false;
        return MetroidElementLayer::RadarDot;
      }
      state.scan_visor_active = false;
      return MetroidElementLayer::UnknownVisor;
    case 4:
      if (state.morphball_active)
        return MetroidElementLayer::MorphballMap;
      if (state.scan_visor_active && !Near(m.h, 8243, 160))
        return MetroidElementLayer::ScanHologram;
      if (state.xray_visor || state.thermal_visor)
        return MetroidElementLayer::Helmet;
      if (!wii && (state.dark_visor || state.is_demo1))
        return MetroidElementLayer::RadarDot;
      if (state.scan_visor_active)
        return MetroidElementLayer::UnknownHUD;
      if (Near(m.h, 8243, 160))
        return MetroidElementLayer::Map;
      return MetroidElementLayer::UnknownHUD;
    case 5:
      if (state.dark_visor)
        return MetroidElementLayer::MapOrHint;
      if (state.is_demo1)
        return MetroidElementLayer::Map;
      if (Near(m.h, 8243, 160) && !state.scan_visor_active)
        return MetroidElementLayer::Helmet;
      return MetroidElementLayer::UnknownHUD;
    case 6:
      return state.dark_visor ? MetroidElementLayer::Map : MetroidElementLayer::UnknownHUD;
    default:
      return MetroidElementLayer::UnknownHUD;
    }
  }
  if (Far4096(m) && Near(m.v, 5500, 280))
  {
    if ((state.map_screen || state.inventory) && (Near(m.h, 7327, 180) || Near(m.h, 8564, 220)))
    {
      state.map_screen = false;
      state.inventory = true;
      return MetroidElementLayer::InventorySamus;
    }
    state.scan_visor_active = true;
    return MetroidElementLayer::ScanHologram;
  }
  if (Far4096(m) && Near(m.v, 5443, 280))
  {
    state.map_screen = true;
    return wii || !state.menu ? MetroidElementLayer::MapLegend : MetroidElementLayer::Dialog;
  }
  if (Far750(m) && (Near(m.v, 4958, 180) || Near(m.v, 4522, 180)))
  {
    state.morphball_active = true;
    state.map_screen = false;
    state.scan_visor = false;
    state.inventory = false;
    state.cinematic = false;
    state.thermal_visor = false;
    return MetroidElementLayer::MorphballWorld;
  }
  if (Far750(m) && Near(m.v, 5021, 180))
  {
    state.xray_visor = true;
    state.morphball_active = false;
    state.map_screen = false;
    state.scan_visor = false;
    state.thermal_visor = false;
    state.inventory = false;
    state.cinematic = false;
    return MetroidElementLayer::XRayWorld;
  }
  if (Far750(m) && (Near(m.h, 7327, 180) || Near(m.h, 8564, 220)))
  {
    state.cinematic = false;
    ++state.normal_count;
    state.map_screen = false;
    state.inventory = false;
    switch (state.normal_count)
    {
    case 1:
      if (state.xray_visor)
      {
        state.morphball_active = false;
        if (m.layer > 1)
          state.scan_visor = false;
        return wii && Near(m.h, 8564, 220) ? MetroidElementLayer::WiiReticle :
                                             MetroidElementLayer::Reticle;
      }
      state.morphball_active = false;
      if (m.layer > 1)
        state.scan_visor = false;
      return MetroidElementLayer::World;
    case 2:
      if (state.thermal_visor)
        return MetroidElementLayer::ThermalGunAndDoor;
      if (state.scan_visor)
        return MetroidElementLayer::ScanIcons;
      return wii && Near(m.h, 8564, 220) ? MetroidElementLayer::WiiReticle :
                                           MetroidElementLayer::Reticle;
    case 3:
      if (state.thermal_visor)
        return wii && Near(m.h, 8564, 220) ? MetroidElementLayer::WiiReticle :
                                             MetroidElementLayer::Reticle;
      if (state.xray_visor)
        return MetroidElementLayer::UnknownHUD;
      state.scan_visor = true;
      return MetroidElementLayer::ScanCross;
    default:
      return MetroidElementLayer::UnknownWorld;
    }
  }
  if (Far750(m) && Near(m.h, 6594, 140))
    return MetroidElementLayer::WiiWorld;
  if (state.morphball_active && m.v >= 4958 && m.v < 5500)
  {
    state.map_screen = false;
    state.scan_visor = false;
    state.inventory = false;
    state.cinematic = false;
    state.thermal_visor = false;
    return MetroidElementLayer::MorphballWorld;
  }

  state.cinematic = true;
  state.thermal_visor = false;
  return MetroidElementLayer::CinematicWorld;
}

MetroidElementLayer MetroidElementClassifier::ClassifyPrime1Ortho(bool wii, State& state,
                                                                  const RoundedMetrics& m)
{
  if (state.xray_visor && Prime1XRayEffectProjection(m))
  {
    state.dark_visor = false;
    state.scan_visor = false;
    state.scan_visor_active = false;
    state.morphball_active = false;
    state.thermal_visor = false;
    state.has_thermal_effect = false;
    return MetroidElementLayer::XRayEffect;
  }

  if (m.layer == 0)
  {
    state.xray_visor = false;
    if (wii)
      state.thermal_visor = false;
  }
  if (std::abs(m.t) == 44800 || std::abs(m.b) == 44800)
    state.vres = 448;
  else if (std::abs(m.t) == 52800 || std::abs(m.b) == 52800)
    state.vres = 528;

  if (m.layer == 0 && state.map_screen && m.l == 0 && Near(m.n, -100, 20))
    return MetroidElementLayer::Map0;
  if ((m.l == -88 || m.l == -87 || (wii && m.l == -88)) && m.n == 0)
  {
    state.morphball_active = true;
    state.thermal_visor = false;
    return MetroidElementLayer::Shadow2D;
  }
  if (m.l == -32000 && Near(m.n, -409600, 1400) && state.xray_visor)
  {
    state.map_screen = false;
    state.thermal_visor = false;
    return MetroidElementLayer::VisorDirt;
  }
  if (m.l == -32000 && Near(m.n, -409600, 1400) && state.map_screen)
  {
    state.xray_visor = false;
    state.thermal_visor = false;
    return m.layer <= 1 ? MetroidElementLayer::Map1 :
                          (m.layer == 2 ? MetroidElementLayer::Map2 : MetroidElementLayer::MapNorth);
  }
  if (m.l == -32000 && (m.t == 22400 || m.t == 26400) && Near(m.n, -409600, 1400) &&
      Near(m.f, 409600, 1400) && (state.cinematic || wii))
  {
    state.thermal_visor = false;
    return wii ? MetroidElementLayer::EFBCopy : MetroidElementLayer::BlackBars;
  }
  if (m.l == -32000 && Near(m.n, -409600, 1400) && Near(m.f, 409600, 1400))
  {
    state.thermal_visor = false;
    if (!wii && m.layer == 4 && !state.scan_visor)
      return MetroidElementLayer::BlackBars;
    if (wii && (m.layer == 3 || m.layer == 4) && !state.scan_visor)
      return MetroidElementLayer::EFBCopy;
    return wii ? MetroidElementLayer::ScanCircle : MetroidElementLayer::ScanDarken;
  }
  if (m.l == -32000 && Near(m.n, -100, 20) && Near(m.f, 100, 20))
  {
    state.thermal_visor = false;
    return MetroidElementLayer::ScanBox;
  }
  if (m.l == -320)
  {
    state.morphball_active = true;
    state.thermal_visor = false;
    return MetroidElementLayer::MorphballHUD;
  }
  if (!wii && m.layer > 2 && m.l == 0 && m.t == 0 && Near(m.n, -409600, 1400) &&
      state.has_thermal_effect)
  {
    state.has_thermal_effect = true;
    state.thermal_visor = true;
    state.dark_visor = false;
    state.scan_visor = false;
    state.scan_visor_active = false;
    state.morphball_active = false;
    state.xray_visor = false;
    return MetroidElementLayer::ThermalEffectGun;
  }
  if (m.l == 0 && m.t == 0 && Near(m.n, -409600, 1400))
  {
    state.has_thermal_effect = true;
    if (state.thermal_visor || wii)
    {
      state.thermal_visor = true;
      state.dark_visor = false;
      state.scan_visor = false;
      state.scan_visor_active = false;
      state.morphball_active = false;
      state.xray_visor = false;
      return m.layer > 2 ? MetroidElementLayer::ThermalEffectGun :
                           MetroidElementLayer::ThermalEffect;
    }
    return MetroidElementLayer::ChargeBeamEffect;
  }
  if (state.inventory && m.l == 0 && m.r == 100 && m.t == 100 && Near(m.n, -100, 20) &&
      Near(m.f, 100, 20))
  {
    return MetroidElementLayer::InventorySamusOutline;
  }
  if (m.l == -1570)
  {
    state.menu = true;
    state.scan_visor = false;
    state.thermal_visor = false;
    state.xray_visor = false;
    state.morphball_active = false;
    state.inventory = false;
    state.map_screen = false;
    state.cinematic = false;
    return MetroidElementLayer::Unknown2D;
  }
  if (m.l == -105)
    return MetroidElementLayer::Shadow2D;
  state.scan_visor = false;
  state.echo_visor = false;
  return MetroidElementLayer::Unknown2D;
}

MetroidElementLayer MetroidElementClassifier::ClassifyPrime2(State& state,
                                                             const RoundedMetrics& metrics)
{
  return metrics.perspective ? ClassifyPrime2Perspective(state, metrics) :
                               ClassifyPrime2Ortho(state, metrics);
}

MetroidElementLayer
MetroidElementClassifier::ClassifyPrime2Perspective(State& state, const RoundedMetrics& m)
{
  if (m.layer == 1)
    state.scan_visor = false;

  if (Near(m.v, m.h, 0) && m.layer <= 1)
    return MetroidElementLayer::Shadows;
  if (Far4096(m) && HudV(m))
  {
    ++state.wide_count;
    switch (state.wide_count)
    {
    case 1:
      if (state.morphball_active)
        return MetroidElementLayer::MorphballHUD;
      if (state.map_screen)
        return MetroidElementLayer::MapMap;
      if (state.dark_visor && (m.layer == 1 || m.layer == 2))
        return MetroidElementLayer::DarkCentral;
      return MetroidElementLayer::Helmet;
    case 2:
      state.inventory = false;
      if (state.morphball_active)
        return MetroidElementLayer::MorphballMapOrHint;
      if (state.map_screen)
        return MetroidElementLayer::MapLegend;
      if (state.dark_visor)
        return MetroidElementLayer::Helmet;
      return MetroidElementLayer::HUD;
    case 3:
      state.map_screen = false;
      if (state.morphball_active)
      {
        state.scan_visor_active = false;
        return MetroidElementLayer::MorphballMap;
      }
      if (state.dark_visor)
      {
        state.scan_visor_active = false;
        return MetroidElementLayer::DarkVisorHUD;
      }
      if (Near(m.h, 8243, 160))
      {
        state.scan_visor_active = false;
        state.scan_visor = false;
        return MetroidElementLayer::RadarDot;
      }
      if (Near(m.h, 8055, 180) || CompactHudProjection(m))
      {
        state.scan_visor_active = true;
        state.morphball_active = false;
        return MetroidElementLayer::ScanText;
      }
      state.scan_visor_active = false;
      return MetroidElementLayer::UnknownVisor;
    case 4:
      if (state.morphball_active)
        return MetroidElementLayer::MorphballMap;
      if (state.scan_visor_active && !Near(m.h, 8243, 160))
        return MetroidElementLayer::ScanHologram;
      if (state.dark_visor)
        return MetroidElementLayer::RadarDot;
      if (state.scan_visor_active)
        return MetroidElementLayer::UnknownHUD;
      if (Near(m.h, 8243, 160))
        return MetroidElementLayer::MapOrHint;
      return MetroidElementLayer::UnknownHUD;
    case 5:
      if (state.dark_visor)
        return MetroidElementLayer::MapOrHint;
      if (Near(m.h, 8243, 160) && !state.scan_visor_active)
        return MetroidElementLayer::Map;
      return MetroidElementLayer::UnknownHUD;
    case 6:
      return state.dark_visor ? MetroidElementLayer::Map : MetroidElementLayer::UnknownHUD;
    default:
      return MetroidElementLayer::UnknownHUD;
    }
  }
  if (Far4096(m) && Near(m.v, 5500, 280))
  {
    state.scan_visor_active = true;
    return MetroidElementLayer::ScanHologram;
  }
  if (Far4096(m) && Near(m.v, 5443, 280))
  {
    state.map_screen = true;
    return MetroidElementLayer::MapLegend;
  }
  if (Far750(m) && (Near(m.v, 4958, 180) || Near(m.v, 4522, 180)))
  {
    state.morphball_active = true;
    state.map_screen = false;
    state.scan_visor = false;
    state.inventory = false;
    state.cinematic = false;
    return MetroidElementLayer::MorphballWorld;
  }
  if (Far750(m) && (Near(m.h, 7327, 180) || Near(m.h, 8564, 220)))
  {
    state.cinematic = false;
    ++state.normal_count;
    state.map_screen = false;
    // The scan reticle is the world projection loaded right after the scan box. (Hydra took the
    // second world load, which holds in the light world only: the dark world loads more first.)
    if (std::exchange(state.after_scan_box, false))
      return MetroidElementLayer::ScanReticle;
    switch (state.normal_count)
    {
    case 1:
      state.morphball_active = false;
      return MetroidElementLayer::World;
    case 2:
      if (state.dark_visor)
        return MetroidElementLayer::UnknownWorld;
      return MetroidElementLayer::Reticle;
    case 3:
      return state.dark_visor ? MetroidElementLayer::Reticle : MetroidElementLayer::UnknownWorld;
    default:
      return MetroidElementLayer::UnknownWorld;
    }
  }
  if (Far4096(m) && m.layer == 4 && state.map_screen)
  {
    state.inventory = true;
    state.cinematic = false;
    return MetroidElementLayer::InventorySamus;
  }
  if (state.morphball_active && m.v >= 4958 && m.v < 5500)
  {
    state.map_screen = false;
    state.scan_visor = false;
    state.inventory = false;
    state.cinematic = false;
    return MetroidElementLayer::MorphballWorld;
  }

  state.cinematic = true;
  state.scan_visor = false;
  state.map_screen = false;
  return MetroidElementLayer::CinematicWorld;
}

MetroidElementLayer MetroidElementClassifier::ClassifyPrime2Ortho(State& state,
                                                                  const RoundedMetrics& m)
{
  if (std::abs(m.t) == 44800 || std::abs(m.b) == 44800)
    state.vres = 448;
  else if (std::abs(m.t) == 52800 || std::abs(m.b) == 52800)
    state.vres = 528;

  if (m.layer == 0)
  {
    state.dark_visor = false;
    if (m.l == 0 && m.r == 32000 && (m.t == 22400 || m.t == 26400) &&
        Near(m.n, -100, 20) && Near(m.f, 100, 20))
    {
      state.map_screen = true;
      state.morphball_active = false;
      state.scan_visor = false;
      state.echo_visor = false;
      return MetroidElementLayer::Map0;
    }
    if ((m.l == -75 || m.l == -76) && m.n == 0)
      return MetroidElementLayer::Shadow2D;
    return MetroidElementLayer::Unknown2D;
  }
  if (m.layer == 1)
  {
    state.dark_visor = false;
    if (m.l == -32000 && Near(m.n, -409600, 1400))
    {
      if (state.map_screen)
      {
        state.scan_visor = false;
        state.echo_visor = false;
        state.morphball_active = false;
        return MetroidElementLayer::Map1;
      }
      if (state.cinematic)
      {
        state.scan_visor = false;
        state.scan_visor_active = false;
        return MetroidElementLayer::BlackBars;
      }
      state.scan_visor = true;
      state.echo_visor = false;
      return MetroidElementLayer::ScanHighlighter;
    }
    if (m.l == 0 && Near(m.n, -409600, 1400))
    {
      state.echo_visor = true;
      state.scan_visor = false;
      return MetroidElementLayer::EchoEffect;
    }
    if ((m.l == -75 || m.l == -76) && m.n == 0)
    {
      state.scan_visor = false;
      return MetroidElementLayer::Shadow2D;
    }
    state.scan_visor = false;
    state.echo_visor = false;
    return m.l == -105 ? MetroidElementLayer::Shadow2D : MetroidElementLayer::Unknown2D;
  }
  if ((m.layer == 2 || m.layer == 3) && m.l == 0 && Near(m.n, -409600, 1400))
  {
    if (state.cinematic)
    {
      state.dark_visor = false;
      state.scan_visor = false;
      state.scan_visor_active = false;
      state.morphball_active = false;
      return MetroidElementLayer::Unknown2D;
    }
    state.dark_visor = true;
    state.scan_visor = false;
    state.scan_visor_active = false;
    state.morphball_active = false;
    state.cinematic = false;
    return MetroidElementLayer::DarkEffect;
  }
  if ((m.layer == 2 || m.layer == 3 || m.layer == 4 || m.layer == 5 || m.layer == 6 ||
       m.layer == 7) &&
      m.l == -32000 && (m.t == 22400 || m.t == 26400) && Near(m.n, -409600, 1400) &&
      Near(m.f, 409600, 1400))
  {
    if (state.cinematic)
      return MetroidElementLayer::BlackBars;
    if (state.map_screen && m.layer == 5)
      return MetroidElementLayer::MapNorth;
    if (state.scan_visor && m.layer == 2)
      return MetroidElementLayer::ScreenOverlay;
    if (m.layer == 4 || (m.layer == 5 && state.normal_count == 2))
      return MetroidElementLayer::ScanDarken;
    return MetroidElementLayer::ScreenOverlay;
  }
  // Hydra's loads 4 to 6 are the light world's; in the dark world the box comes later.
  if ((m.layer == 4 || m.layer == 5 || m.layer == 6 || state.scan_visor) && m.l == -32000 &&
      Near(m.n, -100, 20) && Near(m.f, 100, 20))
  {
    state.after_scan_box = true;
    return MetroidElementLayer::ScanBox;
  }
  if (m.layer == 3 && m.l == 0 && (m.t == 22400 || m.t == 26400) && m.r == 32000 &&
      m.b == 0 && Near(m.n, -100, 20) && Near(m.f, 100, 20))
  {
    return MetroidElementLayer::VisorBootup;
  }
  return MetroidElementLayer::Unknown2D;
}

MetroidElementLayer MetroidElementClassifier::ClassifyPrime3(State& state,
                                                             const RoundedMetrics& metrics)
{
  return metrics.perspective ? ClassifyPrime3Perspective(state, metrics) :
                               ClassifyPrime3Ortho(state, metrics);
}

MetroidElementLayer
MetroidElementClassifier::ClassifyPrime3Perspective(State& state, const RoundedMetrics& m)
{
  if (m.layer < 2)
    state.map_screen = false;

  if ((Near(m.v, m.h, 0) || Near(m.a, 125, 2)) && m.v < 300 && m.h < 300 && m.layer == 0)
    return MetroidElementLayer::Shadows;
  if (Far4096(m) && HudV(m))
  {
    state.cinematic = false;
    if (Near(m.h, 8055, 180) || CompactHudProjection(m))
    {
      state.inventory = false;
      ++state.wide_count;
      if (state.morphball_active)
        return MetroidElementLayer::MorphballHUD;
      if (state.after_cursor && !state.map_screen)
        return MetroidElementLayer::Map;
      if (state.wide_count == state.helmet_index && !state.map_screen)
        return MetroidElementLayer::Helmet;
      switch (state.wide_count)
      {
      case 1:
        return m.layer == 2 && state.map_screen ? MetroidElementLayer::Map2 :
                                                  MetroidElementLayer::HUD;
      case 2:
        return state.map_screen ? MetroidElementLayer::Helmet : MetroidElementLayer::RadarDot;
      case 3:
        return state.map_screen ? MetroidElementLayer::MapMap :
                                  MetroidElementLayer::VisorRadarHint;
      case 4:
        if (state.map_screen)
          return MetroidElementLayer::MapMap;
        state.helmet_index = 4;
        return MetroidElementLayer::Helmet;
      case 5:
        return state.map_screen ? MetroidElementLayer::MapNorth : MetroidElementLayer::Map;
      case 6:
        return state.map_screen ? MetroidElementLayer::MapMap : MetroidElementLayer::UnknownHUD;
      default:
        return MetroidElementLayer::UnknownHUD;
      }
    }
    return MetroidElementLayer::UnknownHUD;
  }
  if (Far750(m) && Near(m.v, 6000, 220))
  {
    state.morphball_active = true;
    state.map_screen = false;
    state.scan_visor = false;
    state.inventory = false;
    state.cinematic = false;
    return MetroidElementLayer::MorphballWorld;
  }
  if (Far750(m) && Near(m.v, 5021, 180))
  {
    state.xray_visor = true;
    state.morphball_active = false;
    state.map_screen = false;
    state.scan_visor = false;
    state.inventory = false;
    state.cinematic = false;
    return MetroidElementLayer::XRayWorld;
  }
  if (Far750(m) && Near(m.h, 8951, 220))
  {
    ++state.normal_count;
    state.cinematic = false;
    state.morphball_active = false;
    state.map_screen = false;
    state.inventory = false;
    if (state.wide_count > 0)
    {
      state.after_cursor = true;
      state.helmet_index = state.wide_count;
      return MetroidElementLayer::WiiReticle;
    }
    switch (state.normal_count)
    {
    case 1:
      return MetroidElementLayer::Gun;
    case 2:
      return MetroidElementLayer::World;
    case 3:
    case 4:
      return MetroidElementLayer::Reticle;
    default:
      return MetroidElementLayer::UnknownWorld;
    }
  }
  if (Far750(m) && Near(m.h, 6594, 140))
    return MetroidElementLayer::WiiWorld;
  if (Near(m.h, 5830, 140) && Near(m.v, 3264, 140) && Near(m.f, 7500, 100))
    return MetroidElementLayer::UnknownWorld;
  if (Near(m.h, 8213, 180) && Near(m.v, 5400, 180) && Near(m.n, 100, 20) &&
      Far4096(m))
    return MetroidElementLayer::UnknownWorld;

  state.cinematic = true;
  state.morphball_active = false;
  return MetroidElementLayer::CinematicWorld;
}

MetroidElementLayer MetroidElementClassifier::ClassifyPrime3Ortho(State& state,
                                                                  const RoundedMetrics& m)
{
  if (std::abs(m.t) == 44800 || std::abs(m.b) == 44800)
    state.vres = 448;
  else if (std::abs(m.t) == 52800 || std::abs(m.b) == 52800)
    state.vres = 528;

  const int half_vres = state.vres * 50;
  const int full_vres = state.vres * 100;
  if (m.l == -32000 && m.t == half_vres && m.r == 32000 && m.b == -half_vres &&
      Near(m.n, -100, 20) && Near(m.f, -1000, 50))
  {
    return MetroidElementLayer::Unknown2D;
  }
  if (m.layer == 0 && m.l == 0 && m.t == half_vres && m.r == 32000 && m.b == 0 &&
      Near(m.n, -100, 20) && Near(m.f, 100, 20))
  {
    state.map_screen = true;
    state.morphball_active = false;
    return MetroidElementLayer::Map0;
  }
  if (m.layer == 1 && m.l == -3200 && m.r == 3200 && m.t == 3200 && m.b == -3200 &&
      Near(m.n, -100, 20) && Near(m.f, -1000, 50))
  {
    return MetroidElementLayer::Shadow2D;
  }
  if (m.layer == 1 && m.l == -32000 && m.t == half_vres && m.r == 32000 &&
      m.b == -half_vres && Near(m.n, -409600, 1400) && Near(m.f, 409600, 1400))
  {
    state.map_screen = true;
    state.morphball_active = false;
    return MetroidElementLayer::Map1;
  }
  if (m.layer > 0 && m.l == 0 && m.t == full_vres && m.r == 64000 && m.b == 0 &&
      Near(m.n, -409600, 1400) && Near(m.f, 409600, 1400))
  {
    return MetroidElementLayer::EFBCopy;
  }
  if (m.layer > 1 && m.l == 0 && m.t == 0 && m.r == 640 && m.b == 528 &&
      Near(m.n, -100, 20) && Near(m.f, 100, 20))
  {
    return MetroidElementLayer::MousePointer;
  }
  if (m.layer > 1 && m.l == -32000 && m.t == half_vres && m.r == 32000 &&
      m.b == -half_vres && Near(m.n, -409600, 1400) && Near(m.f, 409600, 1400))
  {
    return MetroidElementLayer::ScreenFade;
  }
  return MetroidElementLayer::Unknown2D;
}

std::string_view MetroidElementProfileToININame(MetroidElementProfile profile)
{
  for (const ProfileName& name : PROFILE_NAMES)
  {
    if (name.profile == profile)
      return name.ini_name;
  }
  return "none";
}

std::string_view MetroidElementProfileToDisplayName(MetroidElementProfile profile)
{
  for (const ProfileName& name : PROFILE_NAMES)
  {
    if (name.profile == profile)
      return name.display_name;
  }
  return "None";
}

std::optional<MetroidElementProfile> MetroidElementProfileFromString(std::string_view value)
{
  const std::string normalized = NormalizeKey(value);
  for (const ProfileName& name : PROFILE_NAMES)
  {
    if (NormalizeKey(name.ini_name) == normalized || NormalizeKey(name.display_name) == normalized)
      return name.profile;
  }
  return std::nullopt;
}

std::vector<MetroidElementProfile> GetMetroidElementProfiles()
{
  std::vector<MetroidElementProfile> profiles;
  profiles.reserve(PROFILE_NAMES.size());
  for (const ProfileName& name : PROFILE_NAMES)
    profiles.push_back(name.profile);
  return profiles;
}

std::string_view MetroidElementLayerToININame(MetroidElementLayer layer)
{
  for (const LayerName& name : LAYER_NAMES)
  {
    if (name.layer == layer)
      return name.ini_name;
  }
  return "METROID_UNKNOWN";
}

std::string_view MetroidElementLayerToDisplayName(MetroidElementLayer layer)
{
  for (const LayerName& name : LAYER_NAMES)
  {
    if (name.layer == layer)
      return name.display_name;
  }
  return "Unknown";
}

std::optional<MetroidElementLayer> MetroidElementLayerFromString(std::string_view value)
{
  const std::string normalized = NormalizeKey(value);
  for (const LayerName& name : LAYER_NAMES)
  {
    if (NormalizeKey(name.ini_name) == normalized || NormalizeKey(name.display_name) == normalized)
      return name.layer;
  }
  return std::nullopt;
}

std::vector<MetroidElementLayer> GetMetroidElementLayers()
{
  std::vector<MetroidElementLayer> layers;
  layers.reserve(LAYER_NAMES.size());
  for (const LayerName& name : LAYER_NAMES)
    layers.push_back(name.layer);
  return layers;
}
