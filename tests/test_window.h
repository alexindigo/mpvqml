#pragma once

#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtQml/qqmlregistration.h>
#include <memory>

namespace MpvQmlTestSupport {

// Rendering/metadata fixtures do not consume compositor input. Avoid queuing
// pointer-focus events against dying windows: libwayland 1.26 can lose the
// reference to a destroyed object argument when dispatch replaces it with null.
inline Qt::WindowFlags testWindowFlags(Qt::WindowFlags flags = Qt::Window) {
    return flags | Qt::WindowTransparentForInput;
}

class TestWindow : public QQuickWindow {
public:
    explicit TestWindow(QWindow *parent = nullptr) : QQuickWindow(parent) {
        setFlags(testWindowFlags(flags()));
    }
};

inline std::unique_ptr<QObject> createTestWindow(QQmlComponent &component) {
    // Initial properties precede componentComplete(), which shows example roots.
    return std::unique_ptr<QObject>(component.createWithInitialProperties({
        {QStringLiteral("flags"), QVariant::fromValue(testWindowFlags())}
    }));
}

// The QML TestCase is hosted by qmltestrunner's window. Apply the same policy
// when the suite is attached to that window, before the runner shows it.
class WindowPolicy : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
public:
    explicit WindowPolicy(QQuickItem *parent = nullptr) : QQuickItem(parent) {
        auto apply = [](QQuickWindow *window) {
            if (window) window->setFlags(testWindowFlags(window->flags()));
        };
        connect(this, &QQuickItem::windowChanged, this, apply);
        apply(window());
    }

    Q_INVOKABLE bool inputTransparent() const {
        return window() && window()->flags().testFlag(Qt::WindowTransparentForInput);
    }
};

} // namespace MpvQmlTestSupport
