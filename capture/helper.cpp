// SPDX-License-Identifier: LGPL-3.0-only
#include "helper.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/close_range.h>
#include <limits>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace MpvCapture {

QString captureHelperPath() {
    Dl_info info{};
    // Resolve from this library, not the application path, cwd, or $PATH.
    static const char anchor = 0;
    if (!::dladdr(&anchor, &info))
        return {};
    const auto library = QFileInfo(QFile::decodeName(info.dli_fname)).canonicalFilePath();
    if (library.isEmpty())
        return {};
    if (library == QFileInfo(QStringLiteral(MPV_CAPTURE_BUILD_LIB)).canonicalFilePath())
        return QStringLiteral(MPV_CAPTURE_BUILD_EXECUTABLE);
    return QDir::cleanPath(QFileInfo(library).absolutePath() + '/' + MPV_CAPTURE_FROM_LIB);
}

Fd startChild(QProcess &process, const QString &executable, const QStringList &args) {
    if (!QFileInfo(executable).isAbsolute() || process.state() != QProcess::NotRunning)
        return {};
    auto [local, remote] = socketPair();
    if (!local)
        return {};
    // The child endpoint can initially be fd 3. Always duplicate from a
    // higher fd: dup2 then reliably clears CLOEXEC on the inherited fd.
    Fd inherited(::fcntl(remote.get(), F_DUPFD_CLOEXEC, ChildSocket + 1));
    const int flags = ::fcntl(local.get(), F_GETFL);
    if (!inherited || flags < 0 || ::fcntl(local.get(), F_SETFL, flags | O_NONBLOCK) < 0)
        return {};
    rlimit limits{};
    if (::getrlimit(RLIMIT_NOFILE, &limits) < 0)
        return {};
    process.setChildProcessModifier([fd = inherited.get(), limit = limits.rlim_max] {
        if (::dup2(fd, ChildSocket) < 0)
            ::_exit(126);
        // Mark, rather than close, so Qt can still signal an exec error on
        // its private fd. The protocol endpoint is the only extra fd at exec.
#ifdef SYS_close_range
        if (::syscall(SYS_close_range, ChildSocket + 1, ~0u, CLOSE_RANGE_CLOEXEC) == 0)
            return;
#endif
        // Older kernels lack CLOSE_RANGE_CLOEXEC. Keep the startup pipe
        // usable before exec and use the hard descriptor limit on that path.
        if (limit > rlim_t(std::numeric_limits<int>::max()))
            ::_exit(126);
        for (int other = ChildSocket + 1; other < int(limit); ++other) {
            if (::fcntl(other, F_SETFD, FD_CLOEXEC) < 0 && errno != EBADF)
                ::_exit(126);
        }
    });
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(executable, args, QIODevice::NotOpen);
    process.setChildProcessModifier({});
    return std::move(local);
}

} // namespace MpvCapture
