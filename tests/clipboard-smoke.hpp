/** @fileoverview Declares clipboard image loading smoke checks. */
#pragma once

#include <QString>

/** Runs clipboard loading checks and writes the rendered text-card fixture. */
[[nodiscard]] bool runClipboardSmoke(const QString &outputRoot, QString &error);
