// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "wire.h"

#include <QStringList>

class QProcess;

namespace MpvCapture {

QString captureHelperPath();
// Configure and start an idle QProcess. The returned endpoint belongs to
// the caller; only socket #3 passes to the child.
Fd startChild(QProcess &process, const QString &executable, const QStringList &args = {});

} // namespace MpvCapture
