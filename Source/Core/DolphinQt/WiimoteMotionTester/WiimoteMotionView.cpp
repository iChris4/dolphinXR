// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/WiimoteMotionTester/WiimoteMotionView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QWheelEvent>

#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#include "Core/HW/WiimoteEmu/Camera.h"
#include "DolphinQt/Settings.h"

namespace
{
using Common::Vec3;

constexpr float DEFAULT_AZIMUTH = 22 * float(MathUtil::TAU / 360);
constexpr float DEFAULT_ELEVATION = 18 * float(MathUtil::TAU / 360);
constexpr float DEFAULT_RADIUS = 0.5f;
constexpr float MIN_RADIUS = 0.15f;
constexpr float MAX_RADIUS = 8.0f;
constexpr float FIELD_OF_VIEW = 45 * float(MathUtil::TAU / 360);
constexpr float NEAR_DEPTH = 0.01f;

// Wii Remote: 148 x 36 x 31 mm. Length along Y, thickness along Z.
constexpr Vec3 HALF_EXTENTS{0.018f, 0.074f, 0.0155f};
// Marks sit just above their face.
constexpr float MARK_OFFSET = 0.0004f;

// The scene's screen: a 1 m wide 16:9 TV under the sensor bar, in the plane y = 0.
constexpr float SCREEN_HALF_WIDTH = 0.5f;
constexpr float SCREEN_TOP = -0.03f;
constexpr float SCREEN_BOTTOM = SCREEN_TOP - 2 * SCREEN_HALF_WIDTH * 9 / 16;
constexpr float FLOOR_HEIGHT = -1.0f;
constexpr float DEFAULT_DISTANCE = 2.0f;

const QColor AXIS_COLORS[3] = {QColor(0xe0, 0x45, 0x45), QColor(0x3c, 0xb3, 0x50),
                               QColor(0x3c, 0x7d, 0xe0)};
const QColor POINTER_COLOR(0x2d, 0x9c, 0xdb);
const QColor GHOST_COLOR(0xff, 0x9f, 0x1c);
const QColor IR_COLOR(0xe0, 0x24, 0x8e);

struct SceneCamera
{
  Vec3 eye;
  Vec3 right;
  Vec3 up;
  Vec3 forward;
  QPointF center;
  float focal = 1;

  // View space: x right, y up, z depth in front of the camera.
  Vec3 ToView(const Vec3& point) const
  {
    const Vec3 delta = point - eye;
    return {delta.Dot(right), delta.Dot(up), delta.Dot(forward)};
  }

  QPointF ProjectView(const Vec3& view) const
  {
    return {center.x() + view.x / view.z * focal, center.y() - view.y / view.z * focal};
  }

  std::optional<QPointF> Project(const Vec3& point) const
  {
    const Vec3 view = ToView(point);
    if (view.z < NEAR_DEPTH)
      return std::nullopt;
    return ProjectView(view);
  }

  // Clips to the near plane.
  std::optional<QLineF> ProjectSegment(const Vec3& a, const Vec3& b) const
  {
    Vec3 va = ToView(a);
    Vec3 vb = ToView(b);
    if (va.z < NEAR_DEPTH && vb.z < NEAR_DEPTH)
      return std::nullopt;
    if (va.z < NEAR_DEPTH)
      va = va + (vb - va) * ((NEAR_DEPTH - va.z) / (vb.z - va.z));
    else if (vb.z < NEAR_DEPTH)
      vb = vb + (va - vb) * ((NEAR_DEPTH - vb.z) / (va.z - vb.z));
    return QLineF(ProjectView(va), ProjectView(vb));
  }

  // Clips to the near plane (Sutherland-Hodgman against one plane).
  QPolygonF ProjectPolygon(const std::vector<Vec3>& points) const
  {
    QPolygonF result;
    const size_t count = points.size();
    for (size_t i = 0; i != count; ++i)
    {
      const Vec3 a = ToView(points[i]);
      const Vec3 b = ToView(points[(i + 1) % count]);
      const bool a_in = a.z >= NEAR_DEPTH;
      const bool b_in = b.z >= NEAR_DEPTH;
      if (a_in)
        result << ProjectView(a);
      if (a_in != b_in)
        result << ProjectView(a + (b - a) * ((NEAR_DEPTH - a.z) / (b.z - a.z)));
    }
    return result;
  }
};

SceneCamera MakeCamera(const Vec3& target, float azimuth, float elevation, float radius,
                       QSizeF size)
{
  // Azimuth 0 looks from behind the remote (+Y, the player's side) toward the screen (-Y).
  const Vec3 direction(-std::sin(azimuth) * std::cos(elevation),
                       std::cos(azimuth) * std::cos(elevation), std::sin(elevation));
  SceneCamera camera;
  camera.eye = target + direction * radius;
  camera.forward = (target - camera.eye).Normalized();
  camera.right = camera.forward.Cross(Vec3(0, 0, 1)).Normalized();
  camera.up = camera.right.Cross(camera.forward);
  camera.center = QPointF(size.width() / 2, size.height() / 2);
  camera.focal = float(size.height() / 2 / std::tan(FIELD_OF_VIEW / 2));
  return camera;
}

QColor Shade(const QColor& color, float intensity, int alpha = 255)
{
  return QColor(std::clamp(int(color.red() * intensity), 0, 255),
                std::clamp(int(color.green() * intensity), 0, 255),
                std::clamp(int(color.blue() * intensity), 0, 255), alpha);
}

struct Face
{
  Vec3 normal;
  // Two in-plane axes: the face spans +-u and +-v around normal * extent.
  Vec3 u;
  Vec3 v;
};

constexpr std::array<Face, 6> BOX_FACES = {{
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
    {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
    {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}},
}};

// The box's half extent along a body axis.
float Extent(const Vec3& axis)
{
  return std::abs(axis.Dot(HALF_EXTENTS));
}

std::vector<Vec3> FaceCorners(const Face& face)
{
  const Vec3 center = face.normal * Extent(face.normal);
  const Vec3 u = face.u * Extent(face.u);
  const Vec3 v = face.v * Extent(face.v);
  return {center - u - v, center + u - v, center + u + v, center - u + v};
}

class BoxPainter
{
public:
  BoxPainter(QPainter& painter, const SceneCamera& camera, const Vec3& position,
             const Common::Quaternion& device_to_world)
      : m_painter(painter), m_camera(camera), m_position(position), m_rotation(device_to_world)
  {
  }

  Vec3 ToWorld(const Vec3& body) const { return m_position + m_rotation * body; }

  std::vector<Vec3> ToWorld(const std::vector<Vec3>& body) const
  {
    std::vector<Vec3> result;
    result.reserve(body.size());
    for (const Vec3& point : body)
      result.push_back(ToWorld(point));
    return result;
  }

  bool IsFaceVisible(const Face& face) const
  {
    const Vec3 center = ToWorld(face.normal * Extent(face.normal));
    return (m_rotation * face.normal).Dot(m_camera.eye - center) > 0;
  }

  float Lighting(const Face& face) const
  {
    const Vec3 light = Vec3(-0.4f, 0.6f, 0.8f).Normalized();
    return 0.55f + 0.45f * std::max(0.f, (m_rotation * face.normal).Dot(light));
  }

  void DrawPolygon(const std::vector<Vec3>& body, const QBrush& brush, const QPen& pen)
  {
    const QPolygonF polygon = m_camera.ProjectPolygon(ToWorld(body));
    if (polygon.size() < 3)
      return;
    m_painter.setBrush(brush);
    m_painter.setPen(pen);
    m_painter.drawPolygon(polygon);
  }

  // A rectangle on a face, in the face's (u, v) coordinates.
  void DrawFaceRect(const Face& face, float u0, float u1, float v0, float v1, const QColor& color)
  {
    const Vec3 base = face.normal * (Extent(face.normal) + MARK_OFFSET);
    DrawPolygon({base + face.u * u0 + face.v * v0, base + face.u * u1 + face.v * v0,
                 base + face.u * u1 + face.v * v1, base + face.u * u0 + face.v * v1},
                color, Qt::NoPen);
  }

  void DrawFaceCircle(const Face& face, float u, float v, float radius, const QColor& color)
  {
    const Vec3 base = face.normal * (Extent(face.normal) + MARK_OFFSET);
    std::vector<Vec3> points;
    constexpr int SEGMENTS = 16;
    for (int i = 0; i != SEGMENTS; ++i)
    {
      const float angle = float(MathUtil::TAU) * float(i) / SEGMENTS;
      points.push_back(base + face.u * (u + radius * std::cos(angle)) +
                       face.v * (v + radius * std::sin(angle)));
    }
    DrawPolygon(points, color, Qt::NoPen);
  }

  void DrawSolid(const QColor& body_color, const QColor& outline)
  {
    const QColor mark_color(0x50, 0x55, 0x5c);
    for (const Face& face : BOX_FACES)
    {
      if (!IsFaceVisible(face))
        continue;
      DrawPolygon(FaceCorners(face), Shade(body_color, Lighting(face)), QPen(outline, 1));

      if (face.normal.z > 0)
      {
        // Button face. The IR end is -Y.
        DrawFaceRect(face, -0.008f, 0.008f, -0.048f, -0.042f, mark_color);
        DrawFaceRect(face, -0.003f, 0.003f, -0.053f, -0.037f, mark_color);
        DrawFaceCircle(face, 0, -0.022f, 0.0055f, mark_color);
        DrawFaceCircle(face, 0, 0.004f, 0.003f, QColor(0x5f, 0x8f, 0xc8));
        DrawFaceCircle(face, 0, 0.040f, 0.0035f, mark_color);
        DrawFaceCircle(face, 0, 0.052f, 0.0035f, mark_color);
        for (int i = 0; i != 4; ++i)
        {
          const float u = -0.0075f + 0.005f * float(i);
          DrawFaceRect(face, u - 0.0012f, u + 0.0012f, 0.064f, 0.0665f, QColor(0x3c, 0x7d, 0xe0));
        }
      }
      else if (face.normal.y < 0)
      {
        // IR window.
        DrawFaceRect(face, -0.014f, 0.014f, -0.011f, 0.011f, QColor(0x22, 0x18, 0x20));
      }
      else if (face.normal.z < 0)
      {
        // B trigger.
        DrawFaceRect(face, -0.007f, 0.007f, -0.056f, -0.034f, mark_color);
      }
      else if (face.normal.y > 0)
      {
        // Extension connector.
        DrawFaceRect(face, -0.009f, 0.009f, -0.004f, 0.004f, QColor(0x30, 0x30, 0x30));
      }
    }
  }

  void DrawWireframe(const QColor& color)
  {
    // Translucent visible faces, the IR end more opaque, then every edge.
    for (const Face& face : BOX_FACES)
    {
      if (!IsFaceVisible(face))
        continue;
      QColor fill = color;
      fill.setAlpha(face.normal.y < 0 ? 140 : 40);
      DrawPolygon(FaceCorners(face), fill, Qt::NoPen);
    }

    QPen pen(color, 1.5);
    pen.setStyle(Qt::DashLine);
    m_painter.setPen(pen);
    for (int axis = 0; axis != 3; ++axis)
    {
      for (int a = -1; a <= 1; a += 2)
      {
        for (int b = -1; b <= 1; b += 2)
        {
          Vec3 start, end;
          const float sa = float(a), sb = float(b);
          if (axis == 0)
          {
            start = Vec3(-HALF_EXTENTS.x, sa * HALF_EXTENTS.y, sb * HALF_EXTENTS.z);
            end = Vec3(HALF_EXTENTS.x, sa * HALF_EXTENTS.y, sb * HALF_EXTENTS.z);
          }
          else if (axis == 1)
          {
            start = Vec3(sa * HALF_EXTENTS.x, -HALF_EXTENTS.y, sb * HALF_EXTENTS.z);
            end = Vec3(sa * HALF_EXTENTS.x, HALF_EXTENTS.y, sb * HALF_EXTENTS.z);
          }
          else
          {
            start = Vec3(sa * HALF_EXTENTS.x, sb * HALF_EXTENTS.y, -HALF_EXTENTS.z);
            end = Vec3(sa * HALF_EXTENTS.x, sb * HALF_EXTENTS.y, HALF_EXTENTS.z);
          }
          if (const auto line = m_camera.ProjectSegment(ToWorld(start), ToWorld(end)))
            m_painter.drawLine(*line);
        }
      }
    }
  }

  // Short body axes sticking out of the +X, +Y and +Z faces. Stubs on hidden faces are drawn
  // before the box so that it covers them.
  void DrawAxisStubs(bool visible_faces)
  {
    const std::array<Vec3, 3> axes = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    for (size_t i = 0; i != axes.size(); ++i)
    {
      const Face& face = BOX_FACES[i * 2];
      if (IsFaceVisible(face) != visible_faces)
        continue;
      const float extent = Extent(axes[i]);
      const auto line =
          m_camera.ProjectSegment(ToWorld(axes[i] * extent), ToWorld(axes[i] * (extent + 0.025f)));
      if (!line)
        continue;
      m_painter.setPen(QPen(AXIS_COLORS[i], 2));
      m_painter.drawLine(*line);
    }
  }

private:
  QPainter& m_painter;
  const SceneCamera& m_camera;
  Vec3 m_position;
  Common::Quaternion m_rotation;
};

// Where the remote's pointing axis (body -Y) meets the screen plane y = 0.
std::optional<Vec3> PointerHit(const Vec3& position, const Common::Quaternion& device_to_world)
{
  const Vec3 forward = device_to_world * Vec3(0, -1, 0);
  if (forward.y > -0.05f)
    return std::nullopt;
  return position + forward * (-position.y / forward.y);
}

void DrawPointer(QPainter& painter, const SceneCamera& camera, const Vec3& position,
                 const Common::Quaternion& device_to_world, const QColor& color, bool dashed)
{
  const Vec3 forward = device_to_world * Vec3(0, -1, 0);
  const std::optional<Vec3> hit = PointerHit(position, device_to_world);
  const Vec3 end = hit ? *hit : position + forward * 1.0f;

  QPen pen(color, 1.5);
  if (dashed)
    pen.setStyle(Qt::DashLine);
  painter.setPen(pen);
  if (const auto line = camera.ProjectSegment(position + forward * HALF_EXTENTS.y, end))
    painter.drawLine(*line);

  if (!hit)
    return;
  if (const auto point = camera.Project(*hit))
  {
    painter.setPen(QPen(color, 2));
    painter.setBrush(dashed ? QBrush(Qt::NoBrush) : QBrush(color));
    painter.drawEllipse(*point, 5, 5);
  }
}
}  // namespace

WiimoteMotionView::WiimoteMotionView(QWidget* parent) : QWidget(parent)
{
  ResetView();
  setMinimumSize(320, 240);
  setMouseTracking(false);
  setToolTip(tr("Drag to orbit, scroll to zoom, double-click to reset the view."));
}

QSize WiimoteMotionView::sizeHint() const
{
  return {640, 420};
}

void WiimoteMotionView::SetSnapshot(const WiimoteMotionTracker::Snapshot& snapshot, bool has_data)
{
  m_snapshot = snapshot;
  m_has_data = has_data;
  update();
}

void WiimoteMotionView::SetShowGhost(bool show)
{
  m_show_ghost = show;
  update();
}

void WiimoteMotionView::SetShowIR(bool show)
{
  m_show_ir = show;
  update();
}

void WiimoteMotionView::SetUseIRDistance(bool use)
{
  m_use_ir_distance = use;
  update();
}

void WiimoteMotionView::ResetView()
{
  m_azimuth = DEFAULT_AZIMUTH;
  m_elevation = DEFAULT_ELEVATION;
  m_radius = DEFAULT_RADIUS;
  update();
}

void WiimoteMotionView::mousePressEvent(QMouseEvent* event)
{
  m_drag_start = event->position();
  m_drag_azimuth = m_azimuth;
  m_drag_elevation = m_elevation;
}

void WiimoteMotionView::mouseMoveEvent(QMouseEvent* event)
{
  if (!(event->buttons() & Qt::LeftButton))
    return;
  const QPointF delta = event->position() - m_drag_start;
  constexpr float RADIANS_PER_PIXEL = 0.01f;
  m_azimuth = m_drag_azimuth - float(delta.x()) * RADIANS_PER_PIXEL;
  m_elevation = std::clamp(m_drag_elevation + float(delta.y()) * RADIANS_PER_PIXEL, -1.45f, 1.45f);
  update();
}

void WiimoteMotionView::mouseDoubleClickEvent(QMouseEvent*)
{
  ResetView();
}

void WiimoteMotionView::wheelEvent(QWheelEvent* event)
{
  const float steps = float(event->angleDelta().y()) / 120;
  m_radius = std::clamp(m_radius * std::pow(0.85f, steps), MIN_RADIUS, MAX_RADIUS);
  update();
  event->accept();
}

void WiimoteMotionView::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const bool dark = Settings::Instance().IsThemeDark();
  const QColor background = dark ? QColor(0x1e, 0x22, 0x28) : QColor(0xdc, 0xe2, 0xe9);
  const QColor grid = dark ? QColor(255, 255, 255, 28) : QColor(0, 0, 0, 30);
  const QColor text = dark ? QColor(0xd0, 0xd4, 0xda) : QColor(0x30, 0x34, 0x3a);
  painter.fillRect(rect(), background);

  const auto& s = m_snapshot;
  const float distance = std::clamp(
      m_use_ir_distance && s.ir_distance_valid ? s.ir_distance_m : DEFAULT_DISTANCE, 0.3f, 6.0f);
  const Vec3 position(0, distance, 0);
  const SceneCamera camera = MakeCamera(position, m_azimuth, m_elevation, m_radius, size());

  // Floor grid.
  painter.setPen(QPen(grid, 1));
  for (float x = -1.5f; x <= 1.5f + 0.01f; x += 0.5f)
  {
    if (const auto line = camera.ProjectSegment({x, -0.5f, FLOOR_HEIGHT}, {x, 4.0f, FLOOR_HEIGHT}))
      painter.drawLine(*line);
  }
  for (float y = -0.5f; y <= 4.0f + 0.01f; y += 0.5f)
  {
    if (const auto line = camera.ProjectSegment({-1.5f, y, FLOOR_HEIGHT}, {1.5f, y, FLOOR_HEIGHT}))
      painter.drawLine(*line);
  }

  // Screen and sensor bar.
  if (m_show_ir)
  {
    const QPolygonF screen = camera.ProjectPolygon({{SCREEN_HALF_WIDTH, 0, SCREEN_TOP},
                                                    {-SCREEN_HALF_WIDTH, 0, SCREEN_TOP},
                                                    {-SCREEN_HALF_WIDTH, 0, SCREEN_BOTTOM},
                                                    {SCREEN_HALF_WIDTH, 0, SCREEN_BOTTOM}});
    painter.setPen(QPen(QColor(0x80, 0x86, 0x90), 1.5));
    painter.setBrush(QColor(0x10, 0x14, 0x18, dark ? 230 : 200));
    if (screen.size() >= 3)
      painter.drawPolygon(screen);

    painter.setPen(QPen(QColor(0x40, 0x40, 0x44), 4, Qt::SolidLine, Qt::RoundCap));
    if (const auto bar = camera.ProjectSegment({-0.115f, 0, 0}, {0.115f, 0, 0}))
      painter.drawLine(*bar);
    const QColor led = s.ir_visible > 0 ? QColor(0xff, 0x50, 0x50) : QColor(0x70, 0x30, 0x30);
    for (const float x : {-WiimoteEmu::CameraLogic::SENSOR_BAR_LED_SEPARATION / 2,
                          WiimoteEmu::CameraLogic::SENSOR_BAR_LED_SEPARATION / 2})
    {
      if (const auto point = camera.Project({x, 0, 0}))
      {
        painter.setPen(Qt::NoPen);
        painter.setBrush(led);
        painter.drawEllipse(*point, 3.5, 3.5);
      }
    }
  }

  const Common::Quaternion device_to_world = s.orientation.Conjugate();
  const Common::Quaternion ghost_to_world = s.ghost_orientation.Conjugate();
  const bool show_ghost = m_show_ghost && s.ghost_valid && m_has_data;

  // Pointers: where the fused orientation, the VR controller and the IR dots point.
  if (m_show_ir && m_has_data)
  {
    if (show_ghost)
      DrawPointer(painter, camera, position, ghost_to_world, GHOST_COLOR, true);
    DrawPointer(painter, camera, position, device_to_world, POINTER_COLOR, false);

    if (s.ir_visible > 0)
    {
      const Vec3 hit(distance * std::tan(s.ir_yaw), 0, -distance * std::tan(s.ir_pitch));
      if (const auto point = camera.Project(hit))
      {
        painter.setPen(QPen(IR_COLOR, 2));
        painter.setBrush(Qt::NoBrush);
        constexpr double SIZE = 8;
        painter.drawLine(*point - QPointF(SIZE, 0), *point + QPointF(SIZE, 0));
        painter.drawLine(*point - QPointF(0, SIZE), *point + QPointF(0, SIZE));
        painter.drawEllipse(*point, SIZE / 2, SIZE / 2);
      }
    }
  }

  // The remote.
  BoxPainter box(painter, camera, position, device_to_world);
  box.DrawAxisStubs(false);
  const QColor body = m_has_data ? QColor(0xf2, 0xf2, 0xf2) : QColor(0x9a, 0x9a, 0x9a);
  box.DrawSolid(body, QColor(0x40, 0x44, 0x48));
  box.DrawAxisStubs(true);

  if (show_ghost)
  {
    BoxPainter ghost(painter, camera, position, ghost_to_world);
    ghost.DrawWireframe(GHOST_COLOR);
  }

  // World axes.
  const QPointF origin(width() - 38, height() - 38);
  const std::array<Vec3, 3> world_axes = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
  const std::array<QString, 3> axis_names = {tr("X"), tr("Y"), tr("Z")};
  for (size_t i = 0; i != world_axes.size(); ++i)
  {
    const QPointF tip =
        origin + QPointF(world_axes[i].Dot(camera.right), -world_axes[i].Dot(camera.up)) * 24;
    painter.setPen(QPen(AXIS_COLORS[i], 2));
    painter.drawLine(origin, tip);
    painter.drawText(tip + QPointF(-4, 4) + (tip - origin) * 0.3, axis_names[i]);
  }

  // Legend.
  painter.setPen(text);
  const QFontMetrics metrics(font());
  int y = height() - 8 - metrics.height() * 2;
  const auto legend = [&](const QColor& color, const QString& label, bool dashed) {
    QPen pen(color, 2);
    if (dashed)
      pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.drawLine(QPointF(10, y - metrics.ascent() / 2.0),
                     QPointF(30, y - metrics.ascent() / 2.0));
    painter.setPen(text);
    painter.drawText(QPointF(36, y), label);
    y += metrics.height();
  };
  if (m_show_ir)
  {
    y -= metrics.height();
    legend(POINTER_COLOR, tr("Pointer from the fused orientation"), false);
    legend(IR_COLOR, tr("Pointer from the IR dots"), false);
  }
  if (show_ghost)
    legend(GHOST_COLOR, tr("VR controller"), true);

  if (!m_has_data)
  {
    painter.setPen(text);
    painter.drawText(rect(), Qt::AlignHCenter | Qt::AlignTop,
                     tr("\nNo reports from this Wii Remote yet."));
  }
}

WiimoteIRView::WiimoteIRView(QWidget* parent) : QWidget(parent)
{
  setMinimumSize(160, 120);
  setToolTip(tr("The Wii Remote's IR camera image, as seen from behind the remote."));
}

void WiimoteIRView::SetSnapshot(const WiimoteMotionTracker::Snapshot& snapshot, bool has_data)
{
  m_snapshot = snapshot;
  m_has_data = has_data;
  update();
}

void WiimoteIRView::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  // Keep the camera's 4:3 aspect ratio.
  QRectF area = rect();
  if (area.width() * 3 > area.height() * 4)
    area.setWidth(area.height() * 4 / 3);
  else
    area.setHeight(area.width() * 3 / 4);
  area.moveCenter(QRectF(rect()).center());

  painter.fillRect(area, QColor(0x10, 0x10, 0x14));
  painter.setPen(QPen(QColor(0x50, 0x50, 0x58), 1));
  painter.drawRect(area);
  painter.drawLine(QPointF(area.center().x(), area.top()),
                   QPointF(area.center().x(), area.bottom()));
  painter.drawLine(QPointF(area.left(), area.center().y()),
                   QPointF(area.right(), area.center().y()));

  using WiimoteEmu::CameraLogic;
  // Pixel x grows to the right and pixel y upward, as seen by the player behind the remote.
  const auto to_widget = [&area](float x, float y) {
    return QPointF(area.left() + x / (CameraLogic::CAMERA_RES_X - 1) * area.width(),
                   area.bottom() - y / (CameraLogic::CAMERA_RES_Y - 1) * area.height());
  };

  const auto& s = m_snapshot;
  if (m_has_data)
  {
    for (size_t i = 0; i != s.ir_dots.size(); ++i)
    {
      const auto& dot = s.ir_dots[i];
      if (!dot.visible)
        continue;
      const QPointF point = to_widget(dot.x, dot.y);
      const double radius = 2.5 + dot.size * 0.6;
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(0xff, 0xff, 0xff));
      painter.drawEllipse(point, radius, radius);
      painter.setPen(QColor(0xa0, 0xa0, 0xa8));
      painter.drawText(point + QPointF(radius + 2, -radius), QString::number(i + 1));
    }

    if (s.ir_visible > 0)
    {
      const QPointF center = to_widget((s.ir_center.x + 1) / 2 * (CameraLogic::CAMERA_RES_X - 1),
                                       (s.ir_center.y + 1) / 2 * (CameraLogic::CAMERA_RES_Y - 1));
      painter.setPen(QPen(IR_COLOR, 1.5));
      painter.drawLine(center - QPointF(6, 0), center + QPointF(6, 0));
      painter.drawLine(center - QPointF(0, 6), center + QPointF(0, 6));
    }
  }

  if (!m_has_data || s.ir_visible == 0)
  {
    painter.setPen(QColor(0x90, 0x90, 0x98));
    QString message;
    if (!m_has_data)
      message = tr("No reports");
    else if (s.ir_format == WiimoteCommon::IRReportFormat::None)
      message = tr("The reporting mode has no IR data");
    else
      message = tr("No IR dots visible");
    painter.drawText(area, Qt::AlignCenter, message);
  }
}
