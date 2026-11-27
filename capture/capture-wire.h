// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "wire.h"

#include <QSize>
#include <QString>
#include <chrono>

namespace MpvCapture {

using Clock = std::chrono::steady_clock;
inline constexpr auto CaptureBudget = std::chrono::seconds(7);
inline constexpr int MaxEdge = 4096;

enum class Stage { Input, Core, Context, Load, InitialFrame, Seek, SeekFrame, Render,
                   Encode, Publish, Complete, Cancelled, Helper };

struct Result {
  QString code;
  QString reason;
  Stage stage = Stage::Complete;
  int mpvError = 0;
  int systemError = 0;
};

struct Spec {
  QSize bounds;
  double fraction;
  Clock::time_point deadline;
};

QByteArray encodeSpec(const Spec &spec);
bool decodeSpec(const Frame &frame, Spec &spec);
bool validInput(int fd);
QByteArray encodeResult(const Result &result, QSize size = {});
bool decodeResult(const Frame &frame, Result &result, QSize &size);
qint64 pixelBytes(QSize size);
bool validOutput(int fd, QSize size, bool sealed);

} // namespace MpvCapture
