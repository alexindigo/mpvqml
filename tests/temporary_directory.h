#pragma once

#include <QObject>
#include <QTemporaryDir>
#include <QtQml/qqmlregistration.h>

namespace MpvQmlTestSupport {

// Own the QML suite's files for the entire suite, including async completions.
// QTemporaryDir provides a unique private directory and honours TMPDIR.
class TemporaryDirectory : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString path READ path CONSTANT)
    Q_PROPERTY(bool valid READ valid CONSTANT)
public:
    explicit TemporaryDirectory(QObject *parent = nullptr) : QObject(parent) {}
    QString path() const { return m_directory.path(); }
    bool valid() const { return m_directory.isValid(); }
private:
    QTemporaryDir m_directory;
};

} // namespace MpvQmlTestSupport
