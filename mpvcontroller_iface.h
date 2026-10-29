#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <mpv/client.h>

struct MpvInitializationStatus {
    bool attempted = false;
    bool success = false;
    int error = MPV_ERROR_UNINITIALIZED;
    QString message;
};

// GUI-affine admission/completion seam. Positive IDs are local admissions, not
// native submission status. All terminal replies are owned and queued.
class MpvControllerIface : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~MpvControllerIface() override = default;
    virtual MpvInitializationStatus initialize() = 0;
    virtual MpvInitializationStatus initializationResult() const = 0;
    virtual bool available() const = 0;
    virtual void shutdown() = 0;
    virtual int commandAsync(const QStringList &params) = 0;
    virtual int setMpvPropertyAsync(const QString &name, const QVariant &value) = 0;
    virtual int readMpvPropertyAsync(const QString &name) = 0;
    virtual int loadConfigurationAsync(const QString &path) = 0;
    virtual void observeProperty(const QString &name, mpv_format format,
                                 uint64_t id = 0) = 0;
signals:
    void initializationFinished(bool success, int error, const QString &message);
    void coreUnavailable();
    void operationFailed(const QString &operation, int error, const QString &message);
    void propertyChanged(const QString &name, const QVariant &value);
    void fileStarted();
    void fileLoaded();
    void endFile(const QString &reason);
    void videoReconfig();
    void asyncCommandFinished(int id, int error, const QVariant &result);
    void mpvPropertySetFinished(int id, int error);
    void mpvPropertyReadFinished(int id, int error, const QVariant &value);
    void configurationFinished(int id, int error);
};
