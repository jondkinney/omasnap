/** @fileoverview Tests stacked pin slots and compositor dispatch strings. */
#include "pin-layout-smoke.hpp"

#include "pin-layout.hpp"

bool runPinLayoutSmoke(QString &error) {
  const QSize screen(400, 300);
  const QSize pin(100, 80);

  // An empty corner takes the first pin snug against the margins; the next
  // ones pack one gap above whatever is there, whatever its size, and a
  // full column starts a new one to the left.
  const QPoint first = pinPackedPosition({}, screen, pin, 10, 14);
  if (first != QPoint(286, 206)) {
    error = QStringLiteral("The first pin did not land in the corner");
    return false;
  }
  const QPoint second =
      pinPackedPosition({QRect(first, pin)}, screen, pin, 10, 14);
  if (second != QPoint(286, 116)) {
    error = QStringLiteral("The second pin did not pack above the first");
    return false;
  }
  const QRect oddSize(QPoint(280, 150), QSize(110, 130));
  if (pinPackedPosition({oddSize}, screen, pin, 10, 14) != QPoint(286, 60)) {
    error = QStringLiteral("An odd-sized pin was not packed above snugly");
    return false;
  }
  const QVector<QRect> fullColumn{QRect(286, 206, 100, 80),
                                  QRect(286, 116, 100, 80),
                                  QRect(286, 26, 100, 80)};
  if (pinPackedPosition(fullColumn, screen, pin, 10, 14) !=
      QPoint(176, 206)) {
    error = QStringLiteral("A full column did not wrap to a new one");
    return false;
  }
  const QRect elsewhere(QPoint(20, 20), QSize(100, 80));
  if (pinPackedPosition({elsewhere}, screen, pin, 10, 14) !=
      QPoint(286, 206)) {
    error = QStringLiteral("A pin away from the column blocked the corner");
    return false;
  }

  // Column membership is hugging the right edge; dragging a pin away from
  // it takes the pin out of the column, whatever its height.
  if (!pinInColumn(QRect(286, 26, 100, 80), screen, 14) ||
      !pinInColumn(QRect(282, 140, 104, 120), screen, 14) ||
      pinInColumn(QRect(200, 26, 100, 80), screen, 14)) {
    error = QStringLiteral("Column membership did not follow the right edge");
    return false;
  }

  // The dispatch expressions are Lua for a Lua-configured Hyprland and the
  // classic criteria grammar for sway; a placement that silently does
  // nothing is exactly the failure these guard.
  const QString title = QStringLiteral("omasnap-pin 1234");
  if (pinFloatDispatch(title) !=
          QStringLiteral(
              "hl.dsp.window.float({ window = \"title:^(omasnap-pin 1234)$\" })") ||
      pinPinDispatch(title) !=
          QStringLiteral(
              "hl.dsp.window.pin({ window = \"title:^(omasnap-pin 1234)$\" })") ||
      pinMoveDispatch(title, 120, 40) !=
          QStringLiteral("hl.dsp.window.move({ x = 120, y = 40, relative = "
                         "false, window = \"title:^(omasnap-pin 1234)$\" })")) {
    error = QStringLiteral("Hyprland dispatch expressions were malformed");
    return false;
  }
  if (pinSwayArrangeCommand(title, 120, 40) !=
          QStringLiteral("[title=\"^omasnap-pin 1234$\"] floating enable, "
                         "sticky enable, move absolute position 120 40") ||
      pinSwayMoveCommand(title, 120, 40) !=
          QStringLiteral(
              "[title=\"^omasnap-pin 1234$\"] move absolute position 120 40")) {
    error = QStringLiteral("Sway commands were malformed");
    return false;
  }
  return true;
}
