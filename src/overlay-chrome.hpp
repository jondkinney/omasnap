/** @fileoverview The chrome every full-screen overlay wears: the mode badge at
 *  the top, the hotkey guide in the corner, and the status pill along the
 *  bottom. Capture and scroll capture are the same tool in two moods, so they
 *  are drawn by the same code rather than by two that drift apart. */
#pragma once

#include <QColor>
#include <QPair>
#include <QRect>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

class QPainter;

/// The kinds of capture the tab strip across the top offers, on every
/// overlay. Region and Window are modes of the area overlay, Scroll is the
/// scroll overlay, and Fullscreen acts at once.
enum class CaptureKind { Region, Scroll, Window, Fullscreen };
struct CaptureTab {
  CaptureKind kind;
  QRectF rect;
};
[[nodiscard]] QString captureTabLabel(CaptureKind kind);
/// Tab positions for a surface of `bounds`, hanging off the top edge.
[[nodiscard]] QVector<CaptureTab> captureTabLayout(const QRect &bounds);
/// Index of the tab under `position`, or -1.
[[nodiscard]] int captureTabAt(const QVector<CaptureTab> &tabs,
                               const QPointF &position);
/// Draws the strip; `active` is lit, the tab under `cursor` is hinted.
void drawCaptureTabs(QPainter &painter, const QVector<CaptureTab> &tabs,
                     CaptureKind active, const QPointF &cursor);

/// The badge naming what the overlay is doing, centered at the top, with the ×
/// that leaves it. Returns the whole badge; `closeRect` is the × alone, for
/// hit-testing the click that closes.
QRectF drawModeBadge(QPainter &painter, const QRect &bounds,
                     const QString &label, const QColor &accent,
                     QRectF *closeRect = nullptr);

/// The key guide. Pinned to the top-right corner as two columns, moved to
/// the left when the pointer is over it so it never hides what is
/// underneath; `keepVisible` are points (selected handles) the card must
/// not cover. With `anchorAbove` set (a windowed editor's toolbar), the
/// card spreads wide instead, a few rows tall, centered over that anchor.
/// The size the key guide takes in its wide anchored form when it may be
/// at most `maxWidth` wide: rows are added until the card fits.
[[nodiscard]] QSize hotkeyLegendAnchoredSize(
    const QVector<QPair<QString, QString>> &entries, qreal maxWidth);

void drawHotkeyLegend(QPainter &painter, const QRect &bounds,
                      const QPointF &cursor,
                      const QVector<QPair<QString, QString>> &entries,
                      const QVector<QPointF> &keepVisible = {},
                      const QRectF &anchorAbove = QRectF());

/// The instruction line along the bottom.
void drawStatusPill(QPainter &painter, const QRect &bounds,
                    const QString &text);
