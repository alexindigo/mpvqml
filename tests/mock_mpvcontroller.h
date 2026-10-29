#pragma once
#include "../mpvcontroller_iface.h"
#include <limits>

class MockMpvController : public MpvControllerIface {
    Q_OBJECT
public:
    explicit MockMpvController(QObject *parent = nullptr) : MpvControllerIface(parent) {}
    MpvInitializationStatus initialize() override {
        if (!m_initNotified) {
            m_initNotified = true;
            const auto status = m_initialization;
            QMetaObject::invokeMethod(this, [this, status]() {
                emit initializationFinished(status.success, status.error, status.message);
            }, Qt::QueuedConnection);
        }
        return m_initialization;
    }
    MpvInitializationStatus initializationResult() const override { return m_initialization; }
    bool available() const override { return m_available && m_initialization.success; }
    void shutdown() override { m_available = false; }
    int commandAsync(const QStringList &params) override {
        const int id = allocate();
        if (id < 0) return id;
        m_lastCommand = params;
        m_commands.append(params);
        m_submissions.append(QVariantList{QStringLiteral("command"), params});
        ++m_commandCount;
        m_lastAsyncCommandStringList = params;
        m_lastAsyncId = id;
        ++m_asyncCommandCount;
        if (m_autoComplete) {
            const int error = m_replyError;
            const QVariant value = m_replyValue;
            QMetaObject::invokeMethod(this, [this, id, error, value]() { emit asyncCommandFinished(id, error, value); }, Qt::QueuedConnection);
        }
        return id;
    }
    int setMpvPropertyAsync(const QString &name, const QVariant &value) override {
        const int id = allocate();
        if (id < 0) return id;
        m_lastSetProperty = name;
        m_lastSetValue = value;
        m_setValues.insert(name, value);
        m_submissions.append(QVariantList{QStringLiteral("set"), name, value});
        ++m_setPropertyCount;
        if (m_autoComplete) {
            const int error = m_replyError;
            QMetaObject::invokeMethod(this, [this, id, error]() { emit mpvPropertySetFinished(id, error); }, Qt::QueuedConnection);
        }
        return id;
    }
    int readMpvPropertyAsync(const QString &name) override {
        const int id = allocate();
        if (id < 0) return id;
        m_lastReadProperty = name;
        ++m_readPropertyCount;
        if (m_autoComplete) {
            const int error = m_replyError;
            const QVariant value = m_replyValue;
            QMetaObject::invokeMethod(this, [this, id, error, value]() { emit mpvPropertyReadFinished(id, error, value); }, Qt::QueuedConnection);
        }
        return id;
    }
    int loadConfigurationAsync(const QString &path) override {
        const int id = allocate();
        if (id < 0) return id;
        m_lastConfiguration = path;
        if (m_autoComplete) {
            const int error = m_replyError;
            QMetaObject::invokeMethod(this, [this, id, error]() { emit configurationFinished(id, error); }, Qt::QueuedConnection);
        }
        return id;
    }
    void observeProperty(const QString &name, mpv_format format, uint64_t = 0) override {
        m_lastObservedProperty = name;
        m_lastObservedFormat = format;
    }
    void reset() {
        m_lastSetProperty.clear(); m_lastSetValue = {}; m_setPropertyCount = 0;
        m_setValues.clear(); m_commands.clear(); m_submissions.clear();
        m_lastCommand.clear(); m_commandCount = 0;
        m_lastObservedProperty.clear(); m_lastAsyncCommandStringList.clear(); m_asyncCommandCount = 0;
        m_lastReadProperty.clear(); m_readPropertyCount = 0;
    }
    MpvInitializationStatus m_initialization{true, true, MPV_ERROR_SUCCESS, {}};
    bool m_available = true;
    bool m_autoComplete = true;
    int m_replyError = 0;
    QVariant m_replyValue;
    quint64 m_nextId = 1;
    QString m_lastSetProperty;
    QVariant m_lastSetValue;
    QVariantMap m_setValues;
    QList<QStringList> m_commands;
    QList<QVariantList> m_submissions;
    int m_setPropertyCount = 0;
    QStringList m_lastCommand;
    int m_commandCount = 0;
    QString m_lastObservedProperty;
    mpv_format m_lastObservedFormat = MPV_FORMAT_NONE;
    QStringList m_lastAsyncCommandStringList;
    int m_lastAsyncId = 0;
    int m_asyncCommandCount = 0;
    QString m_lastReadProperty;
    int m_readPropertyCount = 0;
    QString m_lastConfiguration;
private:
    bool m_initNotified = false;
    int allocate() {
        if (!available()) return MPV_ERROR_UNINITIALIZED;
        if (m_nextId > static_cast<quint64>(std::numeric_limits<int>::max())) return MPV_ERROR_NOMEM;
        return static_cast<int>(m_nextId++);
    }
};
