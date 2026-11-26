#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QTemporaryDir>
#include <QUrl>
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
    Q_INVOKABLE bool stage(const QUrl &source, int order) const {
        if (!valid() || !source.isLocalFile()) return false;
        const QFileInfo input(source.toLocalFile());
        QFile file(QDir(path()).filePath(input.fileName()));
        return QFile::copy(input.absoluteFilePath(), file.fileName()) && file.open(QIODevice::ReadOnly)
            && file.setFileTime(QDateTime::fromSecsSinceEpoch(1700000000 + 60LL * order),
                                QFileDevice::FileModificationTime);
    }
private:
    QTemporaryDir m_directory;
};

} // namespace MpvQmlTestSupport
