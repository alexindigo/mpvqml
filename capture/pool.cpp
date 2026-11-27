// SPDX-License-Identifier: LGPL-3.0-only
#include "pool.h"
#include "helper.h"

#include <QPointer>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>
#include <algorithm>
#include <limits>
#include <optional>

namespace MpvCapture {
namespace {

constexpr int DefaultChildren = 2;
constexpr unsigned DefaultReuse = 32;
using Clock = std::chrono::steady_clock;

Failure failure(QString code, QString detail) {
    return {QStringLiteral("helper"), std::move(code), std::move(detail)};
}

class ProcessChild final : public Child {
public:
    explicit ProcessChild(QString executable) : m_executable(std::move(executable)) {
        connect(&m_process, &QProcess::finished, this, [this] { reportExit(); });
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                reportExit(true);
        });
    }

    void start() override {
        auto fd = startChild(m_process, m_executable);
        if (!fd) {
            reportExit(true);
            return;
        }
        m_peer = std::make_unique<SeqPacket>(std::move(fd));
        m_read = new QSocketNotifier(m_peer->descriptor(), QSocketNotifier::Read, this);
        m_write = new QSocketNotifier(m_peer->descriptor(), QSocketNotifier::Write, this);
        m_write->setEnabled(false);
        connect(m_read, &QSocketNotifier::activated, this, [this] {
            m_read->setEnabled(false);
            emit readable();
        });
        connect(m_write, &QSocketNotifier::activated, this, [this] {
            m_write->setEnabled(false);
            emit writable();
        });
    }

    Io send(const Frame &frame) override {
        if (!m_peer)
            return Io::Closed;
        const auto status = m_peer->send(frame);
        m_write->setEnabled(status == Io::Again);
        return status;
    }

    Io receive(Frame &frame) override {
        if (!m_peer)
            return Io::Closed;
        const auto status = m_peer->receive(frame);
        if (status == Io::Closed && m_process.state() == QProcess::Starting)
            return Io::Again; // Let QProcess classify exec failure before reporting EOF.
        m_read->setEnabled(!retired() && (status == Io::Ok || status == Io::Again));
        return status;
    }

    void terminate() override {
        closePeer();
        if (m_process.state() == QProcess::NotRunning)
            reportExit();
        else
            m_process.kill();
    }

    ~ProcessChild() override { closePeer(); }

private:
    void closePeer() {
        delete std::exchange(m_read, nullptr);
        delete std::exchange(m_write, nullptr);
        m_peer.reset();
    }

    QString m_executable;
    QProcess m_process;
    std::unique_ptr<SeqPacket> m_peer;
    QSocketNotifier *m_read = nullptr, *m_write = nullptr;
};

} // namespace

void Child::reportExit(bool launchFailed) {
    if (!std::exchange(m_retired, true)) {
        m_launchFailed = launchFailed;
        emit exited();
    }
}

struct Pool::State {
    struct Work {
        int id;
        quint64 serial;
        Clock::time_point deadline;
        std::optional<Frame> pending;
        Completion completion;
        Validator validator;
    };
    struct Slot {
        std::unique_ptr<Child> child;
        std::optional<Work> work;
        QTimer timer;
        unsigned used = 0;
        bool stopping = false;
    };

    State(Pool &owner, Factory makeChild, int capacity, unsigned reuse)
        : q(owner), factory(std::move(makeChild)), maxReuse(qMax(1u, reuse)) {
        for (int i = 0; i < capacity; ++i) {
            auto slot = std::make_unique<Slot>();
            slot->timer.setSingleShot(true);
            slot->timer.setTimerType(Qt::PreciseTimer);
            connect(&slot->timer, &QTimer::timeout, &q, [this, owner = slot.get()] {
                if (owner->work && owner->work->completion) {
                    const auto remaining = owner->work->deadline - Clock::now();
                    if (remaining > Clock::duration::zero())
                        owner->timer.start(std::chrono::ceil<std::chrono::milliseconds>(remaining));
                    else
                        fail(*owner, failure("deadline", "Capture helper deadline exceeded"));
                }
            });
            children.push_back(std::move(slot));
        }
    }

    ~State() {
        for (auto &slot : children) {
            if (auto *child = slot->child.release()) {
                QObject::disconnect(child, nullptr, &q, nullptr);
                if (child->retired()) {
                    child->deleteLater();
                } else {
                    connect(child, &Child::exited, child, &QObject::deleteLater);
                    child->terminate();
                }
            }
        }
    }

    static bool isIdle(const Slot &slot) {
        return !slot.work && !slot.stopping && (!slot.child || !slot.child->retired());
    }

    Slot *idle() const {
        for (const auto &slot : children) {
            if (isIdle(*slot))
                return slot.get();
        }
        return nullptr;
    }

    void launch(Slot &slot) {
        slot.child = factory ? factory() : nullptr;
        slot.used = 0;
        if (!slot.child) {
            fail(slot, failure("helper-unavailable", "Could not create capture helper process ownership"));
            return;
        }
        const QPointer<Child> generation(slot.child.get());
        connect(
            slot.child.get(), &Child::readable, &q,
            [this, &slot, generation] {
                if (generation && slot.child.get() == generation && !slot.stopping)
                    read(slot);
            },
            Qt::QueuedConnection);
        connect(
            slot.child.get(), &Child::writable, &q,
            [this, &slot, generation] {
                if (generation && slot.child.get() == generation && !slot.stopping)
                    send(slot);
            },
            Qt::QueuedConnection);
        connect(
            slot.child.get(), &Child::exited, &q,
            [this, &slot, generation] {
                if (generation && slot.child.get() == generation)
                    reaped(slot);
            },
            Qt::QueuedConnection);
        slot.child->start();
    }

    void send(Slot &slot) {
        if (!slot.work || !slot.work->pending || !slot.child)
            return;
        switch (slot.child->send(*slot.work->pending)) {
        case Io::Ok:
            slot.work->pending.reset();
            break;
        case Io::Again:
            break;
        case Io::Protocol:
            fail(slot, failure("protocol-error", "Cannot encode capture helper request"));
            break;
        default:
            fail(slot, failure("helper-crashed", "Capture helper request channel closed"));
            break;
        }
    }

    void read(Slot &slot) {
        Frame frame;
        const auto status = slot.child->receive(frame);
        if (status == Io::Again)
            return;
        if (slot.work && Clock::now() >= slot.work->deadline) {
            fail(slot, failure("deadline", "Capture helper deadline exceeded"));
            return;
        }
        if (status == Io::Closed || status == Io::Error) {
            fail(slot, slot.child->launchFailed()
                ? failure("helper-unavailable", "Could not start capture helper")
                : failure("helper-crashed", "Capture helper response channel closed"));
            return;
        }
        if (status != Io::Ok || !slot.work || slot.work->pending || frame.kind != Kind::Reply ||
            frame.serial != slot.work->serial ||
            (slot.work->validator && !slot.work->validator(frame))) {
            fail(slot, failure("protocol-error", "Invalid or unmatched capture helper response"));
            return;
        }
        complete(slot, {{}, std::move(frame)});
        if (++slot.used >= maxReuse || slot.child->retired())
            retire(slot);
        else
            release(slot);
    }

    void complete(Slot &slot, JobReply reply) {
        if (!slot.work || !slot.work->completion)
            return;
        slot.timer.stop();
        slot.work->pending.reset();
        // Consume the terminal owner now so cancel/reply/exit cannot settle twice.
        auto callback = std::exchange(slot.work->completion, {});
        QMetaObject::invokeMethod(
            &q,
            [callback = std::move(callback), reply = std::move(reply)]() mutable {
                callback(std::move(reply));
            },
            Qt::QueuedConnection);
    }

    void release(Slot &slot) {
        if (!slot.work)
            return;
        const int id = slot.work->id;
        slot.work.reset();
        QMetaObject::invokeMethod(
            &q, [owner = &q, id] { emit owner->capacityReleased(id); }, Qt::QueuedConnection);
    }

    void retire(Slot &slot) {
        if (!slot.child) {
            release(slot);
            return;
        }
        slot.stopping = true;
        slot.child->terminate();
    }

    void fail(Slot &slot, Failure error) {
        complete(slot, {std::move(error), {}});
        retire(slot);
    }

    void reaped(Slot &slot) {
        // Qt may report reaping before the socket notifier fires. A fully
        // received terminal response still belongs to the live ticket.
        if (!slot.stopping && slot.work)
            read(slot);
        if (slot.work && Clock::now() >= slot.work->deadline)
            complete(slot, {failure("deadline", "Capture helper deadline exceeded"), {}});
        else
            complete(slot, {failure("helper-crashed", "Capture helper exited without a response"), {}});
        slot.child->deleteLater();
        slot.child.release();
        slot.stopping = false;
        release(slot);
    }

    Pool &q;
    Factory factory;
    std::vector<std::unique_ptr<Slot>> children;
    unsigned maxReuse;
    int lastId = 0;
    quint64 lastSerial = quint64(std::numeric_limits<int>::max());
};

Pool::Pool(QObject *parent)
    : Pool([] { return processChild(captureHelperPath()); }, DefaultChildren, DefaultReuse, parent) {}

Pool::Pool(Factory factory, int capacity, unsigned maxReuse, QObject *parent, int previousTicket)
    : QObject(parent), d(std::make_unique<State>(*this, std::move(factory), capacity, maxReuse)) {
    d->lastId = qMax(0, previousTicket);
}

Pool::~Pool() = default;

Pool::Ticket Pool::submit(Frame request, Clock::time_point deadline, Completion completion,
                          Validator validator) {
    const auto budget = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    if (request.kind != Kind::Request || request.serial || request.body.size() > MaxBody ||
        request.fds.size() > MaxFds || budget.count() <= 0 ||
        budget.count() > std::numeric_limits<int>::max() || !completion)
        return {Admission::Invalid};
    if (const auto status = capacity(); status != Admission::Accepted)
        return {status};
    auto *slot = d->idle();
    const int id = ++d->lastId;
    request.serial = ++d->lastSerial;
    const auto serial = request.serial;
    slot->work.emplace(State::Work{id, serial, deadline, std::move(request),
                                   std::move(completion), std::move(validator)});
    slot->timer.start(budget);
    if (!slot->child)
        d->launch(*slot);
    d->send(*slot);
    return {Admission::Accepted, id};
}

bool Pool::cancel(int id) {
    for (const auto &slot : d->children) {
        if (slot->work && slot->work->id == id && slot->work->completion) {
            d->fail(*slot, failure("cancelled", "Capture helper request cancelled"));
            return true;
        }
    }
    return false;
}

Pool::Admission Pool::capacity() const {
    if (d->lastId == std::numeric_limits<int>::max())
        return Admission::Exhausted;
    return d->idle() ? Admission::Accepted : Admission::Busy;
}

int Pool::idleSlots() const {
    return std::count_if(d->children.begin(), d->children.end(),
                         [](const auto &slot) { return State::isIdle(*slot); });
}

std::unique_ptr<Child> processChild(const QString &executable) {
    return std::make_unique<ProcessChild>(executable);
}

} // namespace MpvCapture
