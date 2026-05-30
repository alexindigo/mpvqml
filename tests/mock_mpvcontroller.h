#pragma once

#include "../mpvcontroller_iface.h"

class MockMpvController : public MpvControllerIface {
    Q_OBJECT
public:
    explicit MockMpvController(QObject *parent = nullptr)
        : MpvControllerIface(parent) {}

    int setProperty(const QString &property, const QVariant &value) override {
        m_lastSetProperty = property;
        m_lastSetValue = value;
        ++m_setPropertyCount;
        return 0;
    }

    QVariant command(const QStringList &params) override {
        m_lastCommand = params;
        ++m_commandCount;
        return QVariant();
    }

    void observeProperty(const QString &property, mpv_format format,
                         uint64_t id = 0) override {
        m_lastObservedProperty = property;
        m_lastObservedFormat = format;
    }

    int commandAsync(const QVariant &params, int id = 0) override {
        m_lastAsyncCommand = params;
        m_lastAsyncId = id;
        ++m_asyncCommandCount;
        return 0;
    }

    int commandAsync(const QStringList &params, int id = 0) override {
        m_lastAsyncCommandStringList = params;
        m_lastAsyncId = id;
        ++m_asyncCommandCount;
        return 0;
    }

    mpv_handle *rawMpv() const override {
        return nullptr;
    }

    void reset() {
        m_lastSetProperty.clear();
        m_lastSetValue = QVariant();
        m_setPropertyCount = 0;
        m_lastCommand.clear();
        m_commandCount = 0;
        m_lastObservedProperty.clear();
        m_lastAsyncCommandStringList.clear();
        m_asyncCommandCount = 0;
    }

    // Test accessors
    QString m_lastSetProperty;
    QVariant m_lastSetValue;
    int m_setPropertyCount = 0;
    QStringList m_lastCommand;
    int m_commandCount = 0;
    QString m_lastObservedProperty;
    mpv_format m_lastObservedFormat = MPV_FORMAT_NONE;
    QVariant m_lastAsyncCommand;
    QStringList m_lastAsyncCommandStringList;
    int m_lastAsyncId = 0;
    int m_asyncCommandCount = 0;
};
