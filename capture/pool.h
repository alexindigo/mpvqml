// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "wire.h"

#include <QObject>
#include <QString>
#include <chrono>
#include <functional>
#include <memory>

namespace MpvCapture {

class Child : public QObject, public Transport {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start() = 0;
    virtual void terminate() = 0;
    bool retired() const { return m_retired; }

signals:
    void readable();
    void writable();
    // Emitted once. No process or pending start remains when delivered.
    void exited();

protected:
    void reportExit();

private:
    bool m_retired = false;
};

struct Failure {
    QString stage, code, detail;
    explicit operator bool() const { return !code.isEmpty(); }
};

struct JobReply {
    Failure error;
    Frame frame;
};

class Pool : public QObject {
    Q_OBJECT
public:
    using Completion = std::function<void(JobReply)>;
    using Validator = std::function<bool(const Frame &)>;
    using Factory = std::function<std::unique_ptr<Child>()>;
    enum class Admission { Accepted, Busy, Invalid, Exhausted };
    struct Ticket {
        Admission status;
        int id = 0;
    };

    explicit Pool(QObject *parent = nullptr);
    explicit Pool(Factory factory, int capacity, unsigned maxReuse, QObject *parent = nullptr,
                  int previousTicket = 0);
    ~Pool() override;

    // GUI-affine, local admission only. The callback cannot run inline.
    Ticket submit(Frame request, std::chrono::steady_clock::time_point deadline, Completion completion,
                  Validator validator = {});
    bool cancel(int id);
    Admission capacity() const;
    int idleSlots() const;
    bool available() const { return capacity() == Admission::Accepted; }

signals:
    void capacityReleased(int id);

private:
    struct State;
    std::unique_ptr<State> d;
};

std::unique_ptr<Child> processChild(const QString &executable);

} // namespace MpvCapture
