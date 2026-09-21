/** @fileoverview Independently decode PNG output and verify lossless pixels. */
#include "png-smoke.hpp"

#include "png.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QIODevice>
#include <QPoint>
#include <QRandomGenerator>
#include <QSize>
#include <QString>
#include <Qt>
#include <QtTypes>

namespace {
bool roundTrip(const QImage &source, QString &error) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly) || !writePng(source, buffer)) {
    error = QStringLiteral("Could not encode PNG format %1").arg(source.format());
    return false;
  }
  // Qt/libpng is an independent reader. Compare with Qt's own PNG writer
  // too, including premultiplied-alpha rounding and high-bit-depth pixels.
  QByteArray reference;
  QBuffer referenceBuffer(&reference);
  if (!referenceBuffer.open(QIODevice::WriteOnly) || !source.save(&referenceBuffer, "PNG"))
    return false;
  const QImage decoded = QImage::fromData(bytes, "PNG");
  const QImage expected = QImage::fromData(reference, "PNG");
  if (decoded.isNull() || expected.isNull() ||
      decoded.convertToFormat(QImage::Format_RGBA64) !=
          expected.convertToFormat(QImage::Format_RGBA64) ||
      decoded.dotsPerMeterX() != expected.dotsPerMeterX() ||
      decoded.dotsPerMeterY() != expected.dotsPerMeterY() ||
      decoded.offset() != expected.offset() ||
      decoded.colorSpace() != expected.colorSpace() ||
      decoded.text() != expected.text()) {
    error = QStringLiteral("PNG pixels or metadata changed for format %1").arg(source.format());
    return false;
  }
  return true;
}

class FailingDevice : public QIODevice {
public:
  explicit FailingDevice(qint64 budget) : remaining_(budget) { open(QIODevice::WriteOnly); }
protected:
  qint64 readData(char *, qint64) override { return -1; }
  qint64 writeData(const char *, qint64 size) override {
    if (size > remaining_)
      return -1;
    remaining_ -= size;
    return size;
  }
private:
  qint64 remaining_;
};
} // namespace

bool runPngSmoke(QString &error) {
  QRandomGenerator random(71);
  for (const QSize size : {QSize(1, 1), QSize(3, 7), QSize(127, 65)}) {
    QImage source(size, QImage::Format_ARGB32);
    for (int y = 0; y < size.height(); ++y)
      for (int x = 0; x < size.width(); ++x)
        source.setPixel(x, y, random.generate());
    source.setDotsPerMeterX(3780);
    source.setDotsPerMeterY(7560);
    for (const auto format : {QImage::Format_RGB32, QImage::Format_RGB888,
                              QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied,
                              QImage::Format_RGBA8888, QImage::Format_RGBA8888_Premultiplied,
                              QImage::Format_Grayscale8, QImage::Format_Indexed8,
                              QImage::Format_Mono, QImage::Format_Grayscale16,
                              QImage::Format_RGBA64}) {
      if (!roundTrip(source.convertToFormat(format), error))
        return false;
    }
    source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    source.setOffset(QPoint(-12, 43));
    source.setText(QStringLiteral("Description"), QStringLiteral("Screenshot α with metadata"));
    if (!roundTrip(source, error))
      return false;
  }

  QImage precise(7, 3, QImage::Format_RGBA64);
  precise.fill(QColor::fromRgba64(111, 2222, 33333, 44444));
  for (const auto format : {QImage::Format_RGBA64, QImage::Format_RGB30,
                            QImage::Format_A2RGB30_Premultiplied,
                            QImage::Format_Grayscale16}) {
    if (!roundTrip(precise.convertToFormat(format), error))
      return false;
  }

  QImage image(13, 9, QImage::Format_RGBA8888);
  image.fill(Qt::transparent);
  QBuffer closed;
  if (writePng(image, closed)) {
    error = QStringLiteral("PNG output accepted a closed device");
    return false;
  }
  for (const int budget : {0, 8, 33, 55}) {
    FailingDevice failed(budget);
    if (writePng(image, failed) || writePng({}, failed)) {
      error = QStringLiteral("PNG output ignored a write failure or empty image");
      return false;
    }
  }
  // A long scroll exercises the streaming path, above the bounded encoder's
  // scratch budget. Sparse contents keep this regression inexpensive.
  QImage scroll(1024, 33000, QImage::Format_ARGB32);
  scroll.fill(Qt::white);
  scroll.setPixelColor(23, 32999, Qt::red);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly) || !writePng(scroll, buffer) ||
      QImage::fromData(bytes, "PNG").convertToFormat(scroll.format()) != scroll) {
    error = QStringLiteral("A long scrolling capture lost pixels in PNG output");
    return false;
  }
  return true;
}
