// SPDX-License-Identifier: LGPL-3.0-only
#include "helper.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#if __has_include(<linux/close_range.h>)
#include <linux/close_range.h>
#endif
#include <cstddef>
#include <limits>
#include <sys/syscall.h>
#include <unistd.h>

namespace MpvCapture {
namespace {

// Called in the pre-exec child. Use only syscalls/async-signal-safe calls;
// the actual inherited table can contain fds above a lowered RLIMIT_NOFILE.
bool closeOnExecExceptSocket() {
#if defined(SYS_close_range) && defined(CLOSE_RANGE_CLOEXEC)
    if (::syscall(SYS_close_range, ChildSocket + 1, ~0u, CLOSE_RANGE_CLOEXEC) == 0)
        return true;
#endif
    struct Entry { quint64 inode; qint64 offset; unsigned short length; unsigned char type; char name[1]; };
    Fd entries(::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!entries)
        return false;
    alignas(Entry) char buffer[4096];
    for (;;) {
        const long n = ::syscall(SYS_getdents64, entries.get(), buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n == 0;
        for (long at = 0; at < n;) {
            const auto *entry = reinterpret_cast<const Entry *>(buffer + at);
            int fd = 0;
            const char *end = buffer + at + entry->length;
            const char *p = buffer + at + offsetof(Entry, name);
            for (; p < end && *p >= '0' && *p <= '9'; ++p) {
                if (fd > (std::numeric_limits<int>::max() - (*p - '0')) / 10)
                    return false;
                fd = fd * 10 + (*p - '0');
            }
            if (p < end && *p == '\0' && fd > ChildSocket && ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
                return false;
            at += entry->length;
        }
    }
}

} // namespace

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
    process.setChildProcessModifier([fd = inherited.get()] {
        if (::dup2(fd, ChildSocket) < 0)
            ::_exit(126);
        // Mark, rather than close, so Qt can still signal an exec error on
        // its private fd. The protocol endpoint is the only extra fd at exec.
        if (!closeOnExecExceptSocket())
            ::_exit(126);
    });
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(executable, args, QIODevice::NotOpen);
    process.setChildProcessModifier({});
    return std::move(local);
}

} // namespace MpvCapture
