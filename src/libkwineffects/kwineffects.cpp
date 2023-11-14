/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2009 Lucas Murray <lmurray@undefinedfire.com>
    SPDX-FileCopyrightText: 2018 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "libkwineffects/kwineffects.h"
#include "core/output.h"
#include "group.h"
#include "internalwindow.h"
#include "scene/windowitem.h"
#include "virtualdesktops.h"
#include "waylandwindow.h"
#include "x11window.h"

#include "config-kwin.h"

#include <QFontMetrics>
#include <QMatrix4x4>
#include <QPainter>
#include <QPixmap>
#include <QTimeLine>
#include <QVariant>
#include <QWindow>
#include <QtMath>

#include <kconfiggroup.h>
#include <ksharedconfig.h>

#include <optional>

namespace KWin
{

static QByteArray readWindowProperty(xcb_window_t win, xcb_atom_t atom, xcb_atom_t type, int format)
{
    if (win == XCB_WINDOW_NONE) {
        return QByteArray();
    }
    uint32_t len = 32768;
    for (;;) {
        Xcb::Property prop(false, win, atom, XCB_ATOM_ANY, 0, len);
        if (prop.isNull()) {
            // get property failed
            return QByteArray();
        }
        if (prop->bytes_after > 0) {
            len *= 2;
            continue;
        }
        return prop.toByteArray(format, type);
    }
}

static void deleteWindowProperty(xcb_window_t win, long int atom)
{
    if (win == XCB_WINDOW_NONE) {
        return;
    }
    xcb_delete_property(kwinApp()->x11Connection(), win, atom);
}

void WindowPrePaintData::setTranslucent()
{
    mask |= Effect::PAINT_WINDOW_TRANSLUCENT;
    mask &= ~Effect::PAINT_WINDOW_OPAQUE;
    opaque = QRegion(); // cannot clip, will be transparent
}

void WindowPrePaintData::setTransformed()
{
    mask |= Effect::PAINT_WINDOW_TRANSFORMED;
}

class PaintDataPrivate
{
public:
    PaintDataPrivate()
        : scale(1., 1., 1.)
        , rotationAxis(0, 0, 1.)
        , rotationAngle(0.)
    {
    }
    QVector3D scale;
    QVector3D translation;

    QVector3D rotationAxis;
    QVector3D rotationOrigin;
    qreal rotationAngle;
};

PaintData::PaintData()
    : d(std::make_unique<PaintDataPrivate>())
{
}

PaintData::~PaintData() = default;

qreal PaintData::xScale() const
{
    return d->scale.x();
}

qreal PaintData::yScale() const
{
    return d->scale.y();
}

qreal PaintData::zScale() const
{
    return d->scale.z();
}

void PaintData::setScale(const QVector2D &scale)
{
    d->scale.setX(scale.x());
    d->scale.setY(scale.y());
}

void PaintData::setScale(const QVector3D &scale)
{
    d->scale = scale;
}
void PaintData::setXScale(qreal scale)
{
    d->scale.setX(scale);
}

void PaintData::setYScale(qreal scale)
{
    d->scale.setY(scale);
}

void PaintData::setZScale(qreal scale)
{
    d->scale.setZ(scale);
}

const QVector3D &PaintData::scale() const
{
    return d->scale;
}

void PaintData::setXTranslation(qreal translate)
{
    d->translation.setX(translate);
}

void PaintData::setYTranslation(qreal translate)
{
    d->translation.setY(translate);
}

void PaintData::setZTranslation(qreal translate)
{
    d->translation.setZ(translate);
}

void PaintData::translate(qreal x, qreal y, qreal z)
{
    translate(QVector3D(x, y, z));
}

void PaintData::translate(const QVector3D &t)
{
    d->translation += t;
}

qreal PaintData::xTranslation() const
{
    return d->translation.x();
}

qreal PaintData::yTranslation() const
{
    return d->translation.y();
}

qreal PaintData::zTranslation() const
{
    return d->translation.z();
}

const QVector3D &PaintData::translation() const
{
    return d->translation;
}

qreal PaintData::rotationAngle() const
{
    return d->rotationAngle;
}

QVector3D PaintData::rotationAxis() const
{
    return d->rotationAxis;
}

QVector3D PaintData::rotationOrigin() const
{
    return d->rotationOrigin;
}

void PaintData::setRotationAngle(qreal angle)
{
    d->rotationAngle = angle;
}

void PaintData::setRotationAxis(Qt::Axis axis)
{
    switch (axis) {
    case Qt::XAxis:
        setRotationAxis(QVector3D(1, 0, 0));
        break;
    case Qt::YAxis:
        setRotationAxis(QVector3D(0, 1, 0));
        break;
    case Qt::ZAxis:
        setRotationAxis(QVector3D(0, 0, 1));
        break;
    }
}

void PaintData::setRotationAxis(const QVector3D &axis)
{
    d->rotationAxis = axis;
}

void PaintData::setRotationOrigin(const QVector3D &origin)
{
    d->rotationOrigin = origin;
}

QMatrix4x4 PaintData::toMatrix(qreal deviceScale) const
{
    QMatrix4x4 ret;
    if (d->translation != QVector3D(0, 0, 0)) {
        ret.translate(d->translation * deviceScale);
    }
    if (d->scale != QVector3D(1, 1, 1)) {
        ret.scale(d->scale);
    }

    if (d->rotationAngle != 0) {
        ret.translate(d->rotationOrigin * deviceScale);
        ret.rotate(d->rotationAngle, d->rotationAxis);
        ret.translate(-d->rotationOrigin * deviceScale);
    }

    return ret;
}

class WindowPaintDataPrivate
{
public:
    qreal opacity;
    qreal saturation;
    qreal brightness;
    int screen;
    qreal crossFadeProgress;
    QMatrix4x4 projectionMatrix;
};

WindowPaintData::WindowPaintData()
    : WindowPaintData(QMatrix4x4())
{
}

WindowPaintData::WindowPaintData(const QMatrix4x4 &projectionMatrix)
    : PaintData()
    , d(std::make_unique<WindowPaintDataPrivate>())
{
    setProjectionMatrix(projectionMatrix);
    setOpacity(1.0);
    setSaturation(1.0);
    setBrightness(1.0);
    setScreen(0);
    setCrossFadeProgress(0.0);
}

WindowPaintData::WindowPaintData(const WindowPaintData &other)
    : PaintData()
    , d(std::make_unique<WindowPaintDataPrivate>())
{
    setXScale(other.xScale());
    setYScale(other.yScale());
    setZScale(other.zScale());
    translate(other.translation());
    setRotationOrigin(other.rotationOrigin());
    setRotationAxis(other.rotationAxis());
    setRotationAngle(other.rotationAngle());
    setOpacity(other.opacity());
    setSaturation(other.saturation());
    setBrightness(other.brightness());
    setScreen(other.screen());
    setCrossFadeProgress(other.crossFadeProgress());
    setProjectionMatrix(other.projectionMatrix());
}

WindowPaintData::~WindowPaintData() = default;

qreal WindowPaintData::opacity() const
{
    return d->opacity;
}

qreal WindowPaintData::saturation() const
{
    return d->saturation;
}

qreal WindowPaintData::brightness() const
{
    return d->brightness;
}

int WindowPaintData::screen() const
{
    return d->screen;
}

void WindowPaintData::setOpacity(qreal opacity)
{
    d->opacity = opacity;
}

void WindowPaintData::setSaturation(qreal saturation) const
{
    d->saturation = saturation;
}

void WindowPaintData::setBrightness(qreal brightness)
{
    d->brightness = brightness;
}

void WindowPaintData::setScreen(int screen) const
{
    d->screen = screen;
}

qreal WindowPaintData::crossFadeProgress() const
{
    return d->crossFadeProgress;
}

void WindowPaintData::setCrossFadeProgress(qreal factor)
{
    d->crossFadeProgress = std::clamp(factor, 0.0, 1.0);
}

qreal WindowPaintData::multiplyOpacity(qreal factor)
{
    d->opacity *= factor;
    return d->opacity;
}

qreal WindowPaintData::multiplySaturation(qreal factor)
{
    d->saturation *= factor;
    return d->saturation;
}

qreal WindowPaintData::multiplyBrightness(qreal factor)
{
    d->brightness *= factor;
    return d->brightness;
}

void WindowPaintData::setProjectionMatrix(const QMatrix4x4 &matrix)
{
    d->projectionMatrix = matrix;
}

QMatrix4x4 WindowPaintData::projectionMatrix() const
{
    return d->projectionMatrix;
}

QMatrix4x4 &WindowPaintData::rprojectionMatrix()
{
    return d->projectionMatrix;
}

WindowPaintData &WindowPaintData::operator*=(qreal scale)
{
    this->setXScale(this->xScale() * scale);
    this->setYScale(this->yScale() * scale);
    this->setZScale(this->zScale() * scale);
    return *this;
}

WindowPaintData &WindowPaintData::operator*=(const QVector2D &scale)
{
    this->setXScale(this->xScale() * scale.x());
    this->setYScale(this->yScale() * scale.y());
    return *this;
}

WindowPaintData &WindowPaintData::operator*=(const QVector3D &scale)
{
    this->setXScale(this->xScale() * scale.x());
    this->setYScale(this->yScale() * scale.y());
    this->setZScale(this->zScale() * scale.z());
    return *this;
}

WindowPaintData &WindowPaintData::operator+=(const QPointF &translation)
{
    return this->operator+=(QVector3D(translation));
}

WindowPaintData &WindowPaintData::operator+=(const QPoint &translation)
{
    return this->operator+=(QVector3D(translation));
}

WindowPaintData &WindowPaintData::operator+=(const QVector2D &translation)
{
    return this->operator+=(QVector3D(translation));
}

WindowPaintData &WindowPaintData::operator+=(const QVector3D &translation)
{
    translate(translation);
    return *this;
}

//****************************************
// Effect
//****************************************

Effect::Effect(QObject *parent)
    : QObject(parent)
{
}

Effect::~Effect()
{
}

void Effect::reconfigure(ReconfigureFlags)
{
}

void Effect::windowInputMouseEvent(QEvent *)
{
}

void Effect::grabbedKeyboardEvent(QKeyEvent *)
{
}

bool Effect::borderActivated(ElectricBorder)
{
    return false;
}

void Effect::prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
{
    effects->prePaintScreen(data, presentTime);
}

void Effect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, region, screen);
}

void Effect::postPaintScreen()
{
    effects->postPaintScreen();
}

void Effect::prePaintWindow(EffectWindow *w, WindowPrePaintData &data, std::chrono::milliseconds presentTime)
{
    effects->prePaintWindow(w, data, presentTime);
}

void Effect::paintWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, QRegion region, WindowPaintData &data)
{
    effects->paintWindow(renderTarget, viewport, w, mask, region, data);
}

void Effect::postPaintWindow(EffectWindow *w)
{
    effects->postPaintWindow(w);
}

bool Effect::provides(Feature)
{
    return false;
}

bool Effect::isActive() const
{
    return true;
}

QString Effect::debug(const QString &) const
{
    return QString();
}

void Effect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const QRegion &region, WindowPaintData &data)
{
    effects->drawWindow(renderTarget, viewport, w, mask, region, data);
}

void Effect::setPositionTransformations(WindowPaintData &data, QRect &region, EffectWindow *w,
                                        const QRect &r, Qt::AspectRatioMode aspect)
{
    QSizeF size = w->size();
    size.scale(r.size(), aspect);
    data.setXScale(size.width() / double(w->width()));
    data.setYScale(size.height() / double(w->height()));
    int width = int(w->width() * data.xScale());
    int height = int(w->height() * data.yScale());
    int x = r.x() + (r.width() - width) / 2;
    int y = r.y() + (r.height() - height) / 2;
    region = QRect(x, y, width, height);
    data.setXTranslation(x - w->x());
    data.setYTranslation(y - w->y());
}

QPointF Effect::cursorPos()
{
    return effects->cursorPos();
}

double Effect::animationTime(const KConfigGroup &cfg, const QString &key, int defaultTime)
{
    int time = cfg.readEntry(key, 0);
    return time != 0 ? time : std::max(defaultTime * effects->animationTimeFactor(), 1.);
}

double Effect::animationTime(int defaultTime)
{
    // at least 1ms, otherwise 0ms times can break some things
    return std::max(defaultTime * effects->animationTimeFactor(), 1.);
}

int Effect::requestedEffectChainPosition() const
{
    return 0;
}

xcb_connection_t *Effect::xcbConnection() const
{
    return effects->xcbConnection();
}

xcb_window_t Effect::x11RootWindow() const
{
    return effects->x11RootWindow();
}

bool Effect::touchDown(qint32 id, const QPointF &pos, std::chrono::microseconds time)
{
    return false;
}

bool Effect::touchMotion(qint32 id, const QPointF &pos, std::chrono::microseconds time)
{
    return false;
}

bool Effect::touchUp(qint32 id, std::chrono::microseconds time)
{
    return false;
}

bool Effect::perform(Feature feature, const QVariantList &arguments)
{
    return false;
}

bool Effect::tabletToolEvent(QTabletEvent *event)
{
    return false;
}

bool Effect::tabletToolButtonEvent(uint button, bool pressed, quint64 tabletToolId)
{
    return false;
}

bool Effect::tabletPadButtonEvent(uint button, bool pressed, void *tabletPadId)
{
    return false;
}

bool Effect::tabletPadStripEvent(int number, int position, bool isFinger, void *tabletPadId)
{
    return false;
}

bool Effect::tabletPadRingEvent(int number, int position, bool isFinger, void *tabletPadId)
{
    return false;
}

bool Effect::blocksDirectScanout() const
{
    return true;
}

//****************************************
// EffectFactory
//****************************************
EffectPluginFactory::EffectPluginFactory()
{
}

EffectPluginFactory::~EffectPluginFactory()
{
}

bool EffectPluginFactory::enabledByDefault() const
{
    return true;
}

bool EffectPluginFactory::isSupported() const
{
    return true;
}

//****************************************
// EffectsHandler
//****************************************

EffectsHandler::EffectsHandler(CompositingType type)
    : compositing_type(type)
{
    if (compositing_type == NoCompositing) {
        return;
    }
    KWin::effects = this;
}

EffectsHandler::~EffectsHandler()
{
    // All effects should already be unloaded by Impl dtor
    Q_ASSERT(loaded_effects.count() == 0);
    KWin::effects = nullptr;
}

CompositingType EffectsHandler::compositingType() const
{
    return compositing_type;
}

bool EffectsHandler::isOpenGLCompositing() const
{
    return compositing_type & OpenGLCompositing;
}

EffectsHandler *effects = nullptr;

//****************************************
// EffectWindow
//****************************************

class Q_DECL_HIDDEN EffectWindow::Private
{
public:
    Private(EffectWindow *q, WindowItem *windowItem);

    EffectWindow *q;
    Window *m_window;
    WindowItem *m_windowItem; // This one is used only during paint pass.
    QHash<int, QVariant> dataMap;
    bool managed = false;
    bool m_waylandWindow;
    bool m_x11Window;
};

EffectWindow::Private::Private(EffectWindow *q, WindowItem *windowItem)
    : q(q)
    , m_window(windowItem->window())
    , m_windowItem(windowItem)
{
}

EffectWindow::EffectWindow(WindowItem *windowItem)
    : d(new Private(this, windowItem))
{
    // Deleted windows are not managed. So, when windowClosed signal is
    // emitted, effects can't distinguish managed windows from unmanaged
    // windows(e.g. combo box popups, popup menus, etc). Save value of the
    // managed property during construction of EffectWindow. At that time,
    // parent can be Client, XdgShellClient, or Unmanaged. So, later on, when
    // an instance of Deleted becomes parent of the EffectWindow, effects
    // can still figure out whether it is/was a managed window.
    d->managed = d->m_window->isClient();

    d->m_waylandWindow = qobject_cast<KWin::WaylandWindow *>(d->m_window) != nullptr;
    d->m_x11Window = qobject_cast<KWin::X11Window *>(d->m_window) != nullptr;

    connect(d->m_window, &Window::windowShown, this, [this]() {
        Q_EMIT windowShown(this);
    });
    connect(d->m_window, &Window::windowHidden, this, [this]() {
        Q_EMIT windowHidden(this);
    });
    connect(d->m_window, &Window::maximizedChanged, this, [this]() {
        const MaximizeMode mode = d->m_window->maximizeMode();
        Q_EMIT windowMaximizedStateChanged(this, mode & MaximizeHorizontal, mode & MaximizeVertical);
    });
    connect(d->m_window, &Window::maximizedAboutToChange, this, [this](MaximizeMode m) {
        Q_EMIT windowMaximizedStateAboutToChange(this, m & MaximizeHorizontal, m & MaximizeVertical);
    });
    connect(d->m_window, &Window::frameGeometryAboutToChange, this, [this]() {
        Q_EMIT windowFrameGeometryAboutToChange(this);
    });
    connect(d->m_window, &Window::interactiveMoveResizeStarted, this, [this]() {
        Q_EMIT windowStartUserMovedResized(this);
    });
    connect(d->m_window, &Window::interactiveMoveResizeStepped, this, [this](const QRectF &geometry) {
        Q_EMIT windowStepUserMovedResized(this, geometry);
    });
    connect(d->m_window, &Window::interactiveMoveResizeFinished, this, [this]() {
        Q_EMIT windowFinishUserMovedResized(this);
    });
    connect(d->m_window, &Window::opacityChanged, this, [this](Window *window, qreal oldOpacity) {
        Q_EMIT windowOpacityChanged(this, oldOpacity, window->opacity());
    });
    connect(d->m_window, &Window::minimizedChanged, this, [this]() {
        if (d->m_window->isMinimized()) {
            Q_EMIT windowMinimized(this);
        } else {
            Q_EMIT windowUnminimized(this);
        }
    });
    connect(d->m_window, &Window::modalChanged, this, [this]() {
        Q_EMIT windowModalityChanged(this);
    });
    connect(d->m_window, &Window::frameGeometryChanged, this, [this](const QRectF &oldGeometry) {
        Q_EMIT windowFrameGeometryChanged(this, oldGeometry);
    });
    connect(d->m_window, &Window::damaged, this, [this]() {
        Q_EMIT windowDamaged(this);
    });
    connect(d->m_window, &Window::unresponsiveChanged, this, [this](bool unresponsive) {
        Q_EMIT windowUnresponsiveChanged(this, unresponsive);
    });
    connect(d->m_window, &Window::keepAboveChanged, this, [this]() {
        Q_EMIT windowKeepAboveChanged(this);
    });
    connect(d->m_window, &Window::keepBelowChanged, this, [this]() {
        Q_EMIT windowKeepBelowChanged(this);
    });
    connect(d->m_window, &Window::fullScreenChanged, this, [this]() {
        Q_EMIT windowFullScreenChanged(this);
    });
    connect(d->m_window, &Window::visibleGeometryChanged, this, [this]() {
        Q_EMIT windowExpandedGeometryChanged(this);
    });
    connect(d->m_window, &Window::decorationChanged, this, [this]() {
        Q_EMIT windowDecorationChanged(this);
    });
    connect(d->m_window, &Window::desktopsChanged, this, [this]() {
        Q_EMIT windowDesktopsChanged(this);
    });
}

EffectWindow::~EffectWindow()
{
}

Window *EffectWindow::window() const
{
    return d->m_window;
}

WindowItem *EffectWindow::windowItem() const
{
    return d->m_windowItem;
}

bool EffectWindow::isOnActivity(const QString &activity) const
{
    const QStringList _activities = activities();
    return _activities.isEmpty() || _activities.contains(activity);
}

bool EffectWindow::isOnAllActivities() const
{
    return activities().isEmpty();
}

void EffectWindow::setMinimized(bool min)
{
    if (min) {
        minimize();
    } else {
        unminimize();
    }
}

bool EffectWindow::isOnCurrentActivity() const
{
    return isOnActivity(effects->currentActivity());
}

bool EffectWindow::isOnCurrentDesktop() const
{
    return isOnDesktop(effects->currentDesktop());
}

bool EffectWindow::isOnDesktop(VirtualDesktop *desktop) const
{
    const QList<VirtualDesktop *> ds = desktops();
    return ds.isEmpty() || ds.contains(desktop);
}

bool EffectWindow::isOnAllDesktops() const
{
    return desktops().isEmpty();
}

bool EffectWindow::hasDecoration() const
{
    return contentsRect() != QRect(0, 0, width(), height());
}

bool EffectWindow::isVisible() const
{
    return !isMinimized()
        && isOnCurrentDesktop()
        && isOnCurrentActivity();
}

void EffectWindow::refVisible(const EffectWindowVisibleRef *holder)
{
    d->m_windowItem->refVisible(holder->reason());
}

void EffectWindow::unrefVisible(const EffectWindowVisibleRef *holder)
{
    d->m_windowItem->unrefVisible(holder->reason());
}

void EffectWindow::addRepaint(const QRect &r)
{
    d->m_windowItem->scheduleRepaint(QRegion(r));
}

void EffectWindow::addRepaintFull()
{
    d->m_windowItem->scheduleRepaint(d->m_windowItem->boundingRect());
}

void EffectWindow::addLayerRepaint(const QRect &r)
{
    d->m_windowItem->scheduleRepaint(d->m_windowItem->mapFromGlobal(r));
}

const EffectWindowGroup *EffectWindow::group() const
{
    if (auto c = qobject_cast<X11Window *>(d->m_window)) {
        return c->group()->effectGroup();
    }
    return nullptr; // TODO
}

void EffectWindow::refWindow()
{
    if (d->m_window->isDeleted()) {
        return d->m_window->ref();
    }
    Q_UNREACHABLE(); // TODO
}

void EffectWindow::unrefWindow()
{
    if (d->m_window->isDeleted()) {
        return d->m_window->unref();
    }
    Q_UNREACHABLE(); // TODO
}

Output *EffectWindow::screen() const
{
    return d->m_window->output();
}

#define WINDOW_HELPER(rettype, prototype, toplevelPrototype) \
    rettype EffectWindow::prototype() const                  \
    {                                                        \
        return d->m_window->toplevelPrototype();             \
    }

WINDOW_HELPER(double, opacity, opacity)
WINDOW_HELPER(qreal, x, x)
WINDOW_HELPER(qreal, y, y)
WINDOW_HELPER(qreal, width, width)
WINDOW_HELPER(qreal, height, height)
WINDOW_HELPER(QPointF, pos, pos)
WINDOW_HELPER(QSizeF, size, size)
WINDOW_HELPER(QRectF, frameGeometry, frameGeometry)
WINDOW_HELPER(QRectF, bufferGeometry, bufferGeometry)
WINDOW_HELPER(QRectF, clientGeometry, clientGeometry)
WINDOW_HELPER(QRectF, expandedGeometry, visibleGeometry)
WINDOW_HELPER(QRectF, rect, rect)
WINDOW_HELPER(bool, isDesktop, isDesktop)
WINDOW_HELPER(bool, isDock, isDock)
WINDOW_HELPER(bool, isToolbar, isToolbar)
WINDOW_HELPER(bool, isMenu, isMenu)
WINDOW_HELPER(bool, isNormalWindow, isNormalWindow)
WINDOW_HELPER(bool, isDialog, isDialog)
WINDOW_HELPER(bool, isSplash, isSplash)
WINDOW_HELPER(bool, isUtility, isUtility)
WINDOW_HELPER(bool, isDropdownMenu, isDropdownMenu)
WINDOW_HELPER(bool, isPopupMenu, isPopupMenu)
WINDOW_HELPER(bool, isTooltip, isTooltip)
WINDOW_HELPER(bool, isNotification, isNotification)
WINDOW_HELPER(bool, isCriticalNotification, isCriticalNotification)
WINDOW_HELPER(bool, isAppletPopup, isAppletPopup)
WINDOW_HELPER(bool, isOnScreenDisplay, isOnScreenDisplay)
WINDOW_HELPER(bool, isComboBox, isComboBox)
WINDOW_HELPER(bool, isDNDIcon, isDNDIcon)
WINDOW_HELPER(bool, isDeleted, isDeleted)
WINDOW_HELPER(QString, windowRole, windowRole)
WINDOW_HELPER(QStringList, activities, activities)
WINDOW_HELPER(bool, skipsCloseAnimation, skipsCloseAnimation)
WINDOW_HELPER(SurfaceInterface *, surface, surface)
WINDOW_HELPER(bool, isPopupWindow, isPopupWindow)
WINDOW_HELPER(bool, isOutline, isOutline)
WINDOW_HELPER(bool, isLockScreen, isLockScreen)
WINDOW_HELPER(pid_t, pid, pid)
WINDOW_HELPER(QUuid, internalId, internalId)
WINDOW_HELPER(bool, isMinimized, isMinimized)
WINDOW_HELPER(bool, isHidden, isHidden)
WINDOW_HELPER(bool, isHiddenByShowDesktop, isHiddenByShowDesktop)
WINDOW_HELPER(bool, isModal, isModal)
WINDOW_HELPER(bool, isFullScreen, isFullScreen)
WINDOW_HELPER(bool, keepAbove, keepAbove)
WINDOW_HELPER(bool, keepBelow, keepBelow)
WINDOW_HELPER(QString, caption, caption)
WINDOW_HELPER(bool, isMovable, isMovable)
WINDOW_HELPER(bool, isMovableAcrossScreens, isMovableAcrossScreens)
WINDOW_HELPER(bool, isUserMove, isInteractiveMove)
WINDOW_HELPER(bool, isUserResize, isInteractiveResize)
WINDOW_HELPER(QRectF, iconGeometry, iconGeometry)
WINDOW_HELPER(bool, isSpecialWindow, isSpecialWindow)
WINDOW_HELPER(bool, acceptsFocus, wantsInput)
WINDOW_HELPER(QIcon, icon, icon)
WINDOW_HELPER(bool, isSkipSwitcher, skipSwitcher)
WINDOW_HELPER(bool, decorationHasAlpha, decorationHasAlpha)
WINDOW_HELPER(bool, isUnresponsive, unresponsive)
WINDOW_HELPER(QList<VirtualDesktop *>, desktops, desktops)
WINDOW_HELPER(bool, isInputMethod, isInputMethod)

#undef WINDOW_HELPER

qlonglong EffectWindow::windowId() const
{
    if (X11Window *x11Window = qobject_cast<X11Window *>(d->m_window)) {
        return x11Window->window();
    }
    return 0;
}

QString EffectWindow::windowClass() const
{
    return d->m_window->resourceName() + QLatin1Char(' ') + d->m_window->resourceClass();
}

QRectF EffectWindow::contentsRect() const
{
    return QRectF(d->m_window->clientPos(), d->m_window->clientSize());
}

NET::WindowType EffectWindow::windowType() const
{
    return d->m_window->windowType();
}

QSizeF EffectWindow::basicUnit() const
{
    if (auto window = qobject_cast<X11Window *>(d->m_window)) {
        return window->basicUnit();
    }
    return QSize(1, 1);
}

QRectF EffectWindow::decorationInnerRect() const
{
    return d->m_window->rect() - d->m_window->frameMargins();
}

KDecoration2::Decoration *EffectWindow::decoration() const
{
    return d->m_window->decoration();
}

QByteArray EffectWindow::readProperty(long atom, long type, int format) const
{
    auto x11Window = qobject_cast<X11Window *>(d->m_window);
    if (!x11Window) {
        return QByteArray();
    }
    if (!kwinApp()->x11Connection()) {
        return QByteArray();
    }
    return readWindowProperty(x11Window->window(), atom, type, format);
}

void EffectWindow::deleteProperty(long int atom) const
{
    auto x11Window = qobject_cast<X11Window *>(d->m_window);
    if (!x11Window) {
        return;
    }
    if (kwinApp()->x11Connection()) {
        deleteWindowProperty(x11Window->window(), atom);
    }
}

EffectWindow *EffectWindow::findModal()
{
    Window *modal = d->m_window->findModal();
    if (modal) {
        return modal->effectWindow();
    }

    return nullptr;
}

EffectWindow *EffectWindow::transientFor()
{
    Window *transientFor = d->m_window->transientFor();
    if (transientFor) {
        return transientFor->effectWindow();
    }

    return nullptr;
}

QWindow *EffectWindow::internalWindow() const
{
    if (auto window = qobject_cast<InternalWindow *>(d->m_window)) {
        return window->handle();
    }
    return nullptr;
}

template<typename T>
EffectWindowList getMainWindows(T *c)
{
    const auto mainwindows = c->mainWindows();
    EffectWindowList ret;
    ret.reserve(mainwindows.size());
    std::transform(std::cbegin(mainwindows), std::cend(mainwindows),
                   std::back_inserter(ret),
                   [](auto window) {
                       return window->effectWindow();
                   });
    return ret;
}

EffectWindowList EffectWindow::mainWindows() const
{
    return getMainWindows(d->m_window);
}

void EffectWindow::setData(int role, const QVariant &data)
{
    if (!data.isNull()) {
        d->dataMap[role] = data;
    } else {
        d->dataMap.remove(role);
    }
    Q_EMIT effects->windowDataChanged(this, role);
}

QVariant EffectWindow::data(int role) const
{
    return d->dataMap.value(role);
}

void EffectWindow::elevate(bool elevate)
{
    effects->setElevatedWindow(this, elevate);
}

void EffectWindow::minimize()
{
    if (d->m_window->isClient()) {
        d->m_window->setMinimized(true);
    }
}

void EffectWindow::unminimize()
{
    if (d->m_window->isClient()) {
        d->m_window->setMinimized(false);
    }
}

void EffectWindow::closeWindow()
{
    if (d->m_window->isClient()) {
        d->m_window->closeWindow();
    }
}

bool EffectWindow::isManaged() const
{
    return d->managed;
}

bool EffectWindow::isWaylandClient() const
{
    return d->m_waylandWindow;
}

bool EffectWindow::isX11Client() const
{
    return d->m_x11Window;
}

//****************************************
// EffectWindowGroup
//****************************************

EffectWindowGroup::EffectWindowGroup(Group *group)
    : m_group(group)
{
}

EffectWindowGroup::~EffectWindowGroup()
{
}

EffectWindowList EffectWindowGroup::members() const
{
    const auto memberList = m_group->members();
    EffectWindowList ret;
    ret.reserve(memberList.size());
    std::transform(std::cbegin(memberList), std::cend(memberList), std::back_inserter(ret), [](auto window) {
        return window->effectWindow();
    });
    return ret;
}

/***************************************************************
 WindowQuad
***************************************************************/

WindowQuad WindowQuad::makeSubQuad(double x1, double y1, double x2, double y2) const
{
    Q_ASSERT(x1 < x2 && y1 < y2 && x1 >= left() && x2 <= right() && y1 >= top() && y2 <= bottom());
    WindowQuad ret(*this);
    // vertices are clockwise starting from topleft
    ret.verts[0].px = x1;
    ret.verts[3].px = x1;
    ret.verts[1].px = x2;
    ret.verts[2].px = x2;
    ret.verts[0].py = y1;
    ret.verts[1].py = y1;
    ret.verts[2].py = y2;
    ret.verts[3].py = y2;

    const double xOrigin = left();
    const double yOrigin = top();

    const double widthReciprocal = 1 / (right() - xOrigin);
    const double heightReciprocal = 1 / (bottom() - yOrigin);

    for (int i = 0; i < 4; ++i) {
        const double w1 = (ret.verts[i].px - xOrigin) * widthReciprocal;
        const double w2 = (ret.verts[i].py - yOrigin) * heightReciprocal;

        // Use bilinear interpolation to compute the texture coords.
        ret.verts[i].tx = (1 - w1) * (1 - w2) * verts[0].tx + w1 * (1 - w2) * verts[1].tx + w1 * w2 * verts[2].tx + (1 - w1) * w2 * verts[3].tx;
        ret.verts[i].ty = (1 - w1) * (1 - w2) * verts[0].ty + w1 * (1 - w2) * verts[1].ty + w1 * w2 * verts[2].ty + (1 - w1) * w2 * verts[3].ty;
    }

    return ret;
}

/***************************************************************
 WindowQuadList
***************************************************************/

WindowQuadList WindowQuadList::splitAtX(double x) const
{
    WindowQuadList ret;
    ret.reserve(count());
    for (const WindowQuad &quad : *this) {
        bool wholeleft = true;
        bool wholeright = true;
        for (int i = 0; i < 4; ++i) {
            if (quad[i].x() < x) {
                wholeright = false;
            }
            if (quad[i].x() > x) {
                wholeleft = false;
            }
        }
        if (wholeleft || wholeright) { // is whole in one split part
            ret.append(quad);
            continue;
        }
        if (quad.top() == quad.bottom() || quad.left() == quad.right()) { // quad has no size
            ret.append(quad);
            continue;
        }
        ret.append(quad.makeSubQuad(quad.left(), quad.top(), x, quad.bottom()));
        ret.append(quad.makeSubQuad(x, quad.top(), quad.right(), quad.bottom()));
    }
    return ret;
}

WindowQuadList WindowQuadList::splitAtY(double y) const
{
    WindowQuadList ret;
    ret.reserve(count());
    for (const WindowQuad &quad : *this) {
        bool wholetop = true;
        bool wholebottom = true;
        for (int i = 0; i < 4; ++i) {
            if (quad[i].y() < y) {
                wholebottom = false;
            }
            if (quad[i].y() > y) {
                wholetop = false;
            }
        }
        if (wholetop || wholebottom) { // is whole in one split part
            ret.append(quad);
            continue;
        }
        if (quad.top() == quad.bottom() || quad.left() == quad.right()) { // quad has no size
            ret.append(quad);
            continue;
        }
        ret.append(quad.makeSubQuad(quad.left(), quad.top(), quad.right(), y));
        ret.append(quad.makeSubQuad(quad.left(), y, quad.right(), quad.bottom()));
    }
    return ret;
}

WindowQuadList WindowQuadList::makeGrid(int maxQuadSize) const
{
    if (empty()) {
        return *this;
    }

    // Find the bounding rectangle
    double left = first().left();
    double right = first().right();
    double top = first().top();
    double bottom = first().bottom();

    for (const WindowQuad &quad : std::as_const(*this)) {
        left = std::min(left, quad.left());
        right = std::max(right, quad.right());
        top = std::min(top, quad.top());
        bottom = std::max(bottom, quad.bottom());
    }

    WindowQuadList ret;

    for (const WindowQuad &quad : std::as_const(*this)) {
        const double quadLeft = quad.left();
        const double quadRight = quad.right();
        const double quadTop = quad.top();
        const double quadBottom = quad.bottom();

        // sanity check, see BUG 390953
        if (quadLeft == quadRight || quadTop == quadBottom) {
            ret.append(quad);
            continue;
        }

        // Compute the top-left corner of the first intersecting grid cell
        const double xBegin = left + qFloor((quadLeft - left) / maxQuadSize) * maxQuadSize;
        const double yBegin = top + qFloor((quadTop - top) / maxQuadSize) * maxQuadSize;

        // Loop over all intersecting cells and add sub-quads
        for (double y = yBegin; y < quadBottom; y += maxQuadSize) {
            const double y0 = std::max(y, quadTop);
            const double y1 = std::min(quadBottom, y + maxQuadSize);

            for (double x = xBegin; x < quadRight; x += maxQuadSize) {
                const double x0 = std::max(x, quadLeft);
                const double x1 = std::min(quadRight, x + maxQuadSize);

                ret.append(quad.makeSubQuad(x0, y0, x1, y1));
            }
        }
    }

    return ret;
}

WindowQuadList WindowQuadList::makeRegularGrid(int xSubdivisions, int ySubdivisions) const
{
    if (empty()) {
        return *this;
    }

    // Find the bounding rectangle
    double left = first().left();
    double right = first().right();
    double top = first().top();
    double bottom = first().bottom();

    for (const WindowQuad &quad : *this) {
        left = std::min(left, quad.left());
        right = std::max(right, quad.right());
        top = std::min(top, quad.top());
        bottom = std::max(bottom, quad.bottom());
    }

    double xIncrement = (right - left) / xSubdivisions;
    double yIncrement = (bottom - top) / ySubdivisions;

    WindowQuadList ret;

    for (const WindowQuad &quad : *this) {
        const double quadLeft = quad.left();
        const double quadRight = quad.right();
        const double quadTop = quad.top();
        const double quadBottom = quad.bottom();

        // sanity check, see BUG 390953
        if (quadLeft == quadRight || quadTop == quadBottom) {
            ret.append(quad);
            continue;
        }

        // Compute the top-left corner of the first intersecting grid cell
        const double xBegin = left + qFloor((quadLeft - left) / xIncrement) * xIncrement;
        const double yBegin = top + qFloor((quadTop - top) / yIncrement) * yIncrement;

        // Loop over all intersecting cells and add sub-quads
        for (double y = yBegin; y < quadBottom; y += yIncrement) {
            const double y0 = std::max(y, quadTop);
            const double y1 = std::min(quadBottom, y + yIncrement);

            for (double x = xBegin; x < quadRight; x += xIncrement) {
                const double x0 = std::max(x, quadLeft);
                const double x1 = std::min(quadRight, x + xIncrement);

                ret.append(quad.makeSubQuad(x0, y0, x1, y1));
            }
        }
    }

    return ret;
}

void RenderGeometry::copy(std::span<GLVertex2D> destination)
{
    Q_ASSERT(int(destination.size()) >= size());
    std::copy(cbegin(), cend(), destination.begin());
}

void RenderGeometry::appendWindowVertex(const WindowVertex &windowVertex, qreal deviceScale)
{
    GLVertex2D glVertex;
    switch (m_vertexSnappingMode) {
    case VertexSnappingMode::None:
        glVertex.position = QVector2D(windowVertex.x(), windowVertex.y()) * deviceScale;
        break;
    case VertexSnappingMode::Round:
        glVertex.position = roundVector(QVector2D(windowVertex.x(), windowVertex.y()) * deviceScale);
        break;
    }
    glVertex.texcoord = QVector2D(windowVertex.u(), windowVertex.v());
    append(glVertex);
}

void RenderGeometry::appendWindowQuad(const WindowQuad &quad, qreal deviceScale)
{
    // Geometry assumes we're rendering triangles, so add the quad's
    // vertices as two triangles. Vertex order is top-left, bottom-left,
    // top-right followed by top-right, bottom-left, bottom-right.
    appendWindowVertex(quad[0], deviceScale);
    appendWindowVertex(quad[3], deviceScale);
    appendWindowVertex(quad[1], deviceScale);

    appendWindowVertex(quad[1], deviceScale);
    appendWindowVertex(quad[3], deviceScale);
    appendWindowVertex(quad[2], deviceScale);
}

void RenderGeometry::appendSubQuad(const WindowQuad &quad, const QRectF &subquad, qreal deviceScale)
{
    std::array<GLVertex2D, 4> vertices;
    vertices[0].position = QVector2D(subquad.topLeft());
    vertices[1].position = QVector2D(subquad.topRight());
    vertices[2].position = QVector2D(subquad.bottomRight());
    vertices[3].position = QVector2D(subquad.bottomLeft());

    const auto deviceQuad = QRectF{QPointF(std::round(quad.left() * deviceScale), std::round(quad.top() * deviceScale)),
                                   QPointF(std::round(quad.right() * deviceScale), std::round(quad.bottom() * deviceScale))};

    const QPointF origin = deviceQuad.topLeft();
    const QSizeF size = deviceQuad.size();

#pragma GCC unroll 4
    for (int i = 0; i < 4; ++i) {
        const double weight1 = (vertices[i].position.x() - origin.x()) / size.width();
        const double weight2 = (vertices[i].position.y() - origin.y()) / size.height();
        const double oneMinW1 = 1.0 - weight1;
        const double oneMinW2 = 1.0 - weight2;

        const float u = oneMinW1 * oneMinW2 * quad[0].u() + weight1 * oneMinW2 * quad[1].u()
            + weight1 * weight2 * quad[2].u() + oneMinW1 * weight2 * quad[3].u();
        const float v = oneMinW1 * oneMinW2 * quad[0].v() + weight1 * oneMinW2 * quad[1].v()
            + weight1 * weight2 * quad[2].v() + oneMinW1 * weight2 * quad[3].v();
        vertices[i].texcoord = QVector2D(u, v);
    }

    append(vertices[0]);
    append(vertices[3]);
    append(vertices[1]);

    append(vertices[1]);
    append(vertices[3]);
    append(vertices[2]);
}

void RenderGeometry::postProcessTextureCoordinates(const QMatrix4x4 &textureMatrix)
{
    if (!textureMatrix.isIdentity()) {
        const QVector2D coeff(textureMatrix(0, 0), textureMatrix(1, 1));
        const QVector2D offset(textureMatrix(0, 3), textureMatrix(1, 3));

        for (auto &vertex : (*this)) {
            vertex.texcoord = vertex.texcoord * coeff + offset;
        }
    }
}

/***************************************************************
 Motion1D
***************************************************************/

Motion1D::Motion1D(double initial, double strength, double smoothness)
    : Motion<double>(initial, strength, smoothness)
{
}

Motion1D::Motion1D(const Motion1D &other)
    : Motion<double>(other)
{
}

Motion1D::~Motion1D()
{
}

/***************************************************************
 Motion2D
***************************************************************/

Motion2D::Motion2D(QPointF initial, double strength, double smoothness)
    : Motion<QPointF>(initial, strength, smoothness)
{
}

Motion2D::Motion2D(const Motion2D &other)
    : Motion<QPointF>(other)
{
}

Motion2D::~Motion2D()
{
}

/***************************************************************
 WindowMotionManager
***************************************************************/

WindowMotionManager::WindowMotionManager(bool useGlobalAnimationModifier)
    : m_useGlobalAnimationModifier(useGlobalAnimationModifier)

{
    // TODO: Allow developer to modify motion attributes
} // TODO: What happens when the window moves by an external force?

WindowMotionManager::~WindowMotionManager()
{
}

void WindowMotionManager::manage(EffectWindow *w)
{
    if (m_managedWindows.contains(w)) {
        return;
    }

    double strength = 0.08;
    double smoothness = 4.0;
    if (m_useGlobalAnimationModifier && effects->animationTimeFactor()) {
        // If the factor is == 0 then we just skip the calculation completely
        strength = 0.08 / effects->animationTimeFactor();
        smoothness = effects->animationTimeFactor() * 4.0;
    }

    WindowMotion &motion = m_managedWindows[w];
    motion.translation.setStrength(strength);
    motion.translation.setSmoothness(smoothness);
    motion.scale.setStrength(strength * 1.33);
    motion.scale.setSmoothness(smoothness / 2.0);

    motion.translation.setValue(w->pos());
    motion.scale.setValue(QPointF(1.0, 1.0));
}

void WindowMotionManager::unmanage(EffectWindow *w)
{
    m_movingWindowsSet.remove(w);
    m_managedWindows.remove(w);
}

void WindowMotionManager::unmanageAll()
{
    m_managedWindows.clear();
    m_movingWindowsSet.clear();
}

void WindowMotionManager::calculate(int time)
{
    if (!effects->animationTimeFactor()) {
        // Just skip it completely if the user wants no animation
        m_movingWindowsSet.clear();
        QHash<EffectWindow *, WindowMotion>::iterator it = m_managedWindows.begin();
        for (; it != m_managedWindows.end(); ++it) {
            WindowMotion *motion = &it.value();
            motion->translation.finish();
            motion->scale.finish();
        }
    }

    QHash<EffectWindow *, WindowMotion>::iterator it = m_managedWindows.begin();
    for (; it != m_managedWindows.end(); ++it) {
        WindowMotion *motion = &it.value();
        int stopped = 0;

        // TODO: What happens when distance() == 0 but we are still moving fast?
        // TODO: Motion needs to be calculated from the window's center

        Motion2D *trans = &motion->translation;
        if (trans->distance().isNull()) {
            ++stopped;
        } else {
            // Still moving
            trans->calculate(time);
            const short fx = trans->target().x() <= trans->startValue().x() ? -1 : 1;
            const short fy = trans->target().y() <= trans->startValue().y() ? -1 : 1;
            if (trans->distance().x() * fx / 0.5 < 1.0 && trans->velocity().x() * fx / 0.2 < 1.0
                && trans->distance().y() * fy / 0.5 < 1.0 && trans->velocity().y() * fy / 0.2 < 1.0) {
                // Hide tiny oscillations
                motion->translation.finish();
                ++stopped;
            }
        }

        Motion2D *scale = &motion->scale;
        if (scale->distance().isNull()) {
            ++stopped;
        } else {
            // Still scaling
            scale->calculate(time);
            const short fx = scale->target().x() < 1.0 ? -1 : 1;
            const short fy = scale->target().y() < 1.0 ? -1 : 1;
            if (scale->distance().x() * fx / 0.001 < 1.0 && scale->velocity().x() * fx / 0.05 < 1.0
                && scale->distance().y() * fy / 0.001 < 1.0 && scale->velocity().y() * fy / 0.05 < 1.0) {
                // Hide tiny oscillations
                motion->scale.finish();
                ++stopped;
            }
        }

        // We just finished this window's motion
        if (stopped == 2) {
            m_movingWindowsSet.remove(it.key());
        }
    }
}

void WindowMotionManager::reset()
{
    QHash<EffectWindow *, WindowMotion>::iterator it = m_managedWindows.begin();
    for (; it != m_managedWindows.end(); ++it) {
        WindowMotion *motion = &it.value();
        EffectWindow *window = it.key();
        motion->translation.setTarget(window->pos());
        motion->translation.finish();
        motion->scale.setTarget(QPointF(1.0, 1.0));
        motion->scale.finish();
    }
}

void WindowMotionManager::reset(EffectWindow *w)
{
    QHash<EffectWindow *, WindowMotion>::iterator it = m_managedWindows.find(w);
    if (it == m_managedWindows.end()) {
        return;
    }

    WindowMotion *motion = &it.value();
    motion->translation.setTarget(w->pos());
    motion->translation.finish();
    motion->scale.setTarget(QPointF(1.0, 1.0));
    motion->scale.finish();
}

void WindowMotionManager::apply(EffectWindow *w, WindowPaintData &data)
{
    QHash<EffectWindow *, WindowMotion>::iterator it = m_managedWindows.find(w);
    if (it == m_managedWindows.end()) {
        return;
    }

    // TODO: Take into account existing scale so that we can work with multiple managers (E.g. Present windows + grid)
    WindowMotion *motion = &it.value();
    data += (motion->translation.value() - QPointF(w->x(), w->y()));
    data *= QVector2D(motion->scale.value());
}

void WindowMotionManager::moveWindow(EffectWindow *w, QPoint target, double scale, double yScale)
{
    QHash<EffectWindow *, WindowMotion>::iterator it = m_managedWindows.find(w);
    Q_ASSERT(it != m_managedWindows.end()); // Notify the effect author that they did something wrong

    WindowMotion *motion = &it.value();

    if (yScale == 0.0) {
        yScale = scale;
    }
    QPointF scalePoint(scale, yScale);

    if (motion->translation.value() == target && motion->scale.value() == scalePoint) {
        return; // Window already at that position
    }

    motion->translation.setTarget(target);
    motion->scale.setTarget(scalePoint);

    m_movingWindowsSet << w;
}

QRectF WindowMotionManager::transformedGeometry(EffectWindow *w) const
{
    QHash<EffectWindow *, WindowMotion>::const_iterator it = m_managedWindows.constFind(w);
    if (it == m_managedWindows.end()) {
        return w->frameGeometry();
    }

    const WindowMotion *motion = &it.value();
    QRectF geometry(w->frameGeometry());

    // TODO: Take into account existing scale so that we can work with multiple managers (E.g. Present windows + grid)
    geometry.moveTo(motion->translation.value());
    geometry.setWidth(geometry.width() * motion->scale.value().x());
    geometry.setHeight(geometry.height() * motion->scale.value().y());

    return geometry;
}

void WindowMotionManager::setTransformedGeometry(EffectWindow *w, const QRectF &geometry)
{
    QHash<EffectWindow *, WindowMotion>::iterator it = m_managedWindows.find(w);
    if (it == m_managedWindows.end()) {
        return;
    }
    WindowMotion *motion = &it.value();
    motion->translation.setValue(geometry.topLeft());
    motion->scale.setValue(QPointF(geometry.width() / qreal(w->width()), geometry.height() / qreal(w->height())));
}

QRectF WindowMotionManager::targetGeometry(EffectWindow *w) const
{
    QHash<EffectWindow *, WindowMotion>::const_iterator it = m_managedWindows.constFind(w);
    if (it == m_managedWindows.end()) {
        return w->frameGeometry();
    }

    const WindowMotion *motion = &it.value();
    QRectF geometry(w->frameGeometry());

    // TODO: Take into account existing scale so that we can work with multiple managers (E.g. Present windows + grid)
    geometry.moveTo(motion->translation.target());
    geometry.setWidth(geometry.width() * motion->scale.target().x());
    geometry.setHeight(geometry.height() * motion->scale.target().y());

    return geometry;
}

EffectWindow *WindowMotionManager::windowAtPoint(QPoint point, bool useStackingOrder) const
{
    // TODO: Stacking order uses EffectsHandler::stackingOrder() then filters by m_managedWindows
    QHash<EffectWindow *, WindowMotion>::ConstIterator it = m_managedWindows.constBegin();
    while (it != m_managedWindows.constEnd()) {
        if (exclusiveContains(transformedGeometry(it.key()), point)) {
            return it.key();
        }
        ++it;
    }

    return nullptr;
}

} // namespace

#include "moc_kwineffects.cpp"
#include "moc_kwinglobals.cpp"
