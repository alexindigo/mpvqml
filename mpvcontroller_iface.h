#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <mpv/client.h>

class MpvControllerIface : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~MpvControllerIface() override = default;

    virtual int setProperty(const QString &property, const QVariant &value) = 0;
    virtual QVariant command(const QStringList &params) = 0;
    virtual void observeProperty(const QString &property, mpv_format format,
                                 uint64_t id = 0) = 0;
    virtual int commandAsync(const QVariant &params, int id = 0) = 0;
    virtual int commandAsync(const QStringList &params, int id = 0) = 0;
    virtual mpv_handle *rawMpv() const = 0;

signals:
    void propertyChanged(const QString &property, const QVariant &value);
    void fileStarted();
    void fileLoaded();
    void endFile(const QString &reason);
    void videoReconfig();
    void asyncReply(int id, int error);
};
