/** @fileoverview Fast, bounded PNG encoding for ordinary 8-bit captures. */
#include "png.hpp"

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QImageWriter>
#include <QIODevice>
#include <QPixelFormat>
#include <Qt>
#include <QtEndian>
#include <QtTypes>

#include <libdeflate.h>

#include <array>
#include <cstring>
#include <memory>

namespace {
bool writeChunk(QIODevice &device, const char (&type)[5], const QByteArray &data) {
  std::array<char, 8> header{};
  qToBigEndian(static_cast<quint32>(data.size()), header.data());
  std::memcpy(header.data() + 4, type, 4);
  quint32 crc = libdeflate_crc32(0, type, 4);
  crc = libdeflate_crc32(crc, data.constData(), data.size());
  std::array<char, 4> checksum{};
  qToBigEndian(crc, checksum.data());
  return device.write(header.data(), header.size()) == qsizetype(header.size()) &&
         device.write(data) == data.size() &&
         device.write(checksum.data(), checksum.size()) == qsizetype(checksum.size());
}
} // namespace

bool writePng(const QImage &image, QIODevice &device) {
  if (image.isNull() || !device.isWritable())
    return false;

  const bool alpha = image.hasAlphaChannel();
  const QPixelFormat format = image.pixelFormat();
  const qsizetype rowBytes = qsizetype(image.width()) * (alpha ? 4 : 3);
  const qsizetype stride = rowBytes + 1;
  // libdeflate compresses a whole buffer. Keep its extra working memory
  // bounded for long scroll captures, and let Qt preserve uncommon formats
  // and metadata without maintaining a second general-purpose PNG codec.
  constexpr qsizetype maxFilteredBytes = qsizetype(128) * 1024 * 1024;
  if (image.height() > maxFilteredBytes / stride || image.depth() > 32 ||
      format.redSize() > 8 || format.greenSize() > 8 ||
      format.blueSize() > 8 || format.alphaSize() > 8 ||
      image.colorSpace().isValid() || !image.textKeys().isEmpty() ||
      !image.offset().isNull()) {
    QImageWriter writer(&device, "PNG");
    writer.setCompression(11); // Qt's 0..100 scale maps this to zlib level 1.
    return writer.write(image);
  }

  const QImage pixels = image.convertToFormat(alpha ? QImage::Format_RGBA8888
                                                   : QImage::Format_RGB888);
  if (pixels.isNull())
    return false;
  QByteArray filtered(stride * pixels.height(), Qt::Uninitialized);
  for (int y = 0; y < pixels.height(); ++y) {
    char *row = filtered.data() + qsizetype(y) * stride;
    // No row prediction: screenshots compress well at level 1, and this
    // also avoids an expensive inverse filter in downstream PNG readers.
    *row = 0;
    std::memcpy(row + 1, pixels.constScanLine(y), rowBytes);
  }
  const std::unique_ptr<libdeflate_compressor, decltype(&libdeflate_free_compressor)>
      compressor(libdeflate_alloc_compressor(1), libdeflate_free_compressor);
  if (!compressor)
    return false;
  const auto capacity = static_cast<qsizetype>(
      libdeflate_zlib_compress_bound(compressor.get(), filtered.size()));
  QByteArray compressed(capacity, Qt::Uninitialized);
  const size_t bytes = libdeflate_zlib_compress(
      compressor.get(), filtered.constData(), filtered.size(),
      compressed.data(), compressed.size());
  if (bytes == 0)
    return false;
  compressed.truncate(static_cast<qsizetype>(bytes));

  QByteArray header(13, '\0');
  qToBigEndian(static_cast<quint32>(pixels.width()), header.data());
  qToBigEndian(static_cast<quint32>(pixels.height()), header.data() + 4);
  header[8] = 8; // bits per channel
  header[9] = alpha ? 6 : 2; // RGBA or RGB; standard PNG, non-interlaced
  if (device.write("\x89PNG\r\n\x1a\n", 8) != 8 ||
      !writeChunk(device, "IHDR", header))
    return false;
  if (image.dotsPerMeterX() > 0 && image.dotsPerMeterY() > 0) {
    QByteArray resolution(9, '\0');
    qToBigEndian(static_cast<quint32>(image.dotsPerMeterX()), resolution.data());
    qToBigEndian(static_cast<quint32>(image.dotsPerMeterY()), resolution.data() + 4);
    resolution[8] = 1; // metres
    if (!writeChunk(device, "pHYs", resolution))
      return false;
  }
  return writeChunk(device, "IDAT", compressed) && writeChunk(device, "IEND", {});
}
