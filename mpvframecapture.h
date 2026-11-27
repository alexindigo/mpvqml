// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QHash>
#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <memory>

class MpvFrameCapture : public QObject {
  Q_OBJECT
  QML_ELEMENT
public:
  enum AdmissionStatus {
    Accepted = 0, InvalidArguments, LocalBusy, SharedBusy, IdExhausted,
    AllocationFailed, BackendUnavailable, WorkerUnavailable
  };
  Q_ENUM(AdmissionStatus)

  explicit MpvFrameCapture(QObject *parent = nullptr);
  ~MpvFrameCapture() override;

  Q_INVOKABLE int capture(const QUrl &source, double fraction, int maxWidth, int maxHeight,
                          const QUrl &destination);
  Q_INVOKABLE QVariantMap tryCapture(const QUrl &source, double fraction, int maxWidth, int maxHeight,
                                    const QUrl &destination);
  Q_INVOKABLE bool capacityAvailable() const;
  Q_INVOKABLE bool cancel(int id);

signals:
  void finished(int id, const QString &code, const QString &reason);
  void finishedDetailed(int id, const QVariantMap &detail);
  void capacityReleased(int id);

private:
  struct Admission;
  struct Dispatch;
  struct Job;
  struct Outcome;
  class Queue;
  Admission admit(const QUrl &source, double fraction, int maxWidth, int maxHeight,
                  const QUrl &destination);
  void retired(const std::shared_ptr<Job> &job, const Outcome &outcome);
  void deliver(int id, const Outcome &outcome);
  void deliverCancellation(const std::shared_ptr<Job> &job);
  std::shared_ptr<Dispatch> m_dispatch;
  QHash<int, std::shared_ptr<Job>> m_activeJobs;
  int m_lastId = 0;
};
