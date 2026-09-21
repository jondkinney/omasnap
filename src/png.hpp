/** @fileoverview Lossless PNG output tuned for screenshot latency. */
#pragma once

class QImage;
class QIODevice;

/** Encode on a worker. Preserves pixels and image metadata; does not close
 *  or commit the device. Returns false on encoding or write failure. */
[[nodiscard]] bool writePng(const QImage &image, QIODevice &device);
