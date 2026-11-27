// SPDX-License-Identifier: LGPL-3.0-only
#include "wire.h"

#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace MpvCapture {
namespace {

constexpr quint32 Magic = 0x3143504d; // MPC1 on wire
constexpr quint16 Version = 1;
constexpr int HeaderSize = 24;
constexpr int ControlSize = CMSG_SPACE(MaxFds * sizeof(int));

bool validKind(quint16 kind) {
    return kind >= quint16(Kind::Probe) && kind <= quint16(Kind::Reply);
}

Io syscallError(int error) {
    switch (error) {
    case EAGAIN:
        return Io::Again;
    case EPIPE:
    case ECONNRESET:
        return Io::Closed;
    default:
        return Io::Error;
    }
}

} // namespace

Fd::~Fd() {
    if (m_fd >= 0)
        ::close(m_fd);
}

Fd &Fd::operator=(Fd &&other) noexcept {
    Fd old(std::exchange(m_fd, other.release()));
    return *this;
}

std::array<Fd, 2> socketPair() {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0)
        return {};
    return {Fd(pair[0]), Fd(pair[1])};
}

Io SeqPacket::send(const Frame &frame) {
    if (!validKind(quint16(frame.kind)) || !frame.serial || frame.body.size() > MaxBody ||
        frame.fds.size() > MaxFds)
        return Io::Protocol;
    char header[HeaderSize]{};
    qToLittleEndian(Magic, header);
    qToLittleEndian(Version, header + 4);
    qToLittleEndian(quint16(frame.kind), header + 6);
    qToLittleEndian(frame.serial, header + 8);
    qToLittleEndian(quint32(frame.body.size()), header + 16);
    qToLittleEndian(quint16(frame.fds.size()), header + 20);

    iovec segments[]{{header, sizeof(header)},
                     {const_cast<char *>(frame.body.constData()), size_t(frame.body.size())}};
    alignas(cmsghdr) char control[ControlSize]{};
    msghdr entry{};
    entry.msg_iov = segments;
    entry.msg_iovlen = std::size(segments);
    if (!frame.fds.empty()) {
        const auto bytes = frame.fds.size() * sizeof(int);
        entry.msg_control = control;
        entry.msg_controllen = CMSG_SPACE(bytes);
        auto *rights = CMSG_FIRSTHDR(&entry);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(bytes);
        for (size_t i = 0; i < frame.fds.size(); ++i) {
            const int fd = frame.fds[i].get();
            if (fd < 0)
                return Io::Protocol;
            std::memcpy(CMSG_DATA(rights) + i * sizeof(fd), &fd, sizeof(fd));
        }
    }

    ssize_t count;
    do {
        count = ::sendmsg(m_fd.get(), &entry, MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        return syscallError(errno);
    return count == HeaderSize + frame.body.size() ? Io::Ok : Io::Error;
}

Io SeqPacket::receive(Frame &frame) {
    Frame candidate;
    candidate.fds.reserve(MaxFds); // Allocate before the kernel hands us descriptors.
    char packet[HeaderSize + MaxBody];
    iovec segment{packet, sizeof(packet)};
    alignas(cmsghdr) char control[ControlSize]{};
    msghdr entry{};
    entry.msg_iov = &segment;
    entry.msg_iovlen = 1;
    entry.msg_control = control;
    entry.msg_controllen = sizeof(control);
    ssize_t count;
    do {
        count = ::recvmsg(m_fd.get(), &entry, MSG_CMSG_CLOEXEC);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        return syscallError(errno);

    // Adopt any delivered descriptor even for a truncated or invalid record.
    bool badControl = false;
    for (auto *rights = CMSG_FIRSTHDR(&entry); rights; rights = CMSG_NXTHDR(&entry, rights)) {
        if (rights->cmsg_level != SOL_SOCKET || rights->cmsg_type != SCM_RIGHTS ||
            rights->cmsg_len < CMSG_LEN(0)) {
            badControl = true;
            continue;
        }
        const auto bytes = rights->cmsg_len - CMSG_LEN(0);
        badControl |= bytes == 0 || bytes % sizeof(int) != 0;
        for (size_t i = 0; i < bytes / sizeof(int); ++i) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(rights) + i * sizeof(fd), sizeof(fd));
            Fd owned(fd);
            if (candidate.fds.size() == MaxFds)
                badControl = true;
            else
                candidate.fds.push_back(std::move(owned));
        }
    }
    if (badControl || (entry.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
        return Io::Protocol;
    if (!count && candidate.fds.empty())
        return Io::Closed;
    if (count < HeaderSize || qFromLittleEndian<quint32>(packet) != Magic ||
        qFromLittleEndian<quint16>(packet + 4) != Version ||
        qFromLittleEndian<quint16>(packet + 22) != 0)
        return Io::Protocol;
    const auto kind = qFromLittleEndian<quint16>(packet + 6);
    const auto serial = qFromLittleEndian<quint64>(packet + 8);
    const auto bodySize = qFromLittleEndian<quint32>(packet + 16);
    const auto descriptorCount = qFromLittleEndian<quint16>(packet + 20);
    if (!validKind(kind) || !serial || bodySize != count - HeaderSize ||
        descriptorCount != candidate.fds.size())
        return Io::Protocol;
    candidate.kind = Kind(kind);
    candidate.serial = serial;
    candidate.body = QByteArray(packet + HeaderSize, bodySize);
    frame = std::move(candidate);
    return Io::Ok;
}

} // namespace MpvCapture
