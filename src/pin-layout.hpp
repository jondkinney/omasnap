/** @fileoverview Provides pure pinned-window packing and dispatch helpers. */
#pragma once

#include <QPair>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

/// Where a frame of `frame` size lands so it covers none of `blockers`:
/// snug in the bottom-right corner, or one gap above whatever occupies it,
/// climbing the column and starting a new column to the left when this one
/// is full. Blockers can be any size; the pin packs against what is
/// actually there rather than onto a grid that wastes a slot for every
/// straddled boundary.
/// The frame a pin fills: the display's aspect at a fixed width, clamped
/// so a tall pivot or an ultrawide still yields a pin rather than a line;
/// 16:9 when the display cannot be asked.
[[nodiscard]] QSize pinFrameSize(const QSize &screenSize);

[[nodiscard]] QPoint pinPackedPosition(const QVector<QRect> &blockers,
                                       const QSize &screenSize,
                                       const QSize &frame, int gap,
                                       int margin);

/// Whether a pin still hugs the right edge column; dragging one away from
/// the edge takes it out of the column, and compaction leaves it alone.
[[nodiscard]] bool pinInColumn(const QRect &rect, const QSize &screenSize,
                               int margin);

/// Dispatch expressions for a Lua-configured Hyprland, which evaluates the
/// dispatch argument as Lua; the classic dispatcher grammar parses as an
/// expression there and fails while reporting success.
[[nodiscard]] QString pinFloatDispatch(const QString &title);
[[nodiscard]] QString pinPinDispatch(const QString &title);
[[nodiscard]] QString pinMoveDispatch(const QString &title, int x, int y);
[[nodiscard]] QString pinSwayArrangeCommand(const QString &title, int x, int y);
[[nodiscard]] QString pinSwayMoveCommand(const QString &title, int x, int y);

/** The hover tip for a pin control, empty outside the known controls. */
[[nodiscard]] QString pinControlTip(int index);
