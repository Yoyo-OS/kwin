#include "splitscreen.h"

#include <QtCore>
#include <QMouseEvent>
#include <QtMath>

#include <kwinglutils.h>
#include <effects.h>

#define BRIGHTNESS  0.4
#define SCALE_F     1.0
#define SCALE_S     2.0
#define WINDOW_W_H  300

namespace KWin
{

SplitScreenEffect::SplitScreenEffect()
{
    reconfigure(ReconfigureAll);

    m_backgroundMode = int(QuickTileFlag::None);

    connect(effects, &EffectsHandler::windowStartUserMovedResized, this, &SplitScreenEffect::slotWindowStartUserMovedResized);
    connect(effects, &EffectsHandler::windowFinishUserMovedResized, this, &SplitScreenEffect::slotWindowFinishUserMovedResized);
    connect(effects, &EffectsHandler::windowQuickTileModeChanged, this, &SplitScreenEffect::slotWindowQuickTileModeChanged);
}

SplitScreenEffect::~SplitScreenEffect()
{
}

void SplitScreenEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)
}

void SplitScreenEffect::prePaintScreen(ScreenPrePaintData &data, int time)
{
    if (isActive()) {
        data.mask |= PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS;

        for (auto& mm: m_motionManagers) {
            mm.calculate(time/2.0);
        }
    }

    for (auto const& w: effects->stackingOrder()) {
        w->setData(WindowForceBlurRole, QVariant(true));
    }

    effects->prePaintScreen(data, time);
}

void SplitScreenEffect::paintScreen(int mask, QRegion region, ScreenPaintData &data)
{
    effects->paintScreen(mask, region, data);
}

void SplitScreenEffect::postPaintScreen()
{
    if (m_activated)
        effects->addRepaintFull();

    for (auto const& w: effects->stackingOrder()) {
        w->setData(WindowForceBlurRole, QVariant());
    }
    effects->postPaintScreen();
}

void SplitScreenEffect::prePaintWindow(EffectWindow *w, WindowPrePaintData &data, int time)
{
    data.mask |= PAINT_WINDOW_TRANSFORMED;

    if (m_activated) {
        w->enablePainting(EffectWindow::PAINT_DISABLED_BY_MINIMIZE);   // Display always
    }
    w->enablePainting(EffectWindow::PAINT_DISABLED);
    if (!(w->isDock() || w->isDesktop() || isRelevantWithPresentWindows(w))) {
        w->disablePainting(EffectWindow::PAINT_DISABLED);
        w->disablePainting(EffectWindow::PAINT_DISABLED_BY_MINIMIZE);
    }

    effects->prePaintWindow(w, data, time);
}

void SplitScreenEffect::paintWindow(EffectWindow *w, int mask, QRegion region, WindowPaintData &data)
{
    if (!isActive()) {
        effects->paintWindow(w, mask, region, data);
        return;
    }

    int desktop = effects->currentDesktop();
    WindowMotionManager& wmm = m_motionManagers[desktop-1];
    if (wmm.isManaging(w) || w->isDesktop()) {
        auto area = effects->clientArea(FullArea/*ScreenArea*/, m_screen, 0);

        WindowPaintData d = data;
        if (w->isDesktop()) {
            d.setBrightness(BRIGHTNESS);
            effects->paintWindow(w, mask, area, d);
        } else if (!w->isDesktop()) {
            //NOTE: add lanczos will make partial visible window be rendered completely,
            //but slow down the animation
            //mask |= PAINT_WINDOW_LANCZOS;
            auto geo = m_motionManagers[desktop-1].transformedGeometry(w);

            if (m_hoverwin == w) {
                d += QPoint(qRound(geo.x() - w->x()), qRound(geo.y() - w->y()));
                d.setScale(QVector2D((float)geo.width() / w->width() + 0.015, (float)geo.height() / w->height() + + 0.015));
            }
            else {
                d += QPoint(qRound(geo.x() - w->x()), qRound(geo.y() - w->y()));
                d.setScale(QVector2D((float)geo.width() / w->width(), (float)geo.height() / w->height()));
            }


            effects->paintWindow(w, mask, area, d);
        }
    } else {
        effects->paintWindow(w, mask, region, data);
    }
}

void SplitScreenEffect::windowInputMouseEvent(QEvent* e)
{
    if (!m_activated)
        return;

    switch (e->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
            break;
        default:
            return;
    }

    auto me = static_cast<QMouseEvent*>(e);

    EffectWindow* target = nullptr;
    WindowMotionManager& wm = m_motionManagers[effects->currentDesktop()-1];
    for (const auto& w : wm.managedWindows()) {
        auto geo = wm.transformedGeometry(w);
        if (geo.contains(me->pos())) {
            target = w;
            break;
        }
    }

    switch (me->type()) {
        case QEvent::MouseMove:
            if (target) {
                m_hoverwin = target;
            } else {
                m_hoverwin = nullptr;
            }

            return;
        case QEvent::MouseButtonPress:
            if (target) {
                effects->setElevatedWindow(target, true);
                effects->addRepaintFull();
            }
            break;
        case QEvent::MouseButtonRelease:
            if (target) {
                effects->defineCursor(Qt::PointingHandCursor);
                effects->setElevatedWindow(target, false);
                effects->activateWindow(target);
                effects->setSplitWindow(target, m_backgroundMode);
            }
            setActive(false);
            break;
        default:
            return;
    }
}

void SplitScreenEffect::grabbedKeyboardEvent(QKeyEvent* e)
{

}

void SplitScreenEffect::slotWindowStartUserMovedResized(EffectWindow *w)
{
    if (!w->isMovable())
        return;

    m_window = w;
    m_geometry = w->geometry();

    auto wImpl = static_cast<EffectWindowImpl*>(w);
    if (wImpl && !m_cacheClient)
        m_cacheClient = static_cast<AbstractClient*>(wImpl->window());

    setActive(false);
}

void SplitScreenEffect::slotWindowQuickTileModeChanged(EffectWindow *w)
{
    auto c = static_cast<AbstractClient*>(static_cast<EffectWindowImpl*>(w)->window());
    if (c != m_cacheClient && !m_cacheClient)
        return;

    m_quickTileMode = m_cacheClient->quickTileMode();
}

void SplitScreenEffect::slotWindowFinishUserMovedResized(EffectWindow *w)
{
    if (!m_cacheClient)
        return;

    if (!isEnterSplitMode(m_quickTileMode)) {
        m_cacheClient = nullptr;
        m_quickTileMode = QuickTileMode(QuickTileFlag::None);
        return;
    }

    QPoint pos(w->geometry().x(), w->geometry().y());
    m_backgroundRect = getPreviewWindowsGeometry(pos);
    m_screen = w->screen();

    setActive(true);

    m_cacheClient = nullptr;
    m_quickTileMode = QuickTileMode(QuickTileFlag::None);
}

bool SplitScreenEffect::isEnterSplitMode(QuickTileMode mode)
{
    return ((mode & QuickTileFlag::Left) || (mode & QuickTileFlag::Right))
            && !((mode & QuickTileFlag::Top) || (mode & QuickTileFlag::Bottom));
}

QRect SplitScreenEffect::getPreviewWindowsGeometry(QPoint pos)
{
    QRect ret = effects->clientArea(MaximizeArea, pos, effects->currentDesktop());
    if (m_quickTileMode & QuickTileFlag::Left) {
        ret.setLeft(ret.right() - (ret.width() - ret.width() / 2) + 1);
        m_backgroundMode = int(QuickTileFlag::Right);
    } else if (m_quickTileMode & QuickTileFlag::Right) {
        ret.setRight(ret.left() + ret.width() / 2 - 1);
        m_backgroundMode = int(QuickTileFlag::Left);
    }

    return ret;
}

bool SplitScreenEffect::isActive() const
{
    return m_activated && !effects->isScreenLocked();
}

void SplitScreenEffect::cleanup()
{
    if (m_activated)
        return;

    if (m_hasKeyboardGrab)
        effects->ungrabKeyboard();
    m_hasKeyboardGrab = false;
    effects->stopMouseInterception(this);
    effects->setActiveFullScreenEffect(0);

    while (m_motionManagers.size() > 0) {
        m_motionManagers.first().unmanageAll();
        m_motionManagers.removeFirst();
    }
}

void SplitScreenEffect::setActive(bool active)
{
    if (effects->activeFullScreenEffect() && effects->activeFullScreenEffect() != this)
        return;

    if (m_activated == active)
        return;

    m_activated = active;

    cleanup();

    if (active) {
        effects->startMouseInterception(this, Qt::PointingHandCursor);
        m_hasKeyboardGrab = effects->grabKeyboard(this);
        effects->setActiveFullScreenEffect(this);

        EffectWindowList windows = effects->stackingOrder();
        for (int i = 1; i <= effects->numberOfDesktops(); i++) {
            WindowMotionManager wmm;
            for (const auto& w: windows) {
                if (w->isOnDesktop(i) && isRelevantWithPresentWindows(w)) {
                    if (w == m_window)
                        continue;

                    wmm.manage(w);
                }
            }

            calculateWindowTransformations(wmm.managedWindows(), wmm);
            m_motionManagers.append(wmm);
        }
    } else {
        auto p = m_motionManagers.begin();
        while (p != m_motionManagers.end()) {
            foreach (EffectWindow* w, p->managedWindows()) {
                p->moveWindow(w, w->geometry());
            }
            ++p;
        }
    }

    effects->addRepaintFull();
}

bool SplitScreenEffect::isRelevantWithPresentWindows(EffectWindow *w) const
{
    if (w->isSpecialWindow() || w->isUtility()) {
        return false;
    }

    if (w->isDock()) {
        return false;
    }

    if (w->isSkipSwitcher()) {
        return false;
    }

    if (w->isDeleted()) {
        return false;
    }

    if (!w->acceptsFocus()) {
        return false;
    }

    if (!w->isCurrentTab()) {
        return false;
    }

    if (!w->isOnCurrentActivity()) {
        return false;
    }

    return true;
}

void SplitScreenEffect::calculateWindowTransformations(EffectWindowList windows, WindowMotionManager& wmm)
{
    if (windows.size() == 0)
        return;

    calculateWindowTransformationsClosest(windows, m_screen, wmm);
}

static inline int distance(QPoint &pos1, QPoint &pos2)
{
    const int xdiff = pos1.x() - pos2.x();
    const int ydiff = pos1.y() - pos2.y();
    return int(sqrt(float(xdiff*xdiff + ydiff*ydiff)));
}

void SplitScreenEffect::calculateWindowTransformationsClosest(EffectWindowList windowlist, int screen,
        WindowMotionManager& motionManager)
{
    // This layout mode requires at least one window visible
    if (windowlist.count() == 0)
        return;

    QRect area = m_backgroundRect;

    int columns = int(ceil(sqrt(double(windowlist.count()))));
    int rows = int(ceil(windowlist.count() / double(columns)));

    int desktop = windowlist[0]->desktop();
    m_gridSizes[desktop].columns = columns;
    m_gridSizes[desktop].rows = rows;

    // Assign slots
    int slotWidth = area.width() / columns;
    int slotHeight = area.height() / rows;
    QVector<EffectWindow*> takenSlots;
    takenSlots.resize(rows*columns);
    takenSlots.fill(0);

    // precalculate all slot centers
    QVector<QPoint> slotCenters;
    slotCenters.resize(rows*columns);
    for (int x = 0; x < columns; ++x) {
        for (int y = 0; y < rows; ++y) {
            slotCenters[x + y*columns] = QPoint(area.x() + slotWidth * x + slotWidth / 2,
                                                area.y() + slotHeight * y + slotHeight / 2);
        }
    }

    // Assign each window to the closest available slot
    EffectWindowList tmpList = windowlist; // use a QLinkedList copy instead?
    QPoint otherPos;
    while (!tmpList.isEmpty()) {
        EffectWindow *w = tmpList.first();
        int slotCandidate = -1, slotCandidateDistance = INT_MAX;
        QPoint pos = w->geometry().center();
        for (int i = 0; i < columns*rows; ++i) { // all slots
            const int dist = distance(pos, slotCenters[i]);
            if (dist < slotCandidateDistance) { // window is interested in this slot
                EffectWindow *occupier = takenSlots[i];
                assert(occupier != w);
                if (!occupier ||
                    dist < distance((otherPos = occupier->geometry().center()), slotCenters[i])) {
                    // either nobody lives here, or we're better - takeover the slot if it's our best
                    slotCandidate = i;
                    slotCandidateDistance = dist;
                }
            }
        }
        assert(slotCandidate != -1);
        if (takenSlots[slotCandidate])
            tmpList << takenSlots[slotCandidate]; // occupier needs a new home now :p
        tmpList.removeAll(w);
        takenSlots[slotCandidate] = w; // ...and we rumble in =)
    }

    m_takenSlots[desktop] = takenSlots;
    for (int slot = 0; slot < columns * rows; ++slot) {
        EffectWindow *w = takenSlots[slot];
        if (!w) // some slots might be empty
            continue;

        // Work out where the slot is
        QRect target(
            area.x() + (slot % columns) * slotWidth,
            area.y() + (slot / columns) * slotHeight,
            slotWidth, slotHeight);
        target.adjust(35, 35, -35, -35);   // Borders
        double scale = 0;
        if (target.width() / double(w->width()) < target.height() / double(w->height())) {
            // Center vertically
            scale = target.width() / double(w->width());
            target.moveTop(target.top() + (target.height() - int(w->height() * scale)) / 2);
            target.setHeight(int(w->height() * scale));
        } else {
            // Center horizontally
            scale = target.height() / double(w->height());
            target.moveLeft(target.left() + (target.width() - int(w->width() * scale)) / 2);
            target.setWidth(int(w->width() * scale));
        }
        // Don't scale the windows too much
        if (scale > SCALE_S || (scale > SCALE_F && (w->width() > WINDOW_W_H || w->height() > WINDOW_W_H))) {
            scale = (w->width() > WINDOW_W_H || w->height() > WINDOW_W_H) ? SCALE_F : SCALE_S;
            target = QRect(
                         target.center().x() - int(w->width() * scale) / 2,
                         target.center().y() - int(w->height() * scale) / 2,
                         scale * w->width(), scale * w->height());
        }
        motionManager.moveWindow(w, target);
    }
}


} // namespace KWin