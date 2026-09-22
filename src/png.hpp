/** @fileoverview Lossless PNG output tuned for screenshot latency. */
#pragma once

class QImage;
class QIODevice;
class QSize;

/** Store/read the capture's logical dimensions without changing its pixels.
 *  PNG text survives file moves and clipboard transfers without a sidecar. */
void setPngLogicalSize(QImage &image, const QSize &size);
[[nodiscard]] QSize pngLogicalSize(const QImage &image);

/** Encode on a worker. Preserves pixels and image metadata; does not close
 *  or commit the device. Returns false on encoding or write failure. */
[[nodiscard]] bool writePng(const QImage &image, QIODevice &device);
