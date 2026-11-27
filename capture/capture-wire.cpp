// SPDX-License-Identifier: LGPL-3.0-only
#include "capture-wire.h"

#include <QDataStream>
#include <QIODevice>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>

namespace MpvCapture {
namespace {
constexpr int Immutable = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL;
}

QByteArray encodeSpec(const Spec &spec) {
  QByteArray body;
  QDataStream stream(&body, QIODevice::WriteOnly);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream << qint32(spec.bounds.width()) << qint32(spec.bounds.height()) << spec.fraction
         << qint64(std::chrono::duration_cast<std::chrono::nanoseconds>(spec.deadline.time_since_epoch()).count());
  return body;
}

qint64 pixelBytes(QSize size) {
  return size.width() > 0 && size.width() <= MaxEdge && size.height() > 0 && size.height() <= MaxEdge
      ? qint64(size.width()) * size.height() * 4 : 0;
}

bool validOutput(int fd, QSize size, bool sealed) {
  struct stat info {};
  const int seals = ::fcntl(fd, F_GET_SEALS);
  const auto bytes = pixelBytes(size);
  return bytes && seals >= 0 && (seals & Immutable) == (sealed ? Immutable : F_SEAL_GROW)
      && ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size == bytes;
}

bool decodeSpec(const Frame &frame, Spec &spec) {
  if (frame.kind != Kind::Request || frame.fds.size() != MaxFds || frame.body.size() != 24)
    return false;
  QDataStream stream(frame.body);
  stream.setByteOrder(QDataStream::LittleEndian);
  qint32 width = 0, height = 0;
  qint64 ns = 0;
  stream >> width >> height >> spec.fraction >> ns;
  spec.bounds = {width, height};
  spec.deadline = Clock::time_point(std::chrono::nanoseconds(ns));
  return ns > 0 && spec.deadline <= Clock::now() + CaptureBudget
      && std::isfinite(spec.fraction) && spec.fraction >= 0 && spec.fraction < 1
      && pixelBytes(spec.bounds);
}

bool validInput(int fd) {
  struct stat input {};
  const int flags = ::fcntl(fd, F_GETFL);
  return flags >= 0 && (flags & (O_ACCMODE | O_PATH)) == O_RDONLY
      && ::fstat(fd, &input) == 0 && S_ISREG(input.st_mode);
}

QByteArray encodeResult(const Result &result, QSize size) {
  QByteArray body;
  QDataStream stream(&body, QIODevice::WriteOnly);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream << qint32(size.width()) << qint32(size.height()) << qint32(result.stage)
         << qint32(result.mpvError) << qint32(result.systemError);
  body += result.code.toUtf8().left(64) + '\0' + result.reason.toUtf8().left(4096);
  return body;
}

bool decodeResult(const Frame &frame, Result &result, QSize &size) {
  if (frame.kind != Kind::Reply || !frame.fds.empty() || frame.body.size() < 21 || frame.body.size() > 4181)
    return false;
  QDataStream stream(frame.body);
  stream.setByteOrder(QDataStream::LittleEndian);
  qint32 width = 0, height = 0, stage = 0, native = 0, system = 0;
  stream >> width >> height >> stage >> native >> system;
  const auto sep = frame.body.indexOf('\0', 20);
  if (sep < 20 || sep > 84 || stage < int(Stage::Input) || stage > int(Stage::Helper))
    return false;
  size = {width, height};
  result = {QString::fromUtf8(frame.body.mid(20, sep - 20)), QString::fromUtf8(frame.body.mid(sep + 1)),
            Stage(stage), native, system};
  return result.code.isEmpty() ? stage == int(Stage::Complete) && pixelBytes(size) && native == 0 && system == 0
      : size == QSize() && stage != int(Stage::Complete);
}

} // namespace MpvCapture
