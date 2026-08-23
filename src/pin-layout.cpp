/** @fileoverview Implements pinned-window stacking and dispatch helpers. */
#include "pin-layout.hpp"

#include <algorithm>
#include <cmath>

QSize pinFrameSize(const QSize &screenSize) {
  constexpr int width = 200;
  const double aspect =
      screenSize.width() > 0 && screenSize.height() > 0
          ? static_cast<double>(screenSize.height()) / screenSize.width()
          : 9.0 / 16.0;
  const int height = std::clamp(static_cast<int>(std::lround(width * aspect)),
                                width / 4, width * 2);
  return {width, height};
}

QPoint pinPackedPosition(const QVector<QRect> &blockers,
                         const QSize &screenSize, const QSize &frame, int gap,
                         int margin) {
  int x = screenSize.width() - margin - frame.width();
  for (int column = 0; column < 8; ++column) {
    int y = screenSize.height() - margin - frame.height();
    while (y >= margin) {
      const QRect candidate(x, y, frame.width(), frame.height());
      int lowestTop = -1;
      for (const QRect &blocker : blockers) {
        if (candidate.intersects(blocker))
          lowestTop = std::max(lowestTop, blocker.top());
      }
      if (lowestTop < 0)
        return {x, y};
      // Climb to one gap above the lowest pin in the way, then look again:
      // the spot up there may graze another one.
      y = lowestTop - gap - frame.height();
    }
    x -= frame.width() + gap;
  }
  return {screenSize.width() - margin - frame.width(),
          screenSize.height() - margin - frame.height()};
}

bool pinInColumn(const QRect &rect, const QSize &screenSize, int margin) {
  constexpr int tolerance = 6;
  return std::abs(rect.right() + 1 - (screenSize.width() - margin)) <=
         tolerance;
}

namespace {
// The whole expression is one dispatch argument; the title has a space in
// it, so the selector is quoted inside the expression rather than around it.
QString windowSelector(const QString &title) {
  return QStringLiteral("window = \"title:^(%1)$\"").arg(title);
}
} // namespace

QString pinFloatDispatch(const QString &title) {
  return QStringLiteral("hl.dsp.window.float({ %1 })")
      .arg(windowSelector(title));
}

QString pinPinDispatch(const QString &title) {
  return QStringLiteral("hl.dsp.window.pin({ %1 })").arg(windowSelector(title));
}

QString pinMoveDispatch(const QString &title, int x, int y) {
  return QStringLiteral(
             "hl.dsp.window.move({ x = %1, y = %2, relative = false, %3 })")
      .arg(x)
      .arg(y)
      .arg(windowSelector(title));
}

QString pinSwayArrangeCommand(const QString &title, int x, int y) {
  return QStringLiteral("[title=\"^%1$\"] floating enable, sticky enable, "
                        "move absolute position %2 %3")
      .arg(title)
      .arg(x)
      .arg(y);
}

QString pinSwayMoveCommand(const QString &title, int x, int y) {
  return QStringLiteral("[title=\"^%1$\"] move absolute position %2 %3")
      .arg(title)
      .arg(x)
      .arg(y);
}
