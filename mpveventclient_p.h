#pragma once

#include <QObject>
#include <QPointer>
#include <atomic>
#include <memory>
#include <new>
#include <utility>
#include <mpv/client.h>

namespace MpvQmlPrivate {

// Only event clients share this protocol. The receiver owns this member, and
// opens/closes it on its own thread. Native wakeups only post, never drain.
class MpvEventClient final {
public:
    enum class Lifetime { Borrowed, Owned };
    using Handler = void (*)(QObject *, const mpv_event &);

    MpvEventClient(QObject *receiver, Handler handler) : m_receiver(receiver), m_handler(handler) {}
    ~MpvEventClient() { close(); }
    mpv_handle *handle() const { return m_handle; }

    // Allocate separately so playback can reject before publishing a core.
    bool prepare() {
        if (m_token) return true;
        m_token.reset(new (std::nothrow) Token{this, m_receiver, m_generation});
        return bool(m_token);
    }
    void open(mpv_handle *handle, Lifetime lifetime) {
        Q_ASSERT(!m_handle && handle && m_token);
        m_handle = handle;
        m_lifetime = lifetime;
        mpv_set_wakeup_callback(handle, wakeup, m_token.get());
    }
    void close() {
        auto *handle = std::exchange(m_handle, nullptr);
        auto token = std::move(m_token);
        ++m_generation;
        if (!handle) return;
        mpv_set_wakeup_callback(handle, nullptr, nullptr); // barrier for token access
        if (m_lifetime == Lifetime::Owned) mpv_destroy(handle);
    }

private:
    struct Token {
        MpvEventClient *const client;
        QObject *const receiver;
        const quint64 generation;
        std::atomic<bool> pending{false};
    };
    static void wakeup(void *opaque) noexcept {
        auto *token = static_cast<Token *>(opaque);
        if (token->pending.exchange(true)) return;
        auto *client = token->client;
        const auto generation = token->generation;
        try {
            // Never retain the native token in queued work.
            if (QMetaObject::invokeMethod(token->receiver, [client, generation]() {
                client->drain(generation);
            }, Qt::QueuedConnection)) return;
        } catch (...) {}
        token->pending.store(false);
    }
    void drain(quint64 generation) {
        if (!m_handle || generation != m_generation) return;
        m_token->pending.store(false);
        if (m_draining) { m_redrain = true; return; }
        m_draining = true;
        const QPointer<QObject> alive(m_receiver);
        constexpr int eventsPerTurn = 32;
        int handled = 0;
        while (m_handle && generation == m_generation && handled < eventsPerTurn) {
            const auto *event = mpv_wait_event(m_handle, 0);
            if (!event || event->event_id == MPV_EVENT_NONE) break;
            ++handled;
            m_handler(m_receiver, *event);
            if (!alive) return;
        }
        m_draining = false;
        const bool redrain = std::exchange(m_redrain, false);
        if (m_handle && (redrain || (generation == m_generation && handled == eventsPerTurn)))
            wakeup(m_token.get()); // continue even if libmpv sends no new wakeup
    }

    QObject *const m_receiver;
    const Handler m_handler;
    std::unique_ptr<Token> m_token;
    mpv_handle *m_handle = nullptr;
    Lifetime m_lifetime = Lifetime::Borrowed;
    quint64 m_generation = 1;
    bool m_draining = false;
    bool m_redrain = false;
};

} // namespace MpvQmlPrivate
