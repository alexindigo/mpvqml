// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QByteArray>
#include <array>
#include <utility>
#include <vector>

namespace MpvCapture {

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : m_fd(fd) {}
    ~Fd();
    Fd(Fd &&other) noexcept : m_fd(other.release()) {}
    Fd &operator=(Fd &&other) noexcept;
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int get() const { return m_fd; }
    int release() { return std::exchange(m_fd, -1); }
    explicit operator bool() const { return m_fd >= 0; }

private:
    int m_fd = -1;
};

enum class Kind : quint16 { Probe = 1, ProbeReply, Request, Reply };
enum class Io { Ok, Again, Closed, Protocol, Error };

struct Frame {
    Kind kind = Kind::Probe;
    quint64 serial = 0;
    QByteArray body;
    std::vector<Fd> fds;
};

inline constexpr int MaxBody = 32 * 1024;
inline constexpr int MaxFds = 2;
inline constexpr int ChildSocket = 3;

std::array<Fd, 2> socketPair();

// The endpoint never blocks when its descriptor is nonblocking. Send borrows
// descriptors; successful receive transfers ownership to its caller.
class Transport {
public:
    virtual ~Transport() = default;
    virtual Io send(const Frame &frame) = 0;
    virtual Io receive(Frame &frame) = 0;
};

class SeqPacket : public Transport {
public:
    explicit SeqPacket(Fd fd) : m_fd(std::move(fd)) {}
    int descriptor() const { return m_fd.get(); }
    Io send(const Frame &frame) override;
    Io receive(Frame &frame) override;

private:
    Fd m_fd;
};

} // namespace MpvCapture
