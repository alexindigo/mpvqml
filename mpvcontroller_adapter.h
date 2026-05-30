#pragma once

#include "mpvcontroller_iface.h"
#include <mpvcontroller.h>

class MpvControllerAdapter : public MpvControllerIface {
    Q_OBJECT
public:
    explicit MpvControllerAdapter(MpvController *real, QObject *parent = nullptr);
    ~MpvControllerAdapter() override = default;

    int setProperty(const QString &property, const QVariant &value) override;
    QVariant command(const QStringList &params) override;
    void observeProperty(const QString &property, mpv_format format,
                         uint64_t id = 0) override;
    int commandAsync(const QVariant &params, int id = 0) override;
    int commandAsync(const QStringList &params, int id = 0) override;
    mpv_handle *rawMpv() const override;

private:
    MpvController *m_real;
};
