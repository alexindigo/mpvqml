// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QHash>
#include <QObject>
#include <QUrl>
#include <QtQml/qqmlregistration.h>
#include <memory>

class MpvFrameCapture : public QObject {
  Q_OBJECT
  QML_ELEMENT
public:
  explicit MpvFrameCapture(QObject *parent = nullptr);
  ~MpvFrameCapture() override;

  Q_INVOKABLE int capture(const QUrl &source, double fraction, int maxWidth, int maxHeight,
                          const QUrl &destination);
  Q_INVOKABLE bool cancel(int id);

signals:
  void finished(int id, const QString &code, const QString &reason);

private:
  struct Dispatch;
  struct Job;
  std::shared_ptr<Dispatch> m_dispatch;
  QHash<int, std::shared_ptr<Job>> m_jobs;
  int m_lastId = 0;
};
