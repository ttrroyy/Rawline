#include "include/ui/widget/MikanHome.hpp"
#include "include/ui/widget/MikanIcons.h"
#include "include/ui/widget/NewHome.hpp"

#include "include/ui/mainwindow.h"
#include "include/ui/mainWindow/MainWindowInternal.h"
#include "include/global/Configs.hpp"
#include "include/ui/setting/ThemeManager.hpp"
#include "include/ui/utils/ProfilesTableModel.h"
#include "include/ui/widget/StartStopButton.hpp"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/engine/EngineHost.h"
#include "include/engine/connection/ConnectionManager.h"
#include "include/engine/services/StatsService.h"
#include "include/engine/tests/TestRunner.h"
#include "include/global/Utils.hpp"

#include <QAbstractAnimation>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QMenuBar>
#include <QSlider>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDropEvent>
#include <QFontDatabase>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>
#include <QRegion>
#include <QPointer>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextBrowser>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <tuple>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static QString T(const char *text) { return QCoreApplication::translate("MikanHome", text); }

// A popup is created on the primary display. The anchor's own screen() can stay there after the
// window is dragged, so the screen is taken from the point under the control.
static QScreen *screenContaining(const QPoint &global) {
    if (QScreen *screen = QGuiApplication::screenAt(global)) return screen;
    for (QScreen *screen : QGuiApplication::screens())
        if (screen->geometry().contains(global)) return screen;
    return QGuiApplication::primaryScreen();
}

static QRect screenAvailableAt(const QPoint &global) {
    if (QScreen *screen = screenContaining(global)) return screen->availableGeometry();
    return QRect(0, 0, 1280, 800);
}

static void bindPopupToScreen(QWidget *popup, const QPoint &global) {
    if (!popup) return;
    popup->setAttribute(Qt::WA_NativeWindow);
    popup->winId();
    if (QWindow *handle = popup->windowHandle()) {
        if (QScreen *screen = screenContaining(global)) handle->setScreen(screen);
    }
}

// The mobile app's motion curve: PathInterpolator(.2, 0, 0, 1).
static QEasingCurve softCurve() {
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment(QPointF(.2, 0), QPointF(0, 1), QPointF(1, 1));
    return curve;
}

static int mixPx(int from, int to, qreal t) { return from + qRound((to - from) * t); }

// ---------------------------------------------------------------- style

namespace {
    QString g_theme;
    QString g_family;

    QColor withAlpha(QColor c, int alpha) { c.setAlpha(alpha); return c; }

    // SVG path data as the vector drawables carry it: M L H V C Q A Z, absolute or relative.
    void arcTo(QPainterPath &path, QPointF from, QPointF to, qreal rx, qreal ry, bool large, bool sweep) {
        // Endpoint to centre parametrisation (SVG 1.1 F.6.5), for unrotated ellipses: the icons have no rotation.
        if (rx == 0 || ry == 0) { path.lineTo(to); return; }
        rx = std::abs(rx); ry = std::abs(ry);
        const qreal dx = (from.x() - to.x()) / 2, dy = (from.y() - to.y()) / 2;
        qreal lambda = dx * dx / (rx * rx) + dy * dy / (ry * ry);
        if (lambda > 1) { rx *= std::sqrt(lambda); ry *= std::sqrt(lambda); }
        const qreal num = rx * rx * ry * ry - rx * rx * dy * dy - ry * ry * dx * dx;
        const qreal den = rx * rx * dy * dy + ry * ry * dx * dx;
        qreal coef = den == 0 ? 0 : std::sqrt(std::max<qreal>(0, num / den));
        if (large == sweep) coef = -coef;
        const qreal cxp = coef * rx * dy / ry, cyp = -coef * ry * dx / rx;
        const QPointF centre(cxp + (from.x() + to.x()) / 2, cyp + (from.y() + to.y()) / 2);
        const auto angle = [](qreal ux, qreal uy, qreal vx, qreal vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const qreal start = angle(1, 0, (dx - cxp) / rx, (dy - cyp) / ry);
        qreal span = angle((dx - cxp) / rx, (dy - cyp) / ry, (-dx - cxp) / rx, (-dy - cyp) / ry);
        if (!sweep && span > 0) span -= 2 * M_PI;
        if (sweep && span < 0) span += 2 * M_PI;
        // QPainterPath angles run counter-clockwise on screen, SVG's clockwise.
        path.arcTo(QRectF(centre.x() - rx, centre.y() - ry, 2 * rx, 2 * ry), -start * 180 / M_PI, -span * 180 / M_PI);
    }

    QPainterPath parsePath(const char *data) {
        const QString s = QString::fromLatin1(data);
        QPainterPath path;
        int i = 0;
        QChar cmd;
        QPointF cur, start;
        const auto skip = [&] { while (i < s.size() && (s[i].isSpace() || s[i] == ',')) ++i; };
        const auto number = [&]() -> qreal {
            skip();
            const int begin = i;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
            bool dot = false;
            while (i < s.size()) {
                const QChar c = s[i];
                if (c.isDigit()) ++i;
                else if (c == '.' && !dot) { dot = true; ++i; }
                else if (c == 'e' || c == 'E') { ++i; if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i; }
                else break;
            }
            return s.mid(begin, i - begin).toDouble();
        };
        // Two statements: the order of arguments in one call is unspecified, and MSVC reads y before x.
        const auto point = [&] { const qreal x = number(); const qreal y = number(); return QPointF(x, y); };
        while (true) {
            skip();
            if (i >= s.size()) break;
            if (s[i].isLetter()) cmd = s[i++];
            else if (cmd.isNull()) break;
            const bool rel = cmd.isLower();
            switch (cmd.toUpper().unicode()) {
            case 'M': {
                QPointF p = point();
                if (rel) p += cur;
                path.moveTo(p); cur = start = p;
                cmd = rel ? 'l' : 'L';
                break;
            }
            case 'L': { QPointF p = point(); if (rel) p += cur; path.lineTo(p); cur = p; break; }
            case 'H': { qreal x = number(); if (rel) x += cur.x(); cur.setX(x); path.lineTo(cur); break; }
            case 'V': { qreal y = number(); if (rel) y += cur.y(); cur.setY(y); path.lineTo(cur); break; }
            case 'C': {
                QPointF c1 = point(), c2 = point(), p = point();
                if (rel) { c1 += cur; c2 += cur; p += cur; }
                path.cubicTo(c1, c2, p); cur = p;
                break;
            }
            case 'Q': {
                QPointF c1 = point(), p = point();
                if (rel) { c1 += cur; p += cur; }
                path.quadTo(c1, p); cur = p;
                break;
            }
            case 'A': {
                const qreal rx = number(), ry = number();
                number();
                const bool large = number() != 0, sweep = number() != 0;
                QPointF p = point();
                if (rel) p += cur;
                arcTo(path, cur, p, rx, ry, large, sweep); cur = p;
                break;
            }
            case 'Z': path.closeSubpath(); cur = start; cmd = QChar(); break;
            default: return path;
            }
        }
        return path;
    }

    const MikanIconData *findIcon(const QString &name) {
        for (const auto &icon : mikanIcons) if (name == QLatin1String(icon.name)) return &icon;
        return nullptr;
    }

    // In the new style every control that acts gets the system's pointing hand; the classic one keeps the arrow.
    void giveHand(QWidget *widget) {
        if (widget->testAttribute(Qt::WA_SetCursor)) return;
        if (qobject_cast<QAbstractButton *>(widget) || qobject_cast<QComboBox *>(widget) || qobject_cast<QTabBar *>(widget) ||
            qobject_cast<QMenu *>(widget) || qobject_cast<QMenuBar *>(widget) || qobject_cast<QSlider *>(widget)) {
            widget->setCursor(Qt::PointingHandCursor);
            widget->setProperty("mikanHand", true);
        }
    }

    // A translucent menu with a stylesheet radius paints its corners black: the window stays a rectangle and the
    // unpainted corner pixels are black. Clip the window itself to the rounded shape and keep the fill opaque.
    void prepareMenu(QMenu *menu) {
        menu->setAttribute(Qt::WA_TranslucentBackground, false);
        menu->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        menu->setWindowFlag(Qt::FramelessWindowHint, true);
    }

    void applyMenuMask(QMenu *menu) {
        if (!Mikan::active()) return;
        if (menu->width() < 4 || menu->height() < 4) return;
        const QSize size = menu->size();
        if (menu->property("mikanMenuMask").toSize() == size) return;
        // Record the size first: setMask can deliver Resize before it returns.
        menu->setProperty("mikanMenuMask", size);
        QPainterPath path;
        path.addRoundedRect(QRectF(0, 0, size.width(), size.height()), 16, 16);
        menu->setMask(QRegion(path.toFillPolygon().toPolygon()));
    }

    void restoreMenu(QMenu *menu) {
        menu->setAttribute(Qt::WA_TranslucentBackground, false);
        menu->setWindowFlag(Qt::FramelessWindowHint, false);
        menu->setWindowFlag(Qt::NoDropShadowWindowHint, false);
        menu->clearMask();
        menu->setProperty("mikanMenuMask", QVariant());
    }

    // A menu gets round corners from its window mask; a list popup and a tooltip get the system's round corners.
    class MenuCorners : public QObject {
    public:
        using QObject::QObject;
    protected:
        bool eventFilter(QObject *watched, QEvent *event) override {
            if (!watched->isWidgetType()) return false;
            auto *widget = static_cast<QWidget *>(watched);
            auto *menu = qobject_cast<QMenu *>(widget);
            if (!Mikan::active()) {
                if (menu && event->type() == QEvent::Show && !menu->mask().isEmpty()) menu->clearMask();
                return false;
            }
            if (event->type() == QEvent::Polish) {
                giveHand(widget);
                if (menu) prepareMenu(menu);
            } else if (menu && (event->type() == QEvent::Show || event->type() == QEvent::Resize)) {
                applyMenuMask(menu);
            } else if (event->type() == QEvent::Show && widget->isWindow() &&
                       (widget->inherits("QComboBoxPrivateContainer") || widget->inherits("QTipLabel"))) {
                Mikan::roundCorners(widget, true);
            }
            return false;
        }
    };

    // A second press on the button that opened a popup closes it, rather than closing and opening it again.
    class ReopenGuard : public QObject {
    public:
        ReopenGuard(QWidget *popup, QWidget *anchor) : QObject(popup), anchor(anchor) {
            setObjectName(QStringLiteral("mikanReopenGuard"));
            popup->installEventFilter(this);
        }
        QPointer<QWidget> anchor;
    protected:
        bool eventFilter(QObject *watched, QEvent *event) override {
            auto *popup = static_cast<QWidget *>(watched);
            if (event->type() == QEvent::Show) popup->setAttribute(Qt::WA_NoMouseReplay, false);
            if (event->type() == QEvent::MouseButtonPress && anchor) {
                const QPoint global = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
                if (!popup->rect().contains(popup->mapFromGlobal(global)) && QRect(anchor->mapToGlobal(QPoint()), anchor->size()).contains(global)) {
                    popup->setAttribute(Qt::WA_NoMouseReplay);
                }
            }
            return false;
        }
    };

    void guardReopen(QWidget *popup, QWidget *anchor) {
        if (auto *guard = popup->findChild<QObject *>(QStringLiteral("mikanReopenGuard"), Qt::FindDirectChildrenOnly)) static_cast<ReopenGuard *>(guard)->anchor = anchor;
        else new ReopenGuard(popup, anchor);
    }

    // Fusion under the new palette, with check boxes, radio buttons and menu ticks drawn smooth in the theme's ink.
    class MikanProxyStyle : public QProxyStyle {
    public:
        MikanProxyStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}
        void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *p, const QWidget *widget) const override {
            const bool on = option->state & State_On, partial = option->state & State_NoChange;
            const bool hover = option->state & State_MouseOver;
            const QColor ink = Mikan::text();
            switch (element) {
            case PE_IndicatorCheckBox:
            case PE_IndicatorItemViewItemCheck:
            case PE_IndicatorRadioButton: {
                const qreal side = std::min(option->rect.width(), option->rect.height());
                QRectF box(0, 0, side, side);
                box.moveCenter(QRectF(option->rect).center());
                box.adjust(.75, .75, -.75, -.75);
                p->save();
                p->setRenderHint(QPainter::Antialiasing);
                if (!(option->state & State_Enabled)) p->setOpacity(.45);
                const bool radio = element == PE_IndicatorRadioButton;
                if ((on || partial) && !radio) { p->setPen(Qt::NoPen); p->setBrush(ink); }
                else {
                    p->setPen(QPen(on ? ink : withAlpha(Mikan::muted(), hover ? 235 : 175), 1.3));
                    p->setBrush(hover ? Mikan::overlay(16) : QColor(Qt::transparent));
                }
                if (radio) p->drawEllipse(box);
                else p->drawRoundedRect(box, box.width() * .3, box.width() * .3);
                const qreal x = box.left(), y = box.top(), s = box.width();
                if (radio && on) {
                    p->setPen(Qt::NoPen);
                    p->setBrush(ink);
                    p->drawEllipse(box.center(), s * .26, s * .26);
                } else if (on) {
                    p->setPen(QPen(Mikan::background(), std::max<qreal>(1.4, s * .13), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    p->setBrush(Qt::NoBrush);
                    p->drawPolyline(QPolygonF{QPointF(x + s * .26, y + s * .53), QPointF(x + s * .43, y + s * .70), QPointF(x + s * .75, y + s * .33)});
                } else if (partial) {
                    p->setPen(QPen(Mikan::background(), std::max<qreal>(1.4, s * .13), Qt::SolidLine, Qt::RoundCap));
                    p->drawLine(QPointF(x + s * .28, y + s / 2), QPointF(x + s * .72, y + s / 2));
                }
                p->restore();
                return;
            }
            case PE_IndicatorMenuCheckMark: {
                const qreal s = std::min(option->rect.width(), option->rect.height()) * .8;
                QRectF box(0, 0, s, s);
                box.moveCenter(QRectF(option->rect).center());
                p->save();
                p->setRenderHint(QPainter::Antialiasing);
                if (!(option->state & State_Enabled)) p->setOpacity(.45);
                p->setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->setBrush(Qt::NoBrush);
                const qreal x = box.left(), y = box.top();
                p->drawPolyline(QPolygonF{QPointF(x + s * .18, y + s * .54), QPointF(x + s * .4, y + s * .76), QPointF(x + s * .84, y + s * .26)});
                p->restore();
                return;
            }
            default:
                QProxyStyle::drawPrimitive(element, option, p, widget);
            }
        }
    };
}

QStyle *Mikan::createStyle() { return new MikanProxyStyle; }

void Mikan::roundCorners(QWidget *window, bool compact) {
#ifdef Q_OS_WIN
    // Windows 11 rounds a window without a frame only when asked; older systems ignore the attribute.
    using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static const auto set = [] {
        const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    }();
    if (!set || !window) return;
    const DWORD preference = compact ? 3 : 2; // DWMWCP_ROUNDSMALL, DWMWCP_ROUND
    set(reinterpret_cast<HWND>(window->winId()), 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &preference, sizeof preference);
#else
    Q_UNUSED(window) Q_UNUSED(compact)
#endif
}

bool Mikan::isTheme(const QString &theme) {
    return theme == QLatin1String(DarkTheme) || theme == QLatin1String(LightTheme);
}

void Mikan::setTheme(const QString &theme) {
    static MenuCorners *corners = nullptr;
    if (!corners) { corners = new MenuCorners(qApp); qApp->installEventFilter(corners); }
    if (isTheme(g_theme) && !isTheme(theme)) {
        // Back in the classic look: menus get their frame again, and every control its own cursor.
        for (auto *widget : QApplication::allWidgets()) {
            if (auto *menu = qobject_cast<QMenu *>(widget)) restoreMenu(menu);
            if (widget->property("mikanHand").toBool()) { widget->unsetCursor(); widget->setProperty("mikanHand", QVariant()); }
        }
    }
    const bool entering = !isTheme(g_theme) && isTheme(theme);
    g_theme = theme;
    // Controls made before the switch are polished already.
    if (entering) {
        for (auto *widget : QApplication::allWidgets()) {
            giveHand(widget);
            if (auto *menu = qobject_cast<QMenu *>(widget)) prepareMenu(menu);
        }
    }
}

bool Mikan::active() { return isTheme(g_theme); }
bool Mikan::light() { return g_theme == QLatin1String(LightTheme); }

QColor Mikan::background() { return light() ? QColor(255, 246, 248) : QColor(0, 0, 0); }
QColor Mikan::text() { return light() ? QColor(30, 27, 51) : QColor(245, 245, 247); }
QColor Mikan::muted() { return light() ? QColor(125, 122, 150) : QColor(154, 154, 168); }
QColor Mikan::solid() { return light() ? QColor(255, 255, 255) : QColor(22, 22, 28); }
QColor Mikan::outline() { return withAlpha(muted(), 55); }
QColor Mikan::overlay(int alpha) { return light() ? QColor(0, 0, 0, alpha / 2) : QColor(255, 255, 255, alpha); }

namespace {
struct AccentDef {
    const char *id;
    const char *label;
    Mikan::Accent day;
    Mikan::Accent night;
};

// Day ramp, then night ramp. Marks and text stay readable on the theme background.
// Shine and shade are the power button and the badge: deep enough for a white mark.
const AccentDef kAccents[] = {
    {"violet", "Violet",
     {{109, 74, 224}, {91, 56, 196}, {236, 230, 255}, {230, 222, 255}, {240, 236, 255}, {156, 128, 255}, {236, 230, 255}, {140, 112, 236}, {72, 44, 168}},
     {{168, 146, 255}, {206, 192, 255}, {46, 38, 82}, {52, 44, 92}, {48, 40, 86}, {168, 146, 255}, {44, 40, 62}, {142, 120, 228}, {76, 46, 176}}},
    {"indigo", "Indigo",
     {{64, 70, 214}, {50, 54, 184}, {230, 232, 255}, {224, 226, 255}, {236, 237, 255}, {129, 140, 248}, {230, 232, 255}, {118, 128, 236}, {46, 48, 168}},
     {{160, 168, 255}, {198, 204, 255}, {36, 38, 82}, {42, 44, 92}, {38, 40, 86}, {160, 168, 255}, {40, 42, 64}, {124, 132, 224}, {52, 56, 180}}},
    {"blue", "Blue",
     {{32, 92, 220}, {24, 74, 190}, {226, 236, 255}, {220, 232, 255}, {232, 241, 255}, {96, 165, 250}, {226, 236, 255}, {96, 150, 240}, {24, 64, 176}},
     {{120, 176, 255}, {176, 208, 255}, {28, 44, 82}, {34, 52, 92}, {30, 48, 86}, {120, 176, 255}, {36, 46, 64}, {108, 158, 232}, {30, 78, 190}}},
    {"cyan", "Cyan",
     {{0, 108, 150}, {0, 90, 128}, {220, 244, 252}, {214, 240, 250}, {228, 246, 252}, {40, 180, 220}, {220, 244, 252}, {40, 168, 210}, {0, 82, 122}},
     {{72, 198, 230}, {150, 224, 240}, {16, 48, 64}, {20, 56, 74}, {18, 52, 68}, {72, 198, 230}, {28, 46, 54}, {90, 200, 230}, {0, 100, 140}}},
    {"teal", "Teal",
     {{0, 116, 104}, {0, 96, 86}, {214, 244, 238}, {206, 240, 234}, {224, 248, 242}, {48, 190, 172}, {214, 244, 238}, {36, 176, 160}, {0, 90, 80}},
     {{64, 204, 188}, {150, 230, 218}, {12, 48, 44}, {16, 56, 52}, {14, 52, 48}, {64, 204, 188}, {24, 46, 44}, {80, 206, 190}, {0, 108, 96}}},
    {"green", "Green",
     {{8, 118, 64}, {6, 96, 52}, {214, 244, 226}, {206, 240, 220}, {224, 248, 232}, {48, 186, 116}, {214, 244, 226}, {40, 176, 108}, {4, 90, 50}},
     {{64, 204, 128}, {156, 226, 180}, {12, 48, 30}, {16, 56, 36}, {14, 52, 32}, {64, 204, 128}, {24, 46, 34}, {86, 206, 140}, {8, 112, 64}}},
    {"amber", "Amber",
     {{156, 96, 0}, {128, 76, 0}, {255, 240, 214}, {255, 236, 204}, {255, 244, 224}, {220, 164, 48}, {255, 240, 214}, {214, 156, 32}, {120, 72, 0}},
     {{240, 180, 64}, {255, 214, 140}, {64, 44, 16}, {74, 50, 18}, {68, 46, 16}, {240, 180, 64}, {52, 44, 32}, {220, 168, 56}, {140, 84, 0}}},
    {"orange", "Orange",
     {{186, 72, 8}, {156, 56, 0}, {255, 234, 220}, {255, 228, 210}, {255, 238, 226}, {230, 130, 50}, {255, 234, 220}, {220, 120, 40}, {140, 48, 0}},
     {{250, 150, 72}, {255, 196, 140}, {64, 32, 16}, {74, 38, 18}, {68, 34, 16}, {250, 150, 72}, {54, 40, 32}, {230, 140, 60}, {160, 58, 8}}},
    {"rose", "Rose",
     {{184, 36, 88}, {156, 24, 72}, {255, 228, 236}, {255, 222, 232}, {255, 234, 240}, {230, 110, 150}, {255, 228, 236}, {220, 100, 140}, {136, 20, 64}},
     {{246, 130, 168}, {255, 186, 206}, {64, 24, 40}, {74, 28, 46}, {68, 26, 42}, {246, 130, 168}, {52, 36, 44}, {230, 128, 164}, {156, 28, 76}}},
    {"red", "Red",
     {{184, 32, 40}, {156, 22, 28}, {255, 228, 228}, {255, 222, 222}, {255, 234, 232}, {230, 100, 104}, {255, 228, 228}, {220, 96, 100}, {140, 20, 26}},
     {{246, 128, 124}, {255, 188, 184}, {64, 22, 24}, {74, 26, 28}, {68, 24, 26}, {246, 128, 124}, {52, 36, 36}, {228, 116, 112}, {158, 28, 32}}},
};

const AccentDef &accentDef(const QString &id) {
    for (const auto &item : kAccents)
        if (id == QLatin1String(item.id)) return item;
    return kAccents[0];
}
}

Mikan::Accent Mikan::accent() {
    QString id = QStringLiteral("violet");
    if (Configs::dataManager && Configs::dataManager->settingsRepo && !Configs::dataManager->settingsRepo->accent.isEmpty())
        id = Configs::dataManager->settingsRepo->accent;
    const auto &item = accentDef(id);
    return light() ? item.day : item.night;
}

int Mikan::accentCount() { return int(sizeof kAccents / sizeof kAccents[0]); }
const char *Mikan::accentId(int index) { return kAccents[index].id; }
const char *Mikan::accentLabel(int index) { return kAccents[index].label; }
QColor Mikan::accentPreview(const QString &id, bool day) {
    const auto &item = accentDef(id);
    return (day ? item.day : item.night).mark;
}

QString Mikan::fontFamily() {
    if (g_family.isEmpty()) {
        for (const auto *weight : {"regular", "medium", "bold"}) {
            const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/mikan/font/geologica_%1.ttf").arg(QLatin1String(weight)));
            if (id >= 0 && g_family.isEmpty()) g_family = QFontDatabase::applicationFontFamilies(id).value(0);
        }
        if (g_family.isEmpty()) g_family = qApp->font().family();
    }
    return g_family;
}

QFont Mikan::font(qreal px, int weight) {
    QFont f(fontFamily());
    f.setPixelSize(qRound(px));
    f.setWeight(QFont::Weight(weight));
    return f;
}

QPalette Mikan::palette() {
    const QColor bg = background(), ink = text(), dim = muted();
    const Accent tone = accent();
    QPalette p;
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(group, QPalette::Window, bg);
        p.setColor(group, QPalette::WindowText, ink);
        p.setColor(group, QPalette::Base, bg);
        p.setColor(group, QPalette::AlternateBase, solid());
        p.setColor(group, QPalette::Text, ink);
        p.setColor(group, QPalette::Button, solid());
        p.setColor(group, QPalette::ButtonText, ink);
        p.setColor(group, QPalette::BrightText, bg);
        p.setColor(group, QPalette::ToolTipBase, solid());
        p.setColor(group, QPalette::ToolTipText, ink);
        p.setColor(group, QPalette::Highlight, tone.highlight);
        p.setColor(group, QPalette::HighlightedText, ink);
        p.setColor(group, QPalette::Link, tone.text);
        p.setColor(group, QPalette::LinkVisited, tone.text);
        p.setColor(group, QPalette::PlaceholderText, dim);
        p.setColor(group, QPalette::Light, solid().lighter(130));
        p.setColor(group, QPalette::Midlight, solid().lighter(115));
        p.setColor(group, QPalette::Mid, outline());
        p.setColor(group, QPalette::Dark, dim);
        p.setColor(group, QPalette::Shadow, bg);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        p.setColor(group, QPalette::Accent, tone.mark);
#endif
    }
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::HighlightedText}) {
        p.setColor(QPalette::Disabled, role, withAlpha(dim, 160));
    }
    return p;
}

QString Mikan::styleSheet() {
    const auto rgba = [](const QColor &c) {
        return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
    };
    const bool day = light();
    const QColor ink = text(), dim = muted(), line = outline();
    const Accent tone = accent();
    const QColor glassTop = day ? QColor(255, 255, 255, 236) : QColor(255, 255, 255, 16);
    const QColor glassBottom = day ? QColor(255, 252, 255, 214) : QColor(255, 255, 255, 10);
    const QColor rim = day ? QColor(0, 0, 0, 22) : QColor(255, 255, 255, 26);
    const QColor well = day ? QColor(0, 0, 0, 10) : QColor(255, 255, 255, 14);
    const QColor hover = overlay(22), press = overlay(44);
    const QColor tint = withAlpha(tone.glow, day ? 70 : 110), tintRim = tone.glow;
    const QColor menu = day ? QColor(250, 250, 252) : QColor(22, 22, 28);
    const QString glass = QStringLiteral("qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %1,stop:1 %2)").arg(rgba(glassTop), rgba(glassBottom));
    return QStringLiteral(
        "QMainWindow, QDialog { background: %1; }\n"
        "QToolTip { background: %2; color: %3; border: 1px solid %4; padding: 6px 10px; }\n"
        "QMenu { background: %5; border: none; padding: 8px; }\n"
        "QMenu::item { padding: 7px 18px 7px 14px; border-radius: 12px; background: transparent; color: %3; }\n"
        "QMenu::item:selected { background: %6; }\n"
        "QMenu::item:disabled { color: %7; }\n"
        "QMenu::separator { height: 1px; background: %4; margin: 5px 10px; }\n"
        "QMenu::icon { padding-left: 6px; }\n"
        "QPushButton { background: %8; border: 1px solid %9; border-radius: 14px; padding: 7px 16px; color: %3; }\n"
        "QPushButton:hover { background: %6; }\n"
        "QPushButton:pressed { background: %10; }\n"
        "QPushButton:disabled { color: %7; }\n"
        "QPushButton:default { border: 1px solid %11; }\n"
        "QToolButton { background: transparent; border: none; border-radius: 10px; padding: 4px; }\n"
        "QToolButton:hover { background: %6; }\n"
        "QToolButton:checked, QToolButton:pressed { background: %10; }\n"
        "QLineEdit, QPlainTextEdit, QTextEdit, QAbstractSpinBox, QComboBox, QKeySequenceEdit { background: %12; border: 1px solid %9; border-radius: 12px; padding: 5px 10px; selection-background-color: %13; selection-color: %3; }\n"
        "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QAbstractSpinBox:focus, QComboBox:focus { border: 1px solid %11; }\n"
        "QComboBox::drop-down { border: none; width: 22px; }\n"
        "QComboBox QAbstractItemView { background: %2; border: none; selection-background-color: %6; outline: 0; padding: 4px; }\n"
        "QComboBox QAbstractItemView::item, QListView::item { min-height: 26px; padding: 2px 8px; border-radius: 8px; border: none; color: %3; }\n"
        "QComboBox QAbstractItemView::item:hover, QListView::item:hover { background: %6; }\n"
        "QComboBox QAbstractItemView::item:selected, QListView::item:selected { background: %13; color: %3; }\n"
        "QTabWidget::pane { border: 1px solid %9; border-radius: 18px; background: transparent; top: -1px; }\n"
        "QTabBar::tab { background: transparent; border: 1px solid %9; border-radius: 12px; padding: 5px 11px; margin: 2px 2px; color: %7; }\n"
        "QTabBar::tab:selected { background: %13; color: %3; border: 1px solid %14; }\n"
        "QTabBar::tab:hover:!selected { background: %6; }\n"
        "QHeaderView { background: transparent; }\n"
        "QHeaderView::section { background: transparent; border: none; border-bottom: 1px solid %4; padding: 6px; color: %7; }\n"
        "QTableView, QTreeView, QListView, QListWidget, QTableWidget, QTreeWidget { background: transparent; border: none; alternate-background-color: %12; selection-background-color: %13; selection-color: %3; gridline-color: %4; outline: 0; }\n"
        "QScrollArea { background: transparent; border: none; }\n"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }\n"
        "QScrollBar::handle:vertical { background: %15; border-radius: 3px; min-height: 30px; margin: 0 2px; }\n"
        "QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }\n"
        "QScrollBar::handle:horizontal { background: %15; border-radius: 3px; min-width: 30px; margin: 2px 0; }\n"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }\n"
        "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }\n"
        "QGroupBox { border: 1px solid %9; border-radius: 18px; margin-top: 14px; padding-top: 8px; }\n"
        "QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 4px; color: %7; }\n"
        "QProgressBar { background: %12; border: 1px solid %9; border-radius: 8px; text-align: center; }\n"
        "QProgressBar::chunk { background: %14; border-radius: 7px; }\n"
        "QSplitter::handle { background: transparent; }\n"
        "#stats_widget::pane { border: none; }\n"
        "QLabel[mikanRole=\"muted\"] { color: %7; }\n"
        "QLabel[mikanRole=\"link\"] { color: %11; }\n"
        "QLabel[mikanRole=\"link\"]:hover { color: %3; }\n"
        "QWidget#mikanCard { background: transparent; }\n"
    ).arg(rgba(background()), rgba(solid()), rgba(ink), rgba(line), rgba(menu), rgba(hover), rgba(dim), glass, rgba(rim))
     .arg(rgba(press), rgba(tone.text), rgba(well), rgba(tint), rgba(tintRim), rgba(withAlpha(dim, 110)));
}

void Mikan::paintBackdrop(QPainter &p, const QRect &r) {
    if (!light()) {
        p.fillRect(r, background());
        return;
    }
    QLinearGradient wash(r.topLeft(), r.bottomRight());
    wash.setColorAt(0, QColor(255, 214, 196));
    wash.setColorAt(.38, QColor(255, 226, 236));
    wash.setColorAt(.68, QColor(230, 224, 255));
    wash.setColorAt(1, QColor(198, 220, 255));
    p.fillRect(r, wash);
    QRadialGradient peach(QPointF(r.width() * .08, r.height() * .02), r.width() * .55);
    peach.setColorAt(0, QColor(255, 186, 164, 90));
    peach.setColorAt(1, QColor(255, 186, 164, 0));
    p.fillRect(r, peach);
    QRadialGradient mint(QPointF(r.right() - r.width() * .04, r.bottom() - r.height() * .08), r.width() * .48);
    mint.setColorAt(0, QColor(176, 230, 214, 110));
    mint.setColorAt(1, QColor(176, 230, 214, 0));
    p.fillRect(r, mint);
}

void Mikan::paintGlass(QPainter &p, const QRectF &r0, qreal radius, const QColor &tint) {
    const bool day = light();
    const QRectF r = r0.adjusted(.5, .5, -.5, -.5);
    const qreal corner = std::min({radius, r.height() / 2, r.width() / 2});
    QColor high, low;
    if (tint.isValid()) { high = withAlpha(tint, day ? 42 : 48); low = withAlpha(tint, day ? 24 : 28); }
    else { high = day ? QColor(255, 255, 255, 238) : QColor(255, 255, 255, 16); low = day ? QColor(255, 252, 255, 220) : QColor(255, 255, 255, 10); }
    QLinearGradient body(0, r0.top(), 0, r0.bottom());
    body.setColorAt(0, high);
    body.setColorAt(1, low);
    const QColor line = tint.isValid() ? tint : (day ? QColor(0, 0, 0) : QColor(255, 255, 255));
    QLinearGradient rim(0, r0.top(), 0, r0.bottom());
    rim.setColorAt(0, withAlpha(line, tint.isValid() ? 90 : day ? 20 : 30));
    rim.setColorAt(1, withAlpha(line, tint.isValid() ? 50 : day ? 12 : 14));
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QBrush(rim), .75));
    p.setBrush(body);
    p.drawRoundedRect(r, corner, corner);
    p.restore();
}

void Mikan::paintIcon(QPainter &p, const QString &name, const QRectF &box, const QColor &color) {
    const auto *icon = findIcon(name);
    if (!icon) return;
    static QHash<QString, QList<QPainterPath>> cache;
    auto it = cache.find(name);
    if (it == cache.end()) {
        QList<QPainterPath> paths;
        for (int i = 0; i < icon->count; ++i) paths << parsePath(icon->paths[i].d);
        it = cache.insert(name, paths);
    }
    const qreal scale = std::min(box.width(), box.height()) / icon->viewport;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.center().x() - icon->viewport * scale / 2, box.center().y() - icon->viewport * scale / 2);
    p.scale(scale, scale);
    for (int i = 0; i < icon->count; ++i) {
        if (icon->paths[i].stroke > 0) {
            p.setPen(QPen(color, icon->paths[i].stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(color);
        }
        p.drawPath(it.value()[i]);
    }
    p.restore();
}

QIcon Mikan::icon(const QString &name, const QColor &color) {
    QIcon result;
    for (const int size : {16, 24, 32, 48}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        paintIcon(p, name, QRectF(0, 0, size, size), color);
        p.end();
        result.addPixmap(pixmap);
    }
    return result;
}

QString Mikan::leadingFlag(const QString &name) {
    static const QRegularExpression flag(QStringLiteral(
        "^(?:[\\x{1F1E6}-\\x{1F1FF}]{2}|[\\x{1F3F3}\\x{1F3F4}\\x{1F6A9}\\x{1F38C}\\x{2690}\\x{2691}]\\x{FE0F}?(?:\\x{200D}[^\\s]+|[\\x{E0061}-\\x{E007F}]+)?)"));
    return flag.match(name).captured(0);
}

QPixmap Mikan::flag(const QString &leading, int size, qreal dpr) {
    if (leading.isEmpty()) return {};
    static QHash<QString, QPixmap> cache;
    const QString key = leading + QString::number(size) + QLatin1Char('@') + QString::number(dpr);
    if (const auto it = cache.constFind(key); it != cache.constEnd()) return it.value();
    const int px = qRound(size * dpr);
    QPixmap art;
    const auto ucs = leading.toUcs4();
    if (ucs.size() >= 2 && ucs[0] >= 0x1F1E6 && ucs[0] <= 0x1F1FF && ucs[1] >= 0x1F1E6 && ucs[1] <= 0x1F1FF) {
        const QString code = QString(QChar('a' + int(ucs[0] - 0x1F1E6))) + QChar('a' + int(ucs[1] - 0x1F1E6));
        art = QPixmap(QStringLiteral(":/mikan/flags/%1.png").arg(code));
        if (!art.isNull()) art = art.scaled(px, px, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (art.isNull()) {
        // A flag with no picture of its own, drawn by the emoji font; the black one is just black.
        art = QPixmap(px, px);
        art.fill(leading.startsWith(QString::fromUcs4(U"\U0001F3F4")) && ucs.size() <= 2 ? QColor(0, 0, 0) : QColor(0, 0, 0, 0));
        if (!(leading.startsWith(QString::fromUcs4(U"\U0001F3F4")) && ucs.size() <= 2)) {
            QPainter p(&art);
            QFont f = qApp->font();
            f.setPixelSize(qRound(px * .72));
            p.setFont(f);
            p.drawText(QRect(0, 0, px, px), Qt::AlignCenter, leading);
        }
    }
    art.setDevicePixelRatio(dpr);
    cache.insert(key, art);
    return art;
}

void Mikan::rise(QWidget *widget, int distance, int duration) {
    if (!widget || !widget->isVisible() || widget->property("mikanRising").toBool()) return;
    widget->setProperty("mikanRising", true);
    auto *effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(0);
    widget->setGraphicsEffect(effect);
    const QPoint end = widget->pos();
    auto *anim = new QVariantAnimation(widget);
    anim->setDuration(duration);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setEasingCurve(softCurve());
    QObject::connect(anim, &QVariantAnimation::valueChanged, widget, [widget, effect, end, distance](const QVariant &value) {
        const qreal t = value.toReal();
        effect->setOpacity(t);
        widget->move(end.x(), end.y() + qRound(distance * (1 - t)));
    });
    QObject::connect(anim, &QVariantAnimation::finished, widget, [widget] {
        widget->setGraphicsEffect(nullptr);
        widget->setProperty("mikanRising", false);
        if (auto *parent = widget->parentWidget(); parent && parent->layout()) {
            parent->layout()->invalidate();
            parent->layout()->activate();
        }
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

// ---------------------------------------------------------------- tiles

static void clearPlate(QWidget *widget) {
    widget->setObjectName(QStringLiteral("mikanCard"));
    widget->setAutoFillBackground(false);
    widget->setAttribute(Qt::WA_NoSystemBackground, true);
    widget->setAttribute(Qt::WA_StyledBackground, true);
}

MikanTile::MikanTile(Kind kind, QWidget *parent) : QAbstractButton(parent), m_kind(kind) {
    clearPlate(this);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
}

void MikanTile::setSpinning(bool spinning) {
    if (spinning == (m_spin != nullptr)) return;
    if (!spinning) { delete m_spin; m_spin = nullptr; m_angle = 0; update(); return; }
    m_spin = new QTimer(this);
    connect(m_spin, &QTimer::timeout, this, [this] { m_angle = std::fmod(m_angle + 6.4, 360); update(); });
    m_spin->start(16);
}

QSize MikanTile::sizeHint() const {
    const QFontMetrics fm(Mikan::font(m_textSize, m_kind == Chip ? QFont::Bold : QFont::Normal));
    switch (m_kind) {
    case Icon: return m_flat ? QSize(40, 48) : QSize(48, 48);
    case Chip: return {fm.horizontalAdvance(text()) + 16 + 12 + 30, 48};
    case Tab: return {fm.horizontalAdvance(text()) + 36, 44};
    case Text:
        if (m_alignLeft) {
            int width = fm.horizontalAdvance(text()) + (m_chevron || m_mark ? 56 : 36);
            if (!m_trailing.isEmpty()) width += QFontMetrics(Mikan::font(12)).horizontalAdvance(m_trailing) + 12;
            return {width, 48};
        }
        if (!m_subtitle.isEmpty())
            return {std::max(fm.horizontalAdvance(text()), fm.horizontalAdvance(m_subtitle)) + (m_expandable ? 56 : 32), 60};
        // Icon and the trailing chevron sit outside the label, so the hint has to include them or the label is cut.
        if (!m_icon.isEmpty()) return {fm.horizontalAdvance(text()) + 76, 48};
        return {fm.horizontalAdvance(text()) + 24, 52};
    }
    return {48, 48};
}

void MikanTile::animatePress(qreal to, int duration) {
    if (!m_pressAnim) {
        m_pressAnim = new QVariantAnimation(this);
        connect(m_pressAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { m_press = v.toReal(); update(); });
    }
    m_pressAnim->stop();
    m_pressAnim->setDuration(duration);
    m_pressAnim->setStartValue(m_press);
    m_pressAnim->setEndValue(to);
    m_pressAnim->setEasingCurve(QEasingCurve::OutCubic);
    m_pressAnim->start();
}

void MikanTile::enterEvent(QEnterEvent *e) { m_hover = true; update(); QAbstractButton::enterEvent(e); }
void MikanTile::leaveEvent(QEvent *e) { m_hover = false; update(); QAbstractButton::leaveEvent(e); }
void MikanTile::mousePressEvent(QMouseEvent *e) { if (e->button() == Qt::LeftButton) animatePress(1, 90); QAbstractButton::mousePressEvent(e); }
void MikanTile::mouseReleaseEvent(QMouseEvent *e) { animatePress(0, 260); QAbstractButton::mouseReleaseEvent(e); }

static QColor menuFill() { return Mikan::light() ? QColor(250, 250, 252) : QColor(12, 12, 14); }

void MikanTile::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect();
    if (m_filled) {
        // The plate is rounded, and an unpainted corner on Windows is black.
        p.setPen(Qt::NoPen);
        p.setBrush(menuFill());
        p.drawRect(rect());
    }
    if (!isEnabled()) p.setOpacity(.4);
    const bool pill = m_kind == Icon || m_kind == Chip || (m_kind == Text && !m_icon.isEmpty());
    const qreal radius = pill ? r.height() / 2 : m_kind == Tab ? 16 : 20;
    if (m_filled) {
        // Opaque: a tinted glass plate composites through a translucent popup and the label disappears.
        p.setPen(QPen(Mikan::light() ? QColor(0, 0, 0, 28) : QColor(255, 255, 255, 32), 1));
        p.setBrush(m_selected ? Mikan::accent().plate
                              : (Mikan::light() ? QColor(255, 255, 255) : QColor(32, 32, 40)));
        const qreal corner = std::min(radius, r.height() / 2);
        p.drawRoundedRect(r.adjusted(.5, .5, -.5, -.5), corner, corner);
    } else if (!m_flat) {
        Mikan::paintGlass(p, r, radius, m_selected ? Mikan::accent().glow : QColor());
    }
    if (m_flat && m_kind == Tab && m_selected) {
        p.setPen(Qt::NoPen);
        p.setBrush(Mikan::accent().wash);
        const QRectF pillRect = r.adjusted(0, 6, 0, -6);
        p.drawRoundedRect(pillRect, pillRect.height() / 2, pillRect.height() / 2);
    }
    const int lift = (m_hover && isEnabled() ? 12 : 0) + qRound(m_press * 40);
    if (lift > 0 || hasFocus()) {
        p.setPen(hasFocus() && !m_hover ? QPen(Mikan::text(), 1.5) : Qt::NoPen);
        p.setBrush(Mikan::overlay(lift));
        const qreal c = std::min(radius, r.height() / 2);
        if (m_flat && m_kind == Icon) p.drawEllipse(r.center(), std::min(r.width(), r.height()) / 2 - 2, std::min(r.width(), r.height()) / 2 - 2);
        else p.drawRoundedRect(r.adjusted(.5, .5, -.5, -.5), c, c);
    }
    const QColor ink = Mikan::text();
    switch (m_kind) {
    case Icon: {
        const qreal size = m_flat ? 22 : 24;
        QRectF box(0, 0, size, size);
        box.moveCenter(r.center());
        if (m_spin) {
            p.translate(r.center());
            p.rotate(m_angle);
            p.translate(-r.center());
        }
        Mikan::paintIcon(p, m_icon, box, ink);
        break;
    }
    case Chip: {
        QString name = text();
        const QString leading = Mikan::leadingFlag(name);
        qreal left = 16;
        if (!leading.isEmpty()) {
            name = name.mid(leading.size()).trimmed();
            const QRectF badge(12, r.center().y() - 8, 22, 16);
            const QPixmap art = Mikan::flag(leading, 16, p.device()->devicePixelRatioF());
            if (!art.isNull()) {
                p.save();
                QPainterPath clip;
                clip.addRoundedRect(badge, 3, 3);
                p.setClipPath(clip);
                p.drawPixmap(badge, art, QRectF(art.rect()));
                p.restore();
                left = badge.right() + 8;
            }
        }
        p.setFont(Mikan::font(m_textSize, QFont::Bold));
        p.setPen(ink);
        const QRectF textBox(left, r.top(), r.right() - 36 - left, r.height());
        p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(name, Qt::ElideRight, int(std::max(0.0, textBox.width()))));
        p.save();
        const QRectF arrow(r.right() - 12 - 22, r.center().y() - 11, 22, 22);
        p.translate(arrow.center());
        p.rotate(90);
        p.translate(-arrow.center());
        Mikan::paintIcon(p, QStringLiteral("chevron"), arrow, ink);
        p.restore();
        break;
    }
    case Tab:
    case Text: {
        if (m_kind == Text && !m_icon.isEmpty() && m_subtitle.isEmpty()) {
            Mikan::paintIcon(p, m_icon, QRectF(r.left() + 12, r.center().y() - 8, 16, 16), ink);
            p.setFont(Mikan::font(m_textSize, QFont::DemiBold));
            p.setPen(ink);
            p.drawText(r.adjusted(32, 0, -26, 0), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(text(), Qt::ElideRight, int(std::max(0.0, r.width() - 58))));
            Mikan::paintIcon(p, QStringLiteral("chevron"), QRectF(r.right() - 24, r.center().y() - 8, 16, 16), Mikan::muted());
            break;
        }
        if (!m_subtitle.isEmpty()) {
            QString name = text();
            const QString leading = Mikan::leadingFlag(name);
            qreal left = 16;
            if (!leading.isEmpty()) {
                name = name.mid(leading.size()).trimmed();
                const QRectF badge(14, r.center().y() - 11, 28, 22);
                const QPixmap art = Mikan::flag(leading, 22, p.device()->devicePixelRatioF());
                if (!art.isNull()) {
                    p.save();
                    QPainterPath clip;
                    clip.addRoundedRect(badge, 4, 4);
                    p.setClipPath(clip);
                    p.drawPixmap(badge, art, QRectF(art.rect()));
                    p.restore();
                    left = badge.right() + 10;
                }
            }
            const qreal rightPad = m_expandable ? 36 : 14;
            const qreal textWidth = std::max(0.0, r.width() - left - rightPad);
            p.setFont(Mikan::font(15, QFont::Bold));
            p.setPen(ink);
            p.drawText(QRectF(left, r.top() + 6, textWidth, r.height() / 2 - 4), Qt::AlignLeft | Qt::AlignBottom,
                       p.fontMetrics().elidedText(name, Qt::ElideRight, int(textWidth)));
            p.setFont(Mikan::font(12));
            p.setPen(Mikan::muted());
            p.drawText(QRectF(left, r.center().y() + 1, textWidth, r.height() / 2 - 8), Qt::AlignLeft | Qt::AlignTop,
                       p.fontMetrics().elidedText(m_subtitle, Qt::ElideRight, int(textWidth)));
            if (m_expandable) {
                p.save();
                const QRectF arrow(r.right() - 28, r.center().y() - 8, 16, 16);
                if (m_expanded) {
                    p.translate(arrow.center());
                    p.rotate(90);
                    p.translate(-arrow.center());
                }
                Mikan::paintIcon(p, QStringLiteral("chevron"), arrow, Mikan::muted());
                p.restore();
            }
            break;
        }
        if (m_alignLeft) {
            qreal right = m_chevron || m_mark ? 32 : 14;
            if (!m_trailing.isEmpty()) {
                p.setFont(Mikan::font(12));
                p.setPen(Mikan::muted());
                const int tw = p.fontMetrics().horizontalAdvance(m_trailing);
                p.drawText(QRectF(r.right() - right - tw, r.top(), tw, r.height()), Qt::AlignVCenter | Qt::AlignRight, m_trailing);
                right += tw + 12;
            }
            p.setFont(Mikan::font(m_textSize, QFont::DemiBold));
            p.setPen(ink);
            p.drawText(r.adjusted(16, 0, -right, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(text(), Qt::ElideRight, int(std::max(0.0, r.width() - 16 - right))));
            if (m_chevron) {
                Mikan::paintIcon(p, QStringLiteral("chevron"), QRectF(r.right() - 28, r.center().y() - 8, 16, 16), Mikan::muted());
            } else if (m_mark) {
                const qreal s = 14;
                const qreal x = r.right() - 26, y = r.center().y() - s / 2;
                p.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPolyline(QPolygonF{QPointF(x + s * .18, y + s * .54), QPointF(x + s * .4, y + s * .76), QPointF(x + s * .84, y + s * .26)});
            }
            break;
        }
        p.setFont(Mikan::font(m_textSize, m_kind == Tab && m_selected ? QFont::Medium : QFont::Normal));
        p.setPen(m_kind == Tab && !m_selected ? Mikan::muted() : ink);
        if (m_kind == Tab && m_hover) p.setPen(ink);
        p.drawText(r.adjusted(10, 0, -10, 0), Qt::AlignCenter, p.fontMetrics().elidedText(text(), Qt::ElideRight, int(r.width() - 20)));
        break;
    }
    }
}

MikanSwitch::MikanSwitch(const QString &label, QWidget *parent) : QAbstractButton(parent) {
    clearPlate(this);
    setText(label);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setMinimumHeight(42);
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(180);
    m_anim->setEasingCurve(softCurve());
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { m_knob = v.toReal(); update(); });
}

void MikanSwitch::setDescription(const QString &description) {
    m_description = description;
    setMinimumHeight(description.isEmpty() ? 42 : 64);
    updateGeometry();
    update();
}

void MikanSwitch::setOn(bool on) {
    if (on == m_on) return;
    m_on = on;
    m_anim->stop();
    m_anim->setStartValue(m_knob);
    m_anim->setEndValue(on ? 1.0 : 0.0);
    if (isVisible()) m_anim->start();
    else { m_knob = on ? 1 : 0; update(); }
}

QSize MikanSwitch::sizeHint() const { return m_description.isEmpty() ? QSize(240, 42) : QSize(280, 64); }
void MikanSwitch::enterEvent(QEnterEvent *e) { m_hover = true; update(); QAbstractButton::enterEvent(e); }
void MikanSwitch::leaveEvent(QEvent *e) { m_hover = false; update(); QAbstractButton::leaveEvent(e); }

void MikanSwitch::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) p.setOpacity(.45);
    const QRectF r = rect();
    if (!m_bare) Mikan::paintGlass(p, r, 20);
    if (m_hover || isDown() || hasFocus()) {
        p.setPen(hasFocus() && !m_hover ? QPen(Mikan::accent().glow, 1.5) : Qt::NoPen);
        p.setBrush(Mikan::overlay(isDown() ? 36 : 10));
        p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 16, 16);
    }
    const QRectF track(r.right() - 16 - 46, r.center().y() - 12, 46, 24);
    const QRectF stateBox(track.left() - 8 - 42, track.top(), 42, track.height());
    qreal textLeft = 14;
    if (!m_icon.isEmpty()) {
        const QRectF bubble(12, r.center().y() - 14, 28, 28);
        p.setPen(Qt::NoPen);
        p.setBrush(m_on ? Mikan::accent().bubble
                        : (Mikan::light() ? QColor(236, 234, 242) : QColor(44, 44, 52)));
        p.drawEllipse(bubble);
        Mikan::paintIcon(p, m_icon, bubble.adjusted(6, 6, -6, -6), m_on ? Mikan::accent().mark : Mikan::muted());
        textLeft = bubble.right() + 10;
    }
    const qreal textRight = stateBox.left() - 8;
    const qreal textWidth = std::max(0.0, textRight - textLeft);
    if (m_description.isEmpty()) {
        p.setFont(Mikan::font(14, QFont::DemiBold));
        p.setPen(Mikan::text());
        p.drawText(QRectF(textLeft, 0, textWidth, r.height()), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(text(), Qt::ElideRight, int(textWidth)));
    } else {
        p.setFont(Mikan::font(14, QFont::DemiBold));
        p.setPen(m_on ? Mikan::text() : Mikan::muted());
        p.drawText(QRectF(textLeft, r.top() + 8, textWidth, 22), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(text(), Qt::ElideRight, int(textWidth)));
        p.setFont(Mikan::font(12));
        p.setPen(Mikan::muted());
        p.drawText(QRectF(textLeft, r.top() + 30, textWidth, 20), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(m_description, Qt::ElideRight, int(textWidth)));
    }
    p.setFont(Mikan::font(12, QFont::DemiBold));
    p.setPen(m_on ? Mikan::accent().text : Mikan::muted());
    p.drawText(stateBox, Qt::AlignRight | Qt::AlignVCenter, m_on ? T("On") : T("Off"));
    const QColor on = Mikan::accent().mark;
    const QColor off = Mikan::light() ? QColor(226, 224, 236) : QColor(68, 68, 80);
    const auto mix = [this](int a, int b) { return int(std::round(a + (b - a) * m_knob)); };
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(mix(off.red(), on.red()), mix(off.green(), on.green()), mix(off.blue(), on.blue())));
    p.drawRoundedRect(track, 12, 12);
    const qreal knob = 18;
    const QPointF centre(track.left() + 3 + knob / 2 + (track.width() - 6 - knob) * m_knob, track.center().y());
    p.setBrush(QColor(255, 255, 255));
    p.drawEllipse(centre, knob / 2, knob / 2);
}

// ---------------------------------------------------------------- the connect button

MikanOrb::MikanOrb(QWidget *parent) : QAbstractButton(parent) {
    clearPlate(this);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setFixedSize(92, 92);
    m_clock.start();
    m_frame = new QTimer(this);
    m_frame->setTimerType(Qt::PreciseTimer);
    connect(m_frame, &QTimer::timeout, this, [this] {
        const QWidget *top = window();
        if (!isVisible() || !top || top->isMinimized() || (!m_connecting && !m_connected)) {
            m_frame->stop();
            m_lastFrame = 0;
            return;
        }
        const qint64 now = m_clock.elapsed();
        if (m_lastFrame == 0 || now - m_lastFrame > 80) {
            m_lastFrame = now;
            update();
            return;
        }
        const double dt = double(now - m_lastFrame);
        m_lastFrame = now;
        // Same average speed as the old fixed step, eased around the circle so it doesn't tick.
        if (m_connecting) {
            const double wobble = 1.0 + 0.35 * std::sin(m_angle * 0.017453292519943295);
            m_angle += (360.0 / 1.15) * (dt / 1000.0) * wobble;
            if (m_angle >= 360.0) m_angle = std::fmod(m_angle, 360.0);
        }
        if (m_connected && !m_connecting) {
            m_breath += dt / 3000.0;
            if (m_breath >= 1.0) m_breath -= std::floor(m_breath);
        }
        update();
    });
}

void MikanOrb::setState(bool connected, bool connecting, const QString &status, const QString &elapsed) {
    const bool wasConnecting = m_connecting;
    m_connected = connected;
    m_connecting = connecting;
    m_status = status;
    m_elapsed = elapsed;
    m_wave = connected ? 1 : connecting ? .6 : .3;
    m_lit = connected || connecting ? 1 : 0;
    m_spin = connecting ? 1 : 0;
    if (connecting && !wasConnecting) swellTo(1.04);
    else if (!connecting && wasConnecting) swellTo(1);
    syncMotion();
    update();
}

void MikanOrb::swellTo(qreal to) {
    if (qAbs(m_swell - to) < 0.001 && !(m_swellAnim && m_swellAnim->state() == QAbstractAnimation::Running)) return;
    if (!m_swellAnim) {
        m_swellAnim = new QVariantAnimation(this);
        connect(m_swellAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_swell = v.toReal();
            update();
        });
    }
    m_swellAnim->stop();
    m_swellAnim->setDuration(to > m_swell ? 480 : 320);
    m_swellAnim->setStartValue(m_swell);
    m_swellAnim->setEndValue(to);
    m_swellAnim->setEasingCurve(to > m_swell ? QEasingCurve::OutBack : QEasingCurve::OutCubic);
    m_swellAnim->start();
}

void MikanOrb::syncMotion() {
    const QWidget *top = window();
    const bool live = (m_connecting || m_connected) && isVisible() && top && !top->isMinimized();
    if (live) {
        if (!m_frame->isActive()) m_frame->start(16);
    } else if (m_frame->isActive()) {
        m_frame->stop();
        m_lastFrame = 0;
    }
}

void MikanOrb::showEvent(QShowEvent *e) {
    QAbstractButton::showEvent(e);
    syncMotion();
}
void MikanOrb::hideEvent(QHideEvent *e) {
    m_frame->stop();
    m_lastFrame = 0;
    QAbstractButton::hideEvent(e);
}

bool MikanOrb::hitButton(const QPoint &pos) const {
    const QPointF centre(width() / 2.0, height() / 2.0);
    return QLineF(QPointF(pos), centre).length() <= std::min(width(), height()) / 2.0 - 4;
}

void MikanOrb::animatePress(qreal to, int duration) {
    if (!m_pressAnim) {
        m_pressAnim = new QVariantAnimation(this);
        connect(m_pressAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { m_pressScale = v.toReal(); update(); });
    }
    m_pressAnim->stop();
    m_pressAnim->setDuration(duration);
    m_pressAnim->setStartValue(m_pressScale);
    m_pressAnim->setEndValue(to);
    m_pressAnim->setEasingCurve(QEasingCurve::OutQuad);
    m_pressAnim->start();
}

void MikanOrb::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && hitButton(e->pos())) animatePress(.92, 90);
    QAbstractButton::mousePressEvent(e);
}

void MikanOrb::mouseReleaseEvent(QMouseEvent *e) {
    animatePress(1, 150);
    QAbstractButton::mouseReleaseEvent(e);
}

void MikanOrb::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) p.setOpacity(.55);
    const QRectF bounds = QRectF(rect()).adjusted(5, 5, -5, -5);
    const qreal base = std::min(bounds.width(), bounds.height()) * m_pressScale;
    const qreal side = base * m_swell;
    QRectF disc(0, 0, side, side);
    disc.moveCenter(bounds.center());
    const QPointF c = disc.center();
    const bool lit = m_connected || m_connecting;
    const Mikan::Accent tone = Mikan::accent();
    const QColor top = lit ? tone.shine : tone.mark;
    const QColor bottom = tone.shade;
    // The glyph sits on the middle of the disc, so a pale accent (amber, rose) gets a dark mark.
    const int midL = (top.lightness() + bottom.lightness()) / 2;
    const QColor ink = midL > 145 ? QColor(32, 28, 48) : QColor(255, 255, 255);
    if (lit) {
        int glowAlpha = 62;
        if (m_connected && !m_connecting) {
            const double breath = 0.5 + 0.5 * std::sin(m_breath * 6.283185307179586);
            glowAlpha = int(std::lround(40 + 30 * breath));
        }
        QRadialGradient glow(c, side * .72);
        glow.setColorAt(0, QColor(tone.glow.red(), tone.glow.green(), tone.glow.blue(), glowAlpha));
        glow.setColorAt(1, QColor(tone.glow.red(), tone.glow.green(), tone.glow.blue(), 0));
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(c, side * .72, side * .72);
    }
    QLinearGradient fill(disc.topLeft(), disc.bottomRight());
    fill.setColorAt(0, top);
    fill.setColorAt(1, bottom);
    p.setPen(QPen(QColor(ink.red(), ink.green(), ink.blue(), midL > 145 ? 50 : 90), 1));
    p.setBrush(fill);
    p.drawEllipse(disc);
    if (m_connecting) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(ink.red(), ink.green(), ink.blue(), 230), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(disc.adjusted(-3, -3, 3, 3), int(-m_angle * 16), 100 * 16);
    }
    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(tone.glow, 1.5));
        p.drawEllipse(disc.adjusted(-3, -3, 3, 3));
    }
    const QColor glyph = ink;
    const qreal g = base * .2;
    p.setPen(QPen(glyph, std::max(2.0, base * .055), Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(c.x() - g, c.y() - g * .85, g * 2, g * 2), 50 * 16, -290 * 16);
    p.drawLine(QPointF(c.x(), c.y() - g * 1.45), QPointF(c.x(), c.y() - g * .15));
}

// ---------------------------------------------------------------- window buttons

namespace {
    class WindowButton : public QAbstractButton {
    public:
        enum Kind { Minimize, Maximize, Close };
        WindowButton(Kind kind, QWidget *parent) : QAbstractButton(parent), m_kind(kind) {
            setFixedSize(40, 28);
            setAttribute(Qt::WA_Hover);
            setFocusPolicy(Qt::NoFocus);
        }
    protected:
        void enterEvent(QEnterEvent *e) override { m_hover = true; update(); QAbstractButton::enterEvent(e); }
        void leaveEvent(QEvent *e) override { m_hover = false; update(); QAbstractButton::leaveEvent(e); }
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            const QRectF r = rect();
            if (m_hover || isDown()) Mikan::paintGlass(p, r.adjusted(2, 1, -2, -1), 14, m_kind == Close ? QColor(220, 66, 61) : QColor());
            if (isDown()) { p.setPen(Qt::NoPen); p.setBrush(Mikan::overlay(30)); p.drawRoundedRect(r.adjusted(2, 1, -2, -1), 13, 13); }
            const QColor ink = m_kind == Close && m_hover ? QColor(235, 90, 85) : Mikan::text();
            p.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            const QPointF c = r.center();
            switch (m_kind) {
            case Minimize: p.drawLine(QPointF(c.x() - 5, c.y()), QPointF(c.x() + 5, c.y())); break;
            case Maximize:
                if (window()->isMaximized()) {
                    p.drawRoundedRect(QRectF(c.x() - 5, c.y() - 3, 8, 8), 2, 2);
                    p.drawPolyline(QPolygonF{QPointF(c.x() - 3, c.y() - 5), QPointF(c.x() + 5, c.y() - 5), QPointF(c.x() + 5, c.y() + 3)});
                } else {
                    p.drawRoundedRect(QRectF(c.x() - 5, c.y() - 5, 10, 10), 2.5, 2.5);
                }
                break;
            case Close:
                p.drawLine(QPointF(c.x() - 5, c.y() - 5), QPointF(c.x() + 5, c.y() + 5));
                p.drawLine(QPointF(c.x() + 5, c.y() - 5), QPointF(c.x() - 5, c.y() + 5));
                break;
            }
        }
    private:
        Kind m_kind;
        bool m_hover = false;
    };

    // The sidebar's own plate. Glass alone is too thin in the dark theme, so the panel paints the
    // same backdrop as the window and a soft edge while it slides over the dashboard.
    class SidePanel : public QWidget {
    public:
        explicit SidePanel(QWidget *parent = nullptr) : QWidget(parent) { clearPlate(this); }
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            Mikan::paintBackdrop(p, rect());
            QLinearGradient edge(0, 0, 22, 0);
            edge.setColorAt(0, QColor(0, 0, 0, Mikan::light() ? 36 : 110));
            edge.setColorAt(1, QColor(0, 0, 0, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(edge);
            p.drawRect(QRect(0, 0, 22, height()));
        }
    };

    // The window behind a rounded plate. Unpainted corner pixels are black on Windows, so the plate
    // fills them with the same backdrop the window paints before drawing its glass.
    void paintBackdropSlice(QPainter &p, const QWidget *widget) {
        const QWidget *top = widget->window();
        if (!top) return;
        const QPoint origin = widget->mapTo(top, QPoint(0, 0));
        p.save();
        p.translate(-origin);
        Mikan::paintBackdrop(p, QRect(QPoint(0, 0), top->size()));
        p.restore();
    }

    // A glass card: the notice above the list and the log, connections and traffic pages.
    class GlassCard : public QWidget {
    public:
        explicit GlassCard(qreal radius, QWidget *parent = nullptr) : QWidget(parent), m_radius(radius) { clearPlate(this); }
    protected:
        // A bar drawn on another card. Its square corners must continue that card, not the window gradient.
        GlassCard *plateBehind() const {
            for (QWidget *widget = parentWidget(); widget; widget = widget->parentWidget())
                if (auto *card = dynamic_cast<GlassCard *>(widget)) return card;
            return nullptr;
        }
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            paintBackdropSlice(p, this);
            if (GlassCard *plate = plateBehind()) {
                const QPoint origin = mapTo(plate, QPoint(0, 0));
                p.save();
                p.translate(-origin);
                Mikan::paintGlass(p, QRectF(plate->rect()), plate->m_radius);
                p.restore();
            }
            Mikan::paintGlass(p, rect(), m_radius);
        }
    private:
        qreal m_radius;
    };

    // The subscriptions sheet: switch between them, add one or manage them.
    class GlassPopup : public QWidget {
    public:
        explicit GlassPopup(QWidget *parent) : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
            // A translucent popup leaves its rounded corners black. The fill is opaque and the window is clipped.
            setAttribute(Qt::WA_TranslucentBackground, false);
            setAttribute(Qt::WA_OpaquePaintEvent);
            setAttribute(Qt::WA_DeleteOnClose);
            QPalette pal = palette();
            pal.setColor(QPalette::Window, menuFill());
            pal.setColor(QPalette::Base, menuFill());
            setPalette(pal);
            setAutoFillBackground(true);
        }
        void showAt(const QPoint &pos) {
            move(pos + QPoint(0, 10));
            show();
            auto *anim = new QVariantAnimation(this);
            anim->setDuration(220);
            anim->setStartValue(0.0);
            anim->setEndValue(1.0);
            anim->setEasingCurve(softCurve());
            connect(anim, &QVariantAnimation::valueChanged, this, [this, pos](const QVariant &v) {
                move(pos + QPoint(0, qRound(10 * (1 - v.toReal()))));
            });
            anim->start(QAbstractAnimation::DeleteWhenStopped);
        }
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            const QColor fill = menuFill();
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRect(rect());
            p.setPen(QPen(Mikan::outline(), 1));
            p.setBrush(fill);
            p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 28, 28);
        }
        void resizeEvent(QResizeEvent *event) override {
            QWidget::resizeEvent(event);
            // setMask on every animated size change flashes black on Windows. While the window is moving,
            // leave it a filled rectangle and clip the corners once it settles.
            if (m_deferMask) {
                if (!m_maskCleared) {
                    clearMask();
                    setProperty("mikanPopupMask", QSize());
                    m_maskCleared = true;
                }
                return;
            }
            m_maskCleared = false;
            updatePopupMask();
        }
    protected:
        void setDeferMask(bool defer) {
            m_deferMask = defer;
            if (defer) {
                if (!m_maskCleared) {
                    clearMask();
                    setProperty("mikanPopupMask", QSize());
                    m_maskCleared = true;
                }
                return;
            }
            m_maskCleared = false;
            updatePopupMask();
        }
        void updatePopupMask() {
            if (width() < 4 || height() < 4) return;
            if (property("mikanPopupMask").toSize() == size() && !mask().isEmpty()) return;
            setProperty("mikanPopupMask", size());
            QPainterPath path;
            path.addRoundedRect(QRectF(rect()), 28, 28);
            setMask(QRegion(path.toFillPolygon().toPolygon()));
        }
    private:
        bool m_deferMask = false;
        bool m_maskCleared = false;
    };

    QString menuLabel(QString text) {
        text.replace(QStringLiteral("&&"), QString(QChar(1)));
        text.remove(QLatin1Char('&'));
        text.replace(QChar(1), QLatin1Char('&'));
        return text.trimmed();
    }

    // A menu drawn with the same plates as the dashboard. Submenus replace this sheet: the new page slides in and the window eases to the new height.
    // Arrows move, Enter runs, Right opens, Left returns, Escape steps back and then closes. Letters jump to a row.
    class ActionSheet : public GlassPopup {
    public:
        ActionSheet(QWidget *anchor, QMenu *menu, bool ownsMenu, bool alignRight, const QPoint &globalPos, bool hasPos, const QString &hint)
            : GlassPopup(anchor ? anchor->window() : nullptr), m_anchor(anchor), m_alignRight(alignRight), m_pos(globalPos), m_hasPos(hasPos), m_hint(hint) {
            setFocusPolicy(Qt::StrongFocus);
            auto *layout = new QVBoxLayout(this);
            layout->setContentsMargins(12, 12, 12, 12);
            layout->setSpacing(0);
            m_clip = new QWidget;
            m_clip->setAutoFillBackground(true);
            m_clip->setPalette(palette());
            m_clip->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
            m_clip->setMinimumSize(0, 0);
            layout->addWidget(m_clip);
            if (ownsMenu) connect(this, &QObject::destroyed, menu, &QObject::deleteLater);
            m_stack.append(menu);
            rebuild(0);
        }
        ~ActionSheet() override { dismissMenus(); }

        void push(QMenu *menu) {
            m_stack.append(menu);
            rebuild(1);
        }
        void pop() {
            if (m_stack.size() < 2 || m_closing) return;
            QMenu *leaving = m_stack.takeLast();
            m_shown.removeAll(leaving);
            if (leaving) leaving->aboutToHide();
            if (m_closing) return;
            rebuild(-1);
        }

        // Folded, so the first painted frame is the start of the open instead of the full sheet.
        void foldForOpen() {
            m_fullSize = size();
            const int startH = std::min(m_fullSize.height(), m_hint.isEmpty() ? 84 : 118);
            const int y = m_openUp ? m_edgeY - startH + 10 : m_topLeft.y() + 10;
            setFixedSize(m_fullSize.width(), startH);
            const QPoint start(m_topLeft.x(), y);
            move(start);
            m_shownAt = start;
            if (QLayout *box = layout()) box->activate();
            clipPages();
        }

        void playOpen() {
            setDeferMask(true);
            const QSize full = m_fullSize.isValid() ? m_fullSize : size();
            const QPoint end = m_topLeft;
            const QSize from = size();
            const QPoint start = m_shownAt;
            m_openAnim = new QVariantAnimation(this);
            m_openAnim->setDuration(240);
            m_openAnim->setStartValue(0.0);
            m_openAnim->setEndValue(1.0);
            m_openAnim->setEasingCurve(softCurve());
            connect(m_openAnim, &QVariantAnimation::valueChanged, this, [this, from, full, start, end](const QVariant &v) {
                if (!m_openAnim) return;
                const qreal t = v.toReal();
                applyFrame(QSize(full.width(), mixPx(from.height(), full.height(), t)), QPoint(mixPx(start.x(), end.x(), t), mixPx(start.y(), end.y(), t)));
            });
            connect(m_openAnim, &QVariantAnimation::finished, this, [this, full, end] {
                if (!m_openAnim) return;
                m_openAnim = nullptr;
                applyFrame(full, end);
                setDeferMask(false);
            });
            m_openAnim->start();
        }

    protected:
        void showEvent(QShowEvent *event) override {
            QWidget::showEvent(event);
            // show() can put a new popup back on the primary display. Place it again once the window exists.
            const QPoint probe = m_anchor ? m_anchor->mapToGlobal(m_anchor->rect().center()) : (m_hasPos ? m_pos : pos());
            bindPopupToScreen(this, probe);
            move(m_shownAt);
            if (QLayout *box = layout()) box->activate();
            clipPages();
            focusInitial();
        }
        void hideEvent(QHideEvent *event) override {
            // Menus were never shown as windows, so their own hide signal would not run.
            dismissMenus();
            GlassPopup::hideEvent(event);
        }
        bool event(QEvent *event) override {
            if (event->type() == QEvent::ShortcutOverride && claimsMenuKey(static_cast<QKeyEvent *>(event))) {
                event->accept();
                return true;
            }
            return GlassPopup::event(event);
        }
        void keyPressEvent(QKeyEvent *event) override {
            if (!handleMenuKey(event)) GlassPopup::keyPressEvent(event);
        }
        bool eventFilter(QObject *watched, QEvent *event) override {
            if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) &&
                claimsMenuKey(static_cast<QKeyEvent *>(event))) {
                if (event->type() == QEvent::ShortcutOverride) event->accept();
                else handleMenuKey(static_cast<QKeyEvent *>(event));
                return true;
            }
            if (event->type() == QEvent::Enter) {
                // The filter is installed only on menu rows.
                auto *tile = static_cast<MikanTile *>(watched);
                if (tile->isEnabled()) tile->setFocus(Qt::MouseFocusReason);
            }
            return false;
        }

    private:
        void clipPages() {
            if (!m_clip) return;
            const QMargins margins = layout() ? layout()->contentsMargins() : QMargins();
            const int w = std::max(0, width() - margins.left() - margins.right());
            const int h = std::max(0, height() - margins.top() - margins.bottom());
            if (w < 2 || h < 2) return;
            m_clip->setGeometry(margins.left(), margins.top(), w, h);
        }

        void applyFrame(const QSize &sz, const QPoint &p) {
            if (m_applying) return;
            m_applying = true;
            setFixedSize(sz);
            move(p);
            m_shownAt = p;
            if (QLayout *box = layout()) box->activate();
            clipPages();
            m_applying = false;
        }
        MikanTile *addRow(QVBoxLayout *rows, const QString &text, bool checked, bool submenu, bool enabled, const QString &shortcut) {
            auto *tile = new MikanTile(MikanTile::Text);
            tile->setFilled(true);
            tile->setAlignLeft(true);
            tile->setChevron(submenu);
            tile->setMark(checked);
            tile->setSelected(checked);
            tile->setTrailing(shortcut);
            tile->setText(text);
            tile->setTextSize(14);
            tile->setFixedHeight(48);
            tile->setEnabled(enabled);
            // mikanCard's transparent stylesheet leaves the row's square corners black inside this popup.
            tile->setObjectName(QString());
            tile->setAttribute(Qt::WA_StyledBackground, false);
            tile->installEventFilter(this);
            rows->addWidget(tile);
            m_rows.append(tile);
            return tile;
        }

        void stopMotion() {
            if (m_openAnim) {
                auto *anim = m_openAnim;
                m_openAnim = nullptr;
                anim->disconnect(this);
                anim->stop();
                anim->deleteLater();
            }
            if (m_anim) {
                auto *anim = m_anim;
                m_anim = nullptr;
                anim->disconnect(this);
                anim->stop();
                anim->deleteLater();
            }
            if (m_outgoing) {
                m_outgoing->hide();
                m_outgoing->deleteLater();
                m_outgoing = nullptr;
            }
            if (m_scroll) m_scroll->move(0, 0);
        }

        void finishMotion() {
            if (!m_anim) return;
            m_anim = nullptr;
            if (m_outgoing) {
                m_outgoing->hide();
                m_outgoing->deleteLater();
                m_outgoing = nullptr;
            }
            if (m_scroll) m_scroll->setGeometry(0, 0, m_toSize.width() - 24, m_toSize.height() - 24);
            applyFrame(m_toSize, m_toPos);
            setDeferMask(false);
            m_topLeft = m_toPos;
            m_pinnedRight = m_toPos.x() + m_toSize.width();
            m_fullSize = m_toSize;
        }

        // direction: 0 installs the page, 1 slides it in from the right, -1 from the left.
        void rebuild(int direction) {
            m_query.clear();
            const bool animate = m_placed && direction != 0 && isVisible();
            if (animate) stopMotion();
            QScrollArea *previous = m_scroll;
            buildPage();
            if (!animate) {
                if (previous && previous != m_scroll) previous->deleteLater();
                m_scroll->setGeometry(0, 0, m_scroll->width(), m_scroll->height());
                m_scroll->show();
                setFixedSize(m_scroll->width() + 24, m_scroll->height() + 24);
                place(!m_placed);
                m_fullSize = size();
                m_placed = true;
                if (isVisible()) focusInitial();
                return;
            }
            setDeferMask(true);
            previous->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            m_outgoing = previous;
            if (QLayout *box = layout()) box->activate();
            const int span = m_clip->width() > 1 ? m_clip->width() : std::max(m_scroll->width(), 1);
            m_scroll->move(direction * span, 0);
            m_scroll->show();
            m_scroll->raise();
            m_fromSize = size();
            m_fromPos = m_shownAt;
            m_toSize = QSize(m_scroll->width() + 24, m_scroll->height() + 24);
            m_toPos = destinationFor(m_toSize);
            m_dir = direction;
            m_anim = new QVariantAnimation(this);
            m_anim->setDuration(260);
            m_anim->setStartValue(0.0);
            m_anim->setEndValue(1.0);
            m_anim->setEasingCurve(softCurve());
            connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
                if (!m_anim) return;
                const qreal t = v.toReal();
                applyFrame(QSize(mixPx(m_fromSize.width(), m_toSize.width(), t), mixPx(m_fromSize.height(), m_toSize.height(), t)),
                           QPoint(mixPx(m_fromPos.x(), m_toPos.x(), t), mixPx(m_fromPos.y(), m_toPos.y(), t)));
                const int travel = m_clip->width() > 1 ? m_clip->width() : 1;
                if (m_outgoing) m_outgoing->move(qRound(-m_dir * t * travel), 0);
                if (m_scroll) m_scroll->move(qRound(m_dir * (1.0 - t) * travel), 0);
                if (m_clip) m_clip->update();
            });
            connect(m_anim, &QVariantAnimation::finished, this, [this] { finishMotion(); });
            m_anim->start();
            if (isVisible()) focusInitial();
        }

        void buildPage() {
            m_rows.clear();
            auto *scroll = new QScrollArea(m_clip);
            scroll->hide();
            scroll->setFocusPolicy(Qt::NoFocus);
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setWidgetResizable(false);
            scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: %1; border: none; }").arg(menuFill().name()));
            scroll->setPalette(palette());
            scroll->setAutoFillBackground(true);
            auto *list = new QWidget;
            list->setAutoFillBackground(true);
            list->setPalette(palette());
            list->setFocusPolicy(Qt::NoFocus);
            list->setMaximumHeight(QWIDGETSIZE_MAX);
            list->setMinimumHeight(2400);
            auto *rows = new QVBoxLayout(list);
            rows->setContentsMargins(0, 0, 0, 0);
            rows->setSpacing(6);
            rows->setSizeConstraint(QLayout::SetMinimumSize);
            QMenu *current = m_stack.last();
            emit current->aboutToShow();
            if (!m_shown.contains(current)) m_shown.append(current);
            int width = 260;
            int content = 0;
            int count = 0;
            const auto account = [&](int height) {
                if (count) content += 6;
                content += height;
                ++count;
            };
            const QFontMetrics fm(Mikan::font(14, QFont::DemiBold));
            const QFontMetrics shortcutFm(Mikan::font(12));
            if (m_stack.size() == 1 && !m_hint.isEmpty()) {
                const QFont captionFont = Mikan::font(12, QFont::DemiBold);
                const QFontMetrics captionFm(captionFont);
                auto *caption = new QLabel(captionFm.elidedText(m_hint, Qt::ElideMiddle, 440));
                caption->setAutoFillBackground(true);
                caption->setPalette(palette());
                caption->setFont(captionFont);
                caption->setStyleSheet(QStringLiteral("color: %1; background: %2;").arg(Mikan::muted().name(), menuFill().name()));
                caption->setContentsMargins(6, 0, 6, 0);
                caption->setFixedHeight(22);
                rows->addWidget(caption);
                account(22);
                width = std::max(width, captionFm.horizontalAdvance(caption->text()) + 28);
            }
            if (m_stack.size() > 1) {
                const QString title = current->title().isEmpty() ? T("Back") : current->title();
                const QString backText = QStringLiteral("←  ") + menuLabel(title);
                width = std::max(width, fm.horizontalAdvance(backText) + 48);
                auto *back = addRow(rows, backText, false, false, true, {});
                back->setProperty("sheetBack", true);
                account(48);
                connect(back, &QAbstractButton::clicked, this, [this] { arm([this] { pop(); }); });
            }
            bool separated = m_stack.size() > 1;
            for (QAction *action : current->actions()) {
                if (!action || !action->isVisible()) continue;
                if (action->isSeparator()) {
                    if (separated) continue;
                    auto *line = new QWidget;
                    line->setFixedHeight(1);
                    line->setStyleSheet(QStringLiteral("background-color: rgba(140, 130, 180, 42);"));
                    auto *wrap = new QWidget;
                    wrap->setAutoFillBackground(true);
                    wrap->setPalette(palette());
                    wrap->setFixedHeight(9);
                    auto *box = new QVBoxLayout(wrap);
                    box->setContentsMargins(8, 4, 8, 4);
                    box->addWidget(line);
                    rows->addWidget(wrap);
                    account(9);
                    separated = true;
                    continue;
                }
                QMenu *sub = action->menu();
                QString label = menuLabel(action->text());
                if (label.isEmpty() && sub) label = menuLabel(sub->title());
                if (label.isEmpty()) continue;
                separated = false;
                const QString shortcut = action->shortcut().isEmpty() ? QString() : action->shortcut().toString(QKeySequence::NativeText);
                const bool mark = action->isChecked() && !sub;
                const int right = (sub || mark ? 32 : 14) + (shortcut.isEmpty() ? 0 : shortcutFm.horizontalAdvance(shortcut) + 12);
                width = std::max(width, fm.horizontalAdvance(label) + 16 + right + 8);
                auto *tile = addRow(rows, label, mark, sub != nullptr, action->isEnabled(), shortcut);
                account(48);
                if (sub) {
                    tile->setProperty("sheetSub", true);
                    connect(tile, &QAbstractButton::clicked, this, [this, sub] {
                        arm([this, sub] {
                            if (m_stack.isEmpty() || m_stack.last() == sub) return;
                            push(sub);
                        });
                    });
                } else {
                    connect(tile, &QAbstractButton::clicked, this, [this, action] { runAction(action); });
                }
            }
            const int inner = std::min(std::max(width, 260), 480);
            const int counted = std::max(content, 48);
            list->setFixedWidth(inner);
            rows->invalidate();
            rows->activate();
            const int hint = rows->sizeHint().height();
            const int laid = hint > counted && hint < counted + 80 ? hint : counted;
            const int visible = std::min(laid, 520);
            const int bar = laid > visible ? style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll) : 0;
            list->setFixedSize(inner, laid);
            rows->activate();
            scroll->setWidget(list);
            if (scroll->viewport()) {
                scroll->viewport()->setAutoFillBackground(true);
                scroll->viewport()->setPalette(palette());
                scroll->viewport()->setFocusPolicy(Qt::NoFocus);
            }
            scroll->setFixedSize(inner + bar, visible);
            scroll->setVerticalScrollBarPolicy(laid > visible ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
            m_scroll = scroll;
        }

        void dismissMenus() {
            if (m_dismissed) return;
            m_dismissed = true;
            for (int i = m_shown.size() - 1; i >= 0; --i)
                if (m_shown[i]) m_shown[i]->aboutToHide();
        }

        void runAction(QAction *action) {
            if (!action || m_closing) return;
            m_closing = true;
            hide();
            dismissMenus();
            action->trigger();
            deleteLater();
        }

        template <typename Fn>
        void arm(Fn fn) {
            if (m_navLock || m_closing) return;
            m_navLock = true;
            QTimer::singleShot(0, this, [this, fn] {
                m_navLock = false;
                if (m_closing || !isVisible()) return;
                fn();
            });
        }

        bool claimsMenuKey(const QKeyEvent *event) const {
            if (!event || (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) return false;
            switch (event->key()) {
            case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right:
            case Qt::Key_Home: case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown:
            case Qt::Key_Escape: case Qt::Key_Backspace:
            case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space:
                return true;
            default:
                return event->text().size() == 1 && event->text().at(0).isPrint();
            }
        }

        MikanTile *focusedRow() const {
            for (const auto &tile : m_rows)
                if (tile && tile->hasFocus()) return tile;
            return nullptr;
        }

        int rowIndex(MikanTile *tile) const {
            for (int i = 0; i < m_rows.size(); ++i)
                if (m_rows[i] == tile) return i;
            return -1;
        }

        void focusRow(int index) {
            if (index < 0 || index >= m_rows.size() || !m_rows[index]) return;
            m_rows[index]->setFocus(Qt::OtherFocusReason);
            if (m_scroll) m_scroll->ensureWidgetVisible(m_rows[index], 0, 6);
        }

        void focusInitial() {
            for (int i = 0; i < m_rows.size(); ++i) {
                if (!m_rows[i] || !m_rows[i]->isEnabled() || m_rows[i]->property("sheetBack").toBool()) continue;
                focusRow(i);
                return;
            }
            for (int i = 0; i < m_rows.size(); ++i)
                if (m_rows[i] && m_rows[i]->isEnabled()) { focusRow(i); return; }
        }

        void moveRow(int key) {
            QList<int> enabled;
            for (int i = 0; i < m_rows.size(); ++i)
                if (m_rows[i] && m_rows[i]->isEnabled()) enabled.append(i);
            if (enabled.isEmpty()) return;
            const int last = int(enabled.size()) - 1;
            int pos = int(enabled.indexOf(rowIndex(focusedRow())));
            if (key == Qt::Key_Home || (pos < 0 && key != Qt::Key_End)) pos = 0;
            else if (key == Qt::Key_End) pos = last;
            else if (key == Qt::Key_Up) pos = pos <= 0 ? 0 : pos - 1;
            else if (key == Qt::Key_Down) pos = pos < 0 ? 0 : std::min(pos + 1, last);
            else {
                const int stride = std::max(1, (m_scroll ? m_scroll->viewport()->height() : 320) / 54);
                if (pos < 0) pos = 0;
                pos = std::clamp(pos + (key == Qt::Key_PageDown ? stride : -stride), 0, last);
            }
            focusRow(enabled[pos]);
        }

        void typeAhead(const QString &ch) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_queryAt > 700) m_query.clear();
            m_queryAt = now;
            const int current = rowIndex(focusedRow());
            const auto find = [&](const QString &query, bool afterCurrent) {
                for (int pass = 0; pass < (afterCurrent ? 2 : 1); ++pass) {
                    for (int i = 0; i < m_rows.size(); ++i) {
                        if (afterCurrent && pass == 0 && i <= current) continue;
                        if (afterCurrent && pass == 1 && i > current) continue;
                        const auto &tile = m_rows[i];
                        if (!tile || !tile->isEnabled() || tile->property("sheetBack").toBool()) continue;
                        if (tile->text().startsWith(query, Qt::CaseInsensitive)) return i;
                    }
                }
                return -1;
            };
            if (const int grown = find(m_query + ch, false); grown >= 0) {
                m_query += ch;
                focusRow(grown);
                return;
            }
            m_query = ch;
            if (const int next = find(ch, true); next >= 0) focusRow(next);
        }

        bool handleMenuKey(QKeyEvent *event) {
            if (!claimsMenuKey(event) || m_closing) return false;
            const int key = event->key();
            if (key == Qt::Key_Escape) {
                if (m_stack.size() > 1) arm([this] { pop(); });
                else if (!m_closing) {
                    m_closing = true;
                    QTimer::singleShot(0, this, [this] { close(); });
                }
                return true;
            }
            if (key == Qt::Key_Left || key == Qt::Key_Backspace) {
                if (m_stack.size() > 1) arm([this] { pop(); });
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space || key == Qt::Key_Right) {
                MikanTile *tile = focusedRow();
                if (tile && (key != Qt::Key_Right || tile->property("sheetSub").toBool()))
                    arm([this, tile = QPointer<MikanTile>(tile)] { if (tile) tile->click(); });
                return true;
            }
            if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Home || key == Qt::Key_End ||
                key == Qt::Key_PageUp || key == Qt::Key_PageDown) {
                moveRow(key);
                return true;
            }
            typeAhead(event->text());
            return true;
        }

        QRect availRect() const {
            QPoint probe;
            if (m_anchor) probe = m_anchor->mapToGlobal(m_anchor->rect().center());
            else if (m_hasPos) probe = m_pos;
            else probe = pos();
            return screenAvailableAt(probe);
        }

        QPoint destinationFor(const QSize &size) const {
            const QRect avail = availRect();
            QPoint origin(m_alignRight ? m_pinnedRight - size.width() : m_topLeft.x(), m_openUp ? m_edgeY - size.height() : m_edgeY);
            if (origin.x() + size.width() > avail.right() - 8) origin.setX(avail.right() - size.width() - 8);
            if (origin.x() < avail.left() + 8) origin.setX(avail.left() + 8);
            if (origin.y() + size.height() > avail.bottom() - 8) origin.setY(std::max(avail.top() + 8, avail.bottom() - size.height() - 8));
            if (origin.y() < avail.top() + 8) origin.setY(avail.top() + 8);
            return origin;
        }

        void place(bool first) {
            const QRect avail = availRect();
            QPoint origin;
            if (first) {
                if (m_hasPos) origin = m_pos;
                else if (m_anchor && m_alignRight) origin = m_anchor->mapToGlobal(QPoint(m_anchor->width() - width(), m_anchor->height() + 8));
                else if (m_anchor) origin = m_anchor->mapToGlobal(QPoint(0, m_anchor->height() + 8));
                m_edgeY = origin.y();
                m_openUp = false;
                if (origin.y() + height() > avail.bottom() - 8) {
                    const int above = m_hasPos || !m_anchor ? origin.y() - height() - 8 : m_anchor->mapToGlobal(QPoint(0, 0)).y() - height() - 8;
                    if (above >= avail.top() + 8) {
                        origin.setY(above);
                        m_openUp = true;
                        m_edgeY = above + height();
                    }
                }
            } else if (m_openUp) {
                origin = QPoint(m_topLeft.x(), m_edgeY - height());
            } else {
                origin = QPoint(m_topLeft.x(), m_edgeY);
            }
            if (m_alignRight && !first) origin.setX(m_pinnedRight - width());
            if (origin.x() + width() > avail.right() - 8) origin.setX(avail.right() - width() - 8);
            if (origin.x() < avail.left() + 8) origin.setX(avail.left() + 8);
            if (origin.y() + height() > avail.bottom() - 8) origin.setY(std::max(avail.top() + 8, avail.bottom() - height() - 8));
            if (origin.y() < avail.top() + 8) origin.setY(avail.top() + 8);
            move(origin);
            m_topLeft = origin;
            m_shownAt = origin;
            m_pinnedRight = origin.x() + width();
        }

        QPointer<QWidget> m_anchor;
        bool m_alignRight = false;
        bool m_placed = false;
        bool m_openUp = false;
        bool m_applying = false;
        int m_pinnedRight = 0;
        int m_edgeY = 0;
        int m_dir = 1;
        QPoint m_topLeft;
        QPoint m_shownAt;
        QPoint m_fromPos;
        QPoint m_toPos;
        QSize m_fullSize;
        QSize m_fromSize;
        QSize m_toSize;
        QPoint m_pos;
        bool m_hasPos = false;
        QList<QMenu *> m_stack;
        QList<QMenu *> m_shown;
        QList<QPointer<MikanTile>> m_rows;
        QString m_hint;
        QString m_query;
        qint64 m_queryAt = 0;
        bool m_navLock = false;
        bool m_closing = false;
        bool m_dismissed = false;
        QWidget *m_clip = nullptr;
        QScrollArea *m_scroll = nullptr;
        QPointer<QScrollArea> m_outgoing;
        QVariantAnimation *m_anim = nullptr;
        QVariantAnimation *m_openAnim = nullptr;
    };

    void openSheet(QWidget *anchor, QMenu *menu, bool ownsMenu, bool alignRight = false, QPoint pos = {}, bool hasPos = false, const QString &hint = {}) {
        auto *sheet = new ActionSheet(anchor, menu, ownsMenu, alignRight, pos, hasPos, hint);
        guardReopen(sheet, anchor);
        const QPoint probe = anchor ? anchor->mapToGlobal(anchor->rect().center()) : pos;
        bindPopupToScreen(sheet, probe);
        sheet->foldForOpen();
        sheet->show();
        sheet->playOpen();
    }

    QLabel *label(const QString &text, qreal px, bool muted, int weight = QFont::Normal) {
        auto *l = new QLabel(text);
        clearPlate(l);
        l->setFont(Mikan::font(px, weight));
        if (muted) l->setProperty("mikanRole", "muted");
        return l;
    }

    QString emdash() { return QString::fromUtf8("\xE2\x80\x94"); }

    class LogoBadge : public QWidget {
    public:
        LogoBadge() {
            clearPlate(this);
            setFixedSize(28, 28);
        }
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            QLinearGradient g(rect().topLeft(), rect().bottomRight());
            g.setColorAt(0, Mikan::accent().shine);
            g.setColorAt(1, Mikan::accent().shade);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(rect(), 8, 8);
            p.setPen(Qt::white);
            p.setFont(Mikan::font(16, QFont::Bold));
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("R"));
        }
    };

    class StatusDot : public QWidget {
    public:
        StatusDot() {
            clearPlate(this);
            setFixedSize(10, 10);
            hide();
        }
        void setOn(bool on) {
            if (m_on == on) return;
            m_on = on;
            setVisible(on);
            update();
        }
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(61, 214, 126));
            p.drawEllipse(QRectF(rect()).adjusted(.5, .5, -.5, -.5));
        }
    private:
        bool m_on = false;
    };

    class Glyph : public QWidget {
    public:
        Glyph(const QString &name, int size, QWidget *parent = nullptr) : QWidget(parent), m_name(name) {
            clearPlate(this);
            setFixedSize(size, size);
        }
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            Mikan::paintIcon(p, m_name, QRectF(rect()), Mikan::muted());
        }
    private:
        QString m_name;
    };

    // The block behind the connect button. Two shapes drift opposite each other.
    // Idle and connected take about eight seconds a cycle; connecting is a little quicker.
    // A soft ring leaves the disc about every 2.5 seconds once the link is up.
    class HeroCard : public QWidget {
    public:
        explicit HeroCard(QWidget *parent = nullptr) : QWidget(parent) {
            clearPlate(this);
            m_clock.start();
            m_timer = new QTimer(this);
            m_timer->setTimerType(Qt::PreciseTimer);
            connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
        }
        void follow(MikanOrb *orb) { m_orb = orb; }
        void setPace(int pace) {
            if (pace == 2 && m_pace != 2) m_ring = 0;
            m_pace = pace;
            sync();
        }
    protected:
        void showEvent(QShowEvent *e) override {
            QWidget::showEvent(e);
            if (!m_timer->isActive()) m_timer->start(33);
        }
        void hideEvent(QHideEvent *e) override {
            m_timer->stop();
            m_last = 0;
            QWidget::hideEvent(e);
        }
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            paintBackdropSlice(p, this);
            Mikan::paintGlass(p, rect(), 26);
            p.setRenderHint(QPainter::Antialiasing);
            const QRectF r = rect();
            QPainterPath clip;
            clip.addRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), 26, 26);
            p.setClipPath(clip);
            p.setPen(Qt::NoPen);
            const qreal drift = std::sin(m_phase * 6.283185307179586) * m_amp;
            const qreal drift2 = -drift * .85;
            const qreal lift = m_lift;
            const qreal base = r.right() - r.width() * .46;
            const qreal x0 = base + drift;
            QPainterPath wave;
            wave.moveTo(x0, r.bottom());
            wave.lineTo(x0, r.center().y() + 8 + lift);
            wave.cubicTo(r.right() - r.width() * .30 + drift * .6, r.center().y() - 36 + lift,
                         r.right() - r.width() * .12 + drift * .3, r.center().y() + 28 + lift * .4,
                         r.right() + 2, r.center().y() - 6 + lift * .15);
            wave.lineTo(r.right() + 2, r.bottom());
            wave.closeSubpath();
            QLinearGradient wash(x0, r.top(), r.right(), r.bottom());
            wash.setColorAt(0, QColor(170, 224, 206, Mikan::light() ? 80 : 36));
            wash.setColorAt(1, QColor(150, 196, 230, Mikan::light() ? 20 : 10));
            p.setBrush(wash);
            p.drawPath(wave);
            const qreal x2 = base + 36 + drift2;
            QPainterPath wave2;
            wave2.moveTo(x2, r.bottom());
            wave2.lineTo(x2, r.center().y() + 22 + lift);
            wave2.cubicTo(r.right() - r.width() * .18 + drift2 * .55, r.center().y() - 8 + lift,
                          r.right() - 20 + drift2 * .25, r.center().y() + 18 + lift * .35,
                          r.right() + 2, r.center().y() + 4 + lift * .15);
            wave2.lineTo(r.right() + 2, r.bottom());
            wave2.closeSubpath();
            p.setBrush(QColor(186, 230, 216, Mikan::light() ? 70 : 28));
            p.drawPath(wave2);
            if (m_pace == 2 && m_orb && m_orb->width() > 0) {
                const QRectF orb = m_orb->geometry();
                const qreal emerge = std::min(1.0, m_ring / 0.12);
                const qreal fade = (1.0 - m_ring) * (1.0 - m_ring);
                const int alpha = int(std::lround(100 * fade * emerge));
                if (alpha > 0) {
                    const qreal radius = (std::min(orb.width(), orb.height()) - 10) / 2.0 + 3 + m_ring * 20.0;
                    p.setBrush(Qt::NoBrush);
                    const QColor ring = Mikan::accent().mark;
                    p.setPen(QPen(QColor(ring.red(), ring.green(), ring.blue(), alpha), 2));
                    p.drawEllipse(orb.center(), radius, radius);
                }
            }
        }
    private:
        void sync() {
            const QWidget *top = window();
            const bool live = isVisible() && top && !top->isMinimized();
            if (live) {
                if (!m_timer->isActive()) m_timer->start(33);
            } else if (m_timer->isActive()) {
                m_timer->stop();
                m_last = 0;
            }
        }
        void tick() {
            const QWidget *top = window();
            if (!isVisible() || !top || top->isMinimized()) {
                m_timer->stop();
                m_last = 0;
                return;
            }
            const qint64 now = m_clock.elapsed();
            if (m_last == 0 || now - m_last > 80) {
                m_last = now;
                update();
                return;
            }
            const double dt = double(now - m_last);
            m_last = now;
            const double period = m_pace == 1 ? 4200.0 : 8000.0;
            m_phase += dt / period;
            if (m_phase >= 1.0) m_phase -= std::floor(m_phase);
            if (m_pace == 2) {
                m_ring += dt / 2500.0;
                if (m_ring >= 1.0) m_ring -= std::floor(m_ring);
            }
            const double blend = 1.0 - std::exp(-dt / 220.0);
            const qreal targetAmp = m_pace == 1 ? 10.0 : 7.0;
            const qreal targetLift = m_pace == 2 ? -6.0 : 0.0;
            m_amp += (targetAmp - m_amp) * blend;
            m_lift += (targetLift - m_lift) * blend;
            update();
        }
        MikanOrb *m_orb = nullptr;
        QTimer *m_timer = nullptr;
        QElapsedTimer m_clock;
        int m_pace = 0;
        qreal m_phase = 0;
        qreal m_ring = 0;
        qreal m_amp = 7;
        qreal m_lift = 0;
        qint64 m_last = 0;
    };

    void setHeroPace(QWidget *hero, int pace) {
        if (auto *card = dynamic_cast<HeroCard *>(hero)) card->setPace(pace);
    }

    QWidget *hairline() {
        auto *line = new QWidget;
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background-color: rgba(140, 130, 180, 42);"));
        return line;
    }

    QWidget *infoRow(const QString &icon, const QString &title, QLabel *value, QLabel *flag = nullptr) {
        auto *row = new QWidget;
        clearPlate(row);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(2, 4, 2, 4);
        h->setSpacing(8);
        h->addWidget(new Glyph(icon, 16));
        h->addWidget(label(title, 13, true));
        h->addStretch(1);
        if (flag) {
            flag->setFixedSize(18, 18);
            h->addWidget(flag);
        }
        clearPlate(value);
        value->setFont(Mikan::font(13, QFont::DemiBold));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(value);
        return row;
    }
}

MikanTitleBar::MikanTitleBar(QWidget *parent) : QWidget(parent) {
    clearPlate(this);
    setFixedHeight(48);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(4, 0, 2, 0);
    row->setSpacing(8);
    m_icon = new QLabel(this);
    m_icon->hide();
    row->addWidget(new LogoBadge);
    m_title = new QLabel(this);
    m_title->setFont(Mikan::font(16, QFont::DemiBold));
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(m_icon);
    row->addSpacing(8);
    row->addWidget(m_title, 1);
    auto *minimize = new WindowButton(WindowButton::Minimize, this);
    auto *maximize = new WindowButton(WindowButton::Maximize, this);
    auto *close = new WindowButton(WindowButton::Close, this);
    minimize->setToolTip(T("Minimize"));
    maximize->setToolTip(T("Maximize"));
    close->setToolTip(T("Close"));
    connect(minimize, &QAbstractButton::clicked, this, [this] { window()->showMinimized(); });
    connect(maximize, &QAbstractButton::clicked, this, [this] { window()->isMaximized() ? window()->showNormal() : window()->showMaximized(); });
    connect(close, &QAbstractButton::clicked, this, [this] { window()->close(); });
    row->addWidget(minimize);
    row->addWidget(maximize);
    row->addWidget(close);
}

void MikanTitleBar::follow(QWidget *window) {
    if (m_window == window) return;
    m_window = window;
    window->installEventFilter(this);
    m_icon->setPixmap(window->windowIcon().pixmap(16, 16));
    m_title->setText(window->windowTitle());
}

bool MikanTitleBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_window && event->type() == QEvent::WindowTitleChange) m_title->setText(m_window->windowTitle());
    if (watched == m_window && event->type() == QEvent::WindowIconChange) m_icon->setPixmap(m_window->windowIcon().pixmap(16, 16));
    return QWidget::eventFilter(watched, event);
}

void MikanTitleBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && window()->windowHandle()) window()->windowHandle()->startSystemMove();
}

void MikanTitleBar::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
}

// ---------------------------------------------------------------- the server list

QRect MikanProfileDelegate::chevronRect(const QRect &row) {
    return {row.right() - 5 - 6 - 32, row.top() + (row.height() - 3 - 32) / 2, 32, 32};
}

QSize MikanProfileDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const { return {200, 55}; }

void MikanProfileDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF row = QRectF(option.rect).adjusted(5, 0, -5, -3);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hover = option.state & QStyle::State_MouseOver;
    const int id = index.data(ProfilesTableModel::ProfileIdRole).toInt();
    const QColor ink = Mikan::text(), dim = Mikan::muted();
    Mikan::paintGlass(*p, row, 20, id == runningId ? QColor(47, 191, 115) : QColor());
    if (hover || selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(Mikan::overlay(hover && selected ? 16 : 10));
        p->drawRoundedRect(row.adjusted(.5, .5, -.5, -.5), 20, 20);
    }
    QString name = index.siblingAtColumn(ProfilesTableModel::ColName).data().toString().trimmed();
    const QString leading = Mikan::leadingFlag(name);
    if (!leading.isEmpty()) name = name.mid(leading.size()).trimmed();
    // The country flag, any flag at the start of the name, or a globe.
    const QRectF badge(row.left() + 10, row.center().y() - 14, 28, 28);
    const QPixmap flag = Mikan::flag(leading, 28, p->device()->devicePixelRatioF());
    if (!flag.isNull()) {
        QPainterPath clip;
        clip.addRoundedRect(badge, 10, 10);
        p->save();
        p->setClipPath(clip);
        p->drawPixmap(badge.topLeft(), flag);
        p->restore();
        // White in a flag would melt into the light theme without an edge, and black into the dark one.
        p->setPen(QPen(withAlpha(dim, Mikan::light() ? 70 : 110), 1));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(badge.adjusted(.5, .5, -.5, -.5), 10, 10);
    } else {
        Mikan::paintIcon(*p, QStringLiteral("globe"), badge.adjusted(5, 5, -5, -5), dim);
    }
    const QRect chevron = chevronRect(option.rect);
    Mikan::paintIcon(*p, QStringLiteral("chevron"), QRectF(chevron).adjusted(6, 6, -6, -6), withAlpha(ink, 200));
    // The test result: a spinner while it runs, then the latency in its colour.
    const QString result = index.siblingAtColumn(ProfilesTableModel::ColTestResult).data().toString();
    const int testState = index.siblingAtColumn(ProfilesTableModel::ColTestResult).data(ProfilesTableModel::TestStateRole).toInt();
    p->setFont(Mikan::font(12));
    const int resultWidth = std::clamp(p->fontMetrics().horizontalAdvance(result) + 12, 62, 200);
    const QRectF resultBox(chevron.left() - resultWidth, row.center().y() - 16, resultWidth, 32);
    const bool measuring = testState == 2 && index.siblingAtColumn(ProfilesTableModel::ColTestResult).data(ProfilesTableModel::TestCountryRole).toString().isEmpty();
    if (testState == 1) {
        p->setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap));
        p->setBrush(Qt::NoBrush);
        const QRectF spin(resultBox.center().x() - 9, resultBox.center().y() - 9, 18, 18);
        p->drawArc(spin, int(-spinAngle * 16), 270 * 16);
    } else if (measuring) {
        // The speed test: a gauge whose needle sweeps, as the classic table and the mobile rows show it.
        const QPointF c(resultBox.center().x(), resultBox.center().y() + 3);
        const qreal r = 10;
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(withAlpha(ink, 90), 2, Qt::SolidLine, Qt::RoundCap));
        p->drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), -20 * 16, 220 * 16);
        const qreal swing = std::sin(spinAngle * M_PI / 180);
        const qreal angle = (90 - 95 * swing) * M_PI / 180;
        p->setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap));
        p->drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), 200 * 16, -int((110 + 95 * swing) * 16));
        p->drawLine(c, c + QPointF(std::cos(angle) * r * .72, -std::sin(angle) * r * .72));
        p->setPen(Qt::NoPen);
        p->setBrush(ink);
        p->drawEllipse(c, 2, 2);
    } else if (!result.isEmpty()) {
        const QVariant colour = index.siblingAtColumn(ProfilesTableModel::ColTestResult).data(Qt::ForegroundRole);
        p->setPen(colour.isValid() ? colour.value<QColor>() : dim);
        p->drawText(resultBox, Qt::AlignCenter, p->fontMetrics().elidedText(result, Qt::ElideRight, int(resultBox.width())));
    }
    const qreal textLeft = badge.right() + 10;
    const qreal textRight = resultBox.left() - 4;
    p->setFont(Mikan::font(15, QFont::Bold));
    p->setPen(ink);
    const qreal width = textRight - textLeft;
    p->drawText(QRectF(textLeft, row.top() + 6, width, 22), Qt::AlignLeft | Qt::AlignBottom, p->fontMetrics().elidedText(name, Qt::ElideRight, int(width)));
    p->setFont(Mikan::font(12));
    p->setPen(dim);
    const QString type = index.siblingAtColumn(ProfilesTableModel::ColType).data().toString();
    p->drawText(QRectF(textLeft, row.top() + 29, width, 18), Qt::AlignLeft | Qt::AlignTop, p->fontMetrics().elidedText(type, Qt::ElideRight, int(width)));
    p->restore();
}

MikanProfileList::MikanProfileList(QWidget *parent) : QListView(parent) {
    rowDelegate = new MikanProfileDelegate(this);
    setItemDelegate(rowDelegate);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setUniformItemSizes(true);
    setVerticalScrollMode(ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(ExtendedSelection);
    setSelectionBehavior(SelectRows);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setContextMenuPolicy(Qt::CustomContextMenu);
    viewport()->setAutoFillBackground(false);
    setAutoFillBackground(false);
    m_spinner = new QTimer(this);
    connect(m_spinner, &QTimer::timeout, this, [this] {
        if (!model() || !isVisible()) return;
        // Repaints only while a visible row is being tested.
        const QModelIndex first = indexAt(QPoint(10, 2));
        const int from = first.isValid() ? first.row() : 0;
        const int to = std::min(model()->rowCount(), from + viewport()->height() / 55 + 2);
        for (int row = from; row < to; ++row) {
            if (model()->index(row, ProfilesTableModel::ColTestResult).data(ProfilesTableModel::TestStateRole).toInt() != 0) {
                rowDelegate->spinAngle = std::fmod(rowDelegate->spinAngle + 9, 360);
                viewport()->update();
                return;
            }
        }
    });
    m_spinner->start(33);
}

void MikanProfileList::mousePressEvent(QMouseEvent *e) {
    const QModelIndex index = indexAt(e->pos());
    if (e->button() == Qt::LeftButton && index.isValid() && MikanProfileDelegate::chevronRect(visualRect(index)).contains(e->pos())) {
        if (editRequested) editRequested(index);
        e->accept();
        return;
    }
    QListView::mousePressEvent(e);
}

void MikanProfileList::mouseMoveEvent(QMouseEvent *e) {
    const QModelIndex index = indexAt(e->pos());
    viewport()->setCursor(index.isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QListView::mouseMoveEvent(e);
}

void MikanProfileList::leaveEvent(QEvent *e) {
    viewport()->update();
    QListView::leaveEvent(e);
}

void MikanProfileList::paintEvent(QPaintEvent *e) {
    QListView::paintEvent(e);
    if (model() && model()->rowCount() == 0 && !emptyText.isEmpty()) {
        QPainter p(viewport());
        p.setFont(Mikan::font(14));
        p.setPen(Mikan::muted());
        p.drawText(viewport()->rect().adjusted(24, 0, -24, -80), Qt::AlignCenter | Qt::TextWordWrap, emptyText);
    }
}

void MikanProfileList::dropEvent(QDropEvent *event) {
    // The same reorder as the classic table: source rows, kept as a manual order.
    if (event->source() != this || !event->mimeData()->hasFormat("application/profile-row-number") || !rowsSwapped) {
        event->ignore();
        return;
    }
    QByteArray encoded = event->mimeData()->data("application/profile-row-number");
    QDataStream stream(&encoded, QIODevice::ReadOnly);
    int from;
    stream >> from;
    const QModelIndex target = indexAt(event->position().toPoint());
    int to = target.isValid() ? target.row() : model()->rowCount() - 1;
    if (target.isValid() && dropIndicatorPosition() == AboveItem) to--;
    if (to >= 0 && toSourceRow) {
        to = toSourceRow(to);
        if (to < 0) return;
    }
    rowsSwapped(from, to);
    event->accept();
}

// ---------------------------------------------------------------- the home screen

MikanHome::MikanHome(MainWindow *window) : QWidget(nullptr), mw(window) {
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 4, 18, 16);
    root->setSpacing(12);
    m_titleBar = new MikanTitleBar(this);
    root->addWidget(m_titleBar);
    buildHero(root);
    buildControls(root);

    m_lower = new QStackedWidget(this);
    clearPlate(m_lower);
    auto *dashboard = new QWidget(m_lower);
    clearPlate(dashboard);
    buildDashboard(dashboard);
    m_lower->addWidget(dashboard);
    root->addWidget(m_lower, 1);

    m_drawer = new SidePanel(this);
    m_drawer->hide();
    auto *drawerLayout = new QVBoxLayout(m_drawer);
    drawerLayout->setContentsMargins(0, 0, 0, 0);
    drawerLayout->setSpacing(0);
    buildBody(drawerLayout);

    connect(Engine::host()->stats(), &Engine::StatsService::trafficSampled, this, [this](const Stats::TrafficSample &s) {
        m_rateDown = s.coreRunning ? s.proxyDown : 0;
        m_rateUp = s.coreRunning ? s.proxyUp : 0;
        updateRates(s.coreRunning);
    });
    connect(mw->ui->tabWidget, &QTabWidget::currentChanged, this, [this] { groupChanged(); });
    connect(mw->ui->data_view, &QTextBrowser::textChanged, this, [this] { updateNotice(); });
    connect(mw->ui->stats_widget, &QTabWidget::currentChanged, this, [this](int index) {
        if (m_syncingTabs || !m_attached) return;
        const int tab = m_tabStats.indexOf(index);
        if (tab < 0) return;
        for (int i = 0; i < m_tabs.size(); ++i) m_tabs[i]->setSelected(i == tab);
    });
    // Asked for from a menu (the runtime statistics): the page comes forward.
    connect(mw->ui->actionRuntime_Stats, &QAction::triggered, this, [this] {
        if (!m_attached) return;
        selectTab(m_tabStats.indexOf(mw->ui->stats_widget->indexOf(mw->ui->runtime_tab)));
    });
    auto *find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, [this] {
        if (m_detailsOpen) showDetails(false);
        m_search->setVisible(!m_search->isVisible() || !m_search->hasFocus());
        if (m_search->isVisible()) m_search->setFocus();
        else m_search->clear();
    });

    m_tick = new QTimer(this);
    connect(m_tick, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
    m_tick->start(250);
}

void MikanHome::buildHero(QVBoxLayout *root) {
    auto *hero = new HeroCard;
    m_hero = hero;
    hero->setMinimumHeight(128);
    auto *row = new QHBoxLayout(hero);
    row->setContentsMargins(22, 16, 20, 16);
    row->setSpacing(16);

    m_orb = new MikanOrb;
    hero->follow(m_orb);
    m_orb->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_orb, &QAbstractButton::clicked, this, [this] { mw->ui->toolButton_startstop->click(); });
    connect(m_orb, &QWidget::customContextMenuRequested, this, [this] {
        if (!Configs::dataManager->settingsRepo->kill_switch) return;
        auto *menu = new QMenu(this);
        auto *disable = menu->addAction(QCoreApplication::translate("MainWindow", "Disable Kill Switch"));
        connect(disable, &QAction::triggered, this, [this] { mw->confirm_disable_kill_switch(); });
        openSheet(m_orb, menu, true, false);
    });
    row->addWidget(m_orb, 0, Qt::AlignVCenter);

    auto *status = new QVBoxLayout;
    status->setSpacing(2);
    status->setContentsMargins(0, 4, 0, 4);
    auto *statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    m_status = label(T("Click to connect"), 20, false, QFont::Bold);
    m_dot = new StatusDot;
    statusRow->addWidget(m_status, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_dot, 0, Qt::AlignVCenter);
    statusRow->addStretch(1);
    status->addLayout(statusRow);

    auto *viaRow = new QHBoxLayout;
    viaRow->setSpacing(6);
    m_flag = new QLabel;
    clearPlate(m_flag);
    m_flag->setFixedSize(18, 18);
    m_flag->hide();
    m_via = label({}, 13, true);
    viaRow->addWidget(m_flag, 0, Qt::AlignVCenter);
    viaRow->addWidget(m_via, 1);
    status->addLayout(viaRow);

    m_latencyRow = new QWidget;
    clearPlate(m_latencyRow);
    auto *latency = new QHBoxLayout(m_latencyRow);
    latency->setContentsMargins(0, 2, 0, 0);
    latency->setSpacing(6);
    latency->addWidget(new Glyph(QStringLiteral("bolt"), 14));
    m_check = label(emdash(), 13, true);
    m_check->setProperty("mikanRole", "link");
    latency->addWidget(m_check);
    latency->addStretch(1);
    status->addWidget(m_latencyRow);
    m_latencyRow->installEventFilter(this);
    for (auto *child : m_latencyRow->findChildren<QWidget *>()) child->installEventFilter(this);
    row->addLayout(status, 1);

    m_up = new QLabel;
    m_down = new QLabel;
    for (auto *speed : {m_up, m_down}) {
        clearPlate(speed);
        speed->setTextFormat(Qt::RichText);
        speed->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        speed->setMinimumWidth(128);
    }
    auto *speeds = new QVBoxLayout;
    speeds->setSpacing(4);
    speeds->addStretch(1);
    speeds->addWidget(m_up);
    speeds->addWidget(m_down);
    speeds->addStretch(1);
    row->addLayout(speeds);
    updateRates(false);
    root->addWidget(hero);
}

void MikanHome::buildControls(QVBoxLayout *root) {
    auto *row = new QHBoxLayout;
    row->setSpacing(12);

    auto *subscription = new GlassCard(24);
    auto *subscriptionLayout = new QVBoxLayout(subscription);
    subscriptionLayout->setContentsMargins(16, 12, 12, 12);
    subscriptionLayout->setSpacing(8);
    subscriptionLayout->addStretch(1);
    subscriptionLayout->addWidget(label(T("Groups"), 12, true));
    auto *chipRow = new QHBoxLayout;
    chipRow->setSpacing(8);
    m_chip = new MikanTile(MikanTile::Chip);
    m_chip->setTextSize(15);
    m_chip->setMinimumWidth(0);
    m_chip->setFixedHeight(44);
    m_chip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_chip, &QAbstractButton::clicked, this, [this] { showGroupPicker(); });
    m_add = new MikanTile(MikanTile::Icon);
    m_add->setIconName(QStringLiteral("add"));
    m_add->setToolTip(T("Add"));
    m_add->setFixedSize(44, 44);
    connect(m_add, &QAbstractButton::clicked, this, [this] { showAddMenu(); });
    m_more = new MikanTile(MikanTile::Icon);
    m_more->setIconName(QStringLiteral("settings"));
    m_more->setToolTip(T("Subscription settings"));
    m_more->setFixedSize(44, 44);
    connect(m_more, &QAbstractButton::clicked, this, [this] { showSubscriptionMenu(); });
    chipRow->addWidget(m_chip, 1);
    chipRow->addWidget(m_add);
    chipRow->addWidget(m_more);
    subscriptionLayout->addLayout(chipRow);
    subscriptionLayout->addStretch(1);
    row->addWidget(subscription, 4);

    auto *modes = new QVBoxLayout;
    modes->setSpacing(8);
    const auto mode = [&](MikanSwitch *&slot, const QString &title, const QString &description, const QString &icon) {
        auto *card = new GlassCard(20);
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(4, 2, 8, 2);
        layout->setSpacing(0);
        slot = new MikanSwitch(title);
        slot->setDescription(description);
        slot->setIconName(icon);
        slot->setBare(true);
        slot->setToolTip(description);
        slot->setFixedHeight(64);
        slot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout->addWidget(slot);
        modes->addWidget(card);
    };
    mode(m_tun, T("TUN mode"), T("Route traffic through a virtual adapter"), QStringLiteral("tun"));
    mode(m_proxy, T("System proxy"), T("Send system traffic through the proxy"), QStringLiteral("globe"));
    connect(m_tun, &QAbstractButton::clicked, this, [this] { mw->connection()->setTun(!Configs::dataManager->settingsRepo->spmode_vpn); });
    connect(m_proxy, &QAbstractButton::clicked, this, [this] { mw->connection()->setSystemProxy(!Configs::dataManager->settingsRepo->spmode_system_proxy); });
    row->addLayout(modes, 5);
    root->addLayout(row);
}

void MikanHome::buildDashboard(QWidget *page) {
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(2, 2, 2, 6);
    row->setSpacing(12);

    auto *servers = new GlassCard(24);
    auto *serversLayout = new QVBoxLayout(servers);
    serversLayout->setContentsMargins(8, 8, 8, 8);
    serversLayout->setSpacing(0);
    buildServerList(serversLayout);
    row->addWidget(servers, 3);

    auto *info = new GlassCard(24);
    info->setMinimumWidth(248);
    auto *infoLayout = new QVBoxLayout(info);
    infoLayout->setContentsMargins(16, 14, 16, 12);
    infoLayout->setSpacing(2);
    auto *infoHead = new QHBoxLayout;
    infoHead->setSpacing(8);
    infoHead->addWidget(new Glyph(QStringLiteral("info"), 18));
    infoHead->addWidget(label(T("Connection Info"), 15, false, QFont::DemiBold));
    infoHead->addStretch(1);
    infoLayout->addLayout(infoHead);
    infoLayout->addSpacing(4);
    m_serverFlag = new QLabel;
    clearPlate(m_serverFlag);
    m_serverFlag->hide();
    m_server = new QLabel(emdash());
    m_latency = new QLabel(emdash());
    m_protocol = new QLabel(emdash());
    m_transport = new QLabel(emdash());
    m_uptime = new QLabel(emdash());
    m_inbound = new QLabel(emdash());
    const auto addRow = [&](const QString &icon, const QString &title, QLabel *value, QLabel *flag = nullptr) {
        infoLayout->addWidget(hairline());
        infoLayout->addWidget(infoRow(icon, title, value, flag));
    };
    addRow(QStringLiteral("subscriptions"), T("Server"), m_server, m_serverFlag);
    addRow(QStringLiteral("bolt"), T("Latency"), m_latency);
    addRow(QStringLiteral("link"), T("Protocol"), m_protocol);
    addRow(QStringLiteral("route"), T("Transport"), m_transport);
    addRow(QStringLiteral("clock"), T("Uptime"), m_uptime);
    addRow(QStringLiteral("globe"), T("Inbound"), m_inbound);
    m_running = label({}, 12, true);
    m_running->setWordWrap(true);
    m_running->setContentsMargins(4, 6, 4, 0);
    m_running->hide();
    infoLayout->addWidget(m_running);
    infoLayout->addStretch(1);
    auto *corner = new QHBoxLayout;
    corner->setContentsMargins(0, 6, 0, 0);
    corner->setSpacing(2);
    corner->addStretch(1);
    const auto cornerButton = [](const QString &icon, const QString &tip) {
        auto *button = new MikanTile(MikanTile::Icon);
        button->setFlat(true);
        button->setIconName(icon);
        button->setToolTip(tip);
        button->setFixedSize(36, 36);
        return button;
    };
    m_details = cornerButton(QStringLiteral("logs"), T("Logs, connections and statistics"));
    connect(m_details, &QAbstractButton::clicked, this, [this] { showDetails(true); });
    auto *settings = cornerButton(QStringLiteral("settings"), T("Settings"));
    connect(settings, &QAbstractButton::clicked, this, [this, settings] { showAppMenu(settings); });
    corner->addWidget(m_details);
    corner->addWidget(settings);
    infoLayout->addLayout(corner);
    row->addWidget(info, 2);
}

void MikanHome::buildServerList(QVBoxLayout *serversLayout) {
    m_notice = new GlassCard(20);
    m_noticeLayout = new QVBoxLayout(m_notice);
    m_noticeLayout->setContentsMargins(12, 4, 12, 4);
    m_notice->setMaximumHeight(0);
    m_notice->hide();
    serversLayout->addWidget(m_notice);

    // The focused group's header above the list, so that only the servers scroll.
    auto *title = new QHBoxLayout;
    title->setContentsMargins(12, 4, 0, 0);
    title->setSpacing(0);
    auto *names = new QVBoxLayout;
    names->setContentsMargins(10, 8, 4, 8);
    names->setSpacing(3);
    m_groupName = label({}, 17, false);
    m_groupDate = label({}, 10, true);
    names->addWidget(m_groupName);
    names->addWidget(m_groupDate);
    title->addLayout(names, 1);
    const auto action = [this, title](const QString &icon, const QString &tip, const std::function<void()> &callback) {
        auto *button = new MikanTile(MikanTile::Icon);
        button->setFlat(true);
        button->setIconName(icon);
        button->setToolTip(tip);
        button->setFixedSize(42, 48);
        connect(button, &QAbstractButton::clicked, this, callback);
        title->addWidget(button);
        return button;
    };
    m_refresh = action("refresh", T("Update subscription"), [this] { mw->ui->menu_update_subscription->trigger(); });
    action("test", T("Ping test"), [this] { mw->ui->actionUrl_Test_Group->trigger(); });
    // The desktop has a speed test the mobile app lacks: it sits beside the ping test.
    action("speedtest", T("Speed test"), [this] { mw->ui->actionSpeedtest_Group->trigger(); });
    m_groupMore = action("more", T("More"), [this] { showGroupMenu(); });
    serversLayout->addLayout(title);

    m_traffic = new QWidget;
    clearPlate(m_traffic);
    auto *traffic = new QHBoxLayout(m_traffic);
    traffic->setContentsMargins(12, 0, 12, 8);
    traffic->setSpacing(8);
    auto *info = new MikanTile(MikanTile::Icon);
    info->setFlat(true);
    info->setIconName("info");
    info->setFixedSize(32, 32);
    info->setToolTip(T("Subscription page"));
    connect(info, &QAbstractButton::clicked, this, [] {
        const auto group = Configs::dataManager->groupsRepo->CurrentGroup();
        if (!group) return;
        const QUrl url(group->sub_info.web_url.isEmpty() ? group->url : group->sub_info.web_url);
        if (url.scheme() == "https" || url.scheme() == "http") QDesktopServices::openUrl(url);
    });
    m_quota = label({}, 11, false);
    m_quota->setAlignment(Qt::AlignCenter);
    m_quota->setFixedHeight(28);
    // A stylesheet radius paints the label's square corners in the palette pink, over the glass pill.
    m_quota->setContentsMargins(8, 0, 8, 0);
    auto *bar = new GlassCard(14);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->addWidget(m_quota);
    m_support = new MikanTile(MikanTile::Icon);
    m_support->setFlat(true);
    m_support->setIconName("globe");
    m_support->setFixedSize(32, 32);
    m_support->setToolTip(T("Support"));
    connect(m_support, &QAbstractButton::clicked, this, [] {
        const auto group = Configs::dataManager->groupsRepo->CurrentGroup();
        const QUrl url(group ? group->sub_info.support_url : QString());
        if (url.scheme() == "https" || url.scheme() == "http") QDesktopServices::openUrl(url);
    });
    traffic->addWidget(info);
    traffic->addWidget(bar, 1);
    traffic->addWidget(m_support);
    serversLayout->addWidget(m_traffic);
    m_expiry = label({}, 11, true);
    m_expiry->setContentsMargins(14, 0, 14, 8);
    serversLayout->addWidget(m_expiry);
    m_announce = label({}, 12, true);
    m_announce->setWordWrap(true);
    m_announce->setContentsMargins(14, 0, 14, 10);
    m_announce->setCursor(Qt::PointingHandCursor);
    m_announce->installEventFilter(this);
    serversLayout->addWidget(m_announce);

    m_search = new QLineEdit;
    m_search->setPlaceholderText(T("Search by name"));
    m_search->setClearButtonEnabled(true);
    m_search->hide();
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        mw->nameFilterString = text;
        mw->applyProfileFilters();
    });
    auto *searchBox = new QHBoxLayout;
    searchBox->setContentsMargins(5, 0, 5, 8);
    searchBox->addWidget(m_search);
    serversLayout->addLayout(searchBox);

    m_list = new MikanProfileList;
    m_list->setModel(mw->profilesFilterModel);
    m_list->setModelColumn(ProfilesTableModel::ColName);
    // One selection for both views: every menu acts on the servers chosen here.
    m_list->setSelectionModel(mw->ui->profilesTableView->selectionModel());
    m_list->rowsSwapped = [this](int from, int to) { if (mw->ui->profilesTableView->rowsSwapped) mw->ui->profilesTableView->rowsSwapped(from, to); };
    m_list->toSourceRow = [this](int row) { return mw->profilesFilterModel->toSourceRow(row); };
    m_list->editRequested = [this](const QModelIndex &index) { mw->on_profilesTableView_doubleClicked(index); };
    // One click starts a server, as on the phone; with a modifier it only chooses rows.
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex &index) {
        if (QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) return;
        // The chevron's press never reached the view, so its release may report an older press.
        if (MikanProfileDelegate::chevronRect(m_list->visualRect(index)).contains(m_list->viewport()->mapFromGlobal(QCursor::pos()))) return;
        if (mw->select_mode) { mw->on_profilesTableView_doubleClicked(index); return; }
        const int id = index.data(ProfilesTableModel::ProfileIdRole).toInt();
        if (id != m_list->rowDelegate->runningId) mw->profile_start(id);
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto *selection = m_list->selectionModel();
        const QModelIndex index = m_list->indexAt(pos);
        // A right-click on a row that is not part of the selection becomes the selection, as a table does.
        if (index.isValid() && selection && !selection->isSelected(index))
            selection->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QString hint;
        if (selection) {
            const auto rows = selection->selectedRows(m_list->modelColumn());
            if (rows.size() == 1) {
                QString name = rows.first().data().toString().trimmed();
                const QString leading = Mikan::leadingFlag(name);
                if (!leading.isEmpty()) name = name.mid(leading.size()).trimmed();
                hint = name;
            } else if (rows.size() > 1) {
                hint = T("%1 selected").arg(rows.size());
            }
        }
        openSheet(m_list, mw->ui->menu_server, false, false, m_list->viewport()->mapToGlobal(pos), true, hint);
    });
    serversLayout->addWidget(m_list, 1);

    m_empty = new QWidget;
    auto *imports = new QHBoxLayout(m_empty);
    imports->setContentsMargins(5, 8, 5, 0);
    imports->setSpacing(16);
    const auto importButton = [imports](const QString &text, QAction *action) {
        auto *button = new MikanTile(MikanTile::Text);
        button->setText(text);
        button->setFixedHeight(48);
        QObject::connect(button, &QAbstractButton::clicked, action, &QAction::trigger);
        imports->addWidget(button, 1);
    };
    importButton(T("Clipboard"), mw->ui->menu_add_from_clipboard);
    importButton(T("QR code"), mw->ui->menu_scan_qr);
    m_empty->hide();
    serversLayout->addWidget(m_empty);
}

void MikanHome::buildBody(QVBoxLayout *body) {
    // Logs, connections, the traffic graph and runtime statistics. None of these stay on the dashboard.
    auto *panel = new GlassCard(24);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(14, 12, 12, 12);
    panelLayout->setSpacing(10);

    auto *tabs = new QHBoxLayout;
    tabs->setSpacing(6);
    for (int i = 0; i < mw->ui->stats_widget->count(); ++i) {
        auto *tab = new MikanTile(MikanTile::Tab);
        tab->setText(mw->ui->stats_widget->tabText(i).remove(QLatin1Char('&')));
        tab->setFixedHeight(40);
        const int statsIndex = i;
        connect(tab, &QAbstractButton::clicked, this, [this, statsIndex] { selectTab(m_tabStats.indexOf(statsIndex)); });
        m_tabs << tab;
        m_tabStats << statsIndex;
        tabs->addWidget(tab);
    }
    tabs->addStretch(1);
    auto *close = new WindowButton(WindowButton::Close, panel);
    close->setToolTip(T("Close"));
    connect(close, &QAbstractButton::clicked, this, [this] { showDetails(false); });
    tabs->addWidget(close, 0, Qt::AlignVCenter);
    panelLayout->addLayout(tabs);

    auto *stats = new QWidget;
    clearPlate(stats);
    m_statsLayout = new QVBoxLayout(stats);
    m_statsLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->addWidget(stats, 1);
    body->addWidget(panel);
    if (!m_tabs.isEmpty()) m_tabs.first()->setSelected(true);
}

void MikanHome::attach() {
    if (m_attached) return;
    const auto find = [this](QWidget *widget) {
        Slot slot;
        for (auto *layout : mw->ui->centralwidget->findChildren<QBoxLayout *>()) {
            if (const int index = layout->indexOf(widget); index >= 0) {
                slot = {layout, index, layout->stretch(index)};
                break;
            }
        }
        return slot;
    };
    m_titleBar->follow(mw);
    m_dataSlot = find(mw->ui->data_view);
    m_statsSlot = find(mw->ui->stats_widget);
    m_noticeLayout->addWidget(mw->ui->data_view);
    m_statsLayout->addWidget(mw->ui->stats_widget);
    mw->ui->stats_widget->tabBar()->hide();
    mw->ui->stats_widget->setDocumentMode(true);
    m_attached = true;
    if (!m_tabStats.isEmpty()) mw->ui->stats_widget->setCurrentIndex(m_tabStats.first());
    m_renderedGroup = -1;
    m_headerKey.clear();
    updateNotice();
    refresh();
}

void MikanHome::detach() {
    if (!m_attached) return;
    m_attached = false;
    if (m_search->isVisible() || !m_search->text().isEmpty()) { m_search->clear(); m_search->hide(); }
    mw->ui->stats_widget->tabBar()->show();
    mw->ui->stats_widget->setDocumentMode(false);
    if (m_dataSlot.layout) m_dataSlot.layout->insertWidget(m_dataSlot.index, mw->ui->data_view, m_dataSlot.stretch);
    if (m_statsSlot.layout) m_statsSlot.layout->insertWidget(m_statsSlot.index, mw->ui->stats_widget, m_statsSlot.stretch);
    mw->ui->data_view->show();
    mw->ui->stats_widget->show();
}

void MikanHome::updateRates(bool live) {
    const bool on = live && !Configs::dataManager->settingsRepo->disable_traffic_stats;
    const QString family = Mikan::fontFamily().toHtmlEscaped();
    const QString ink = Mikan::text().name();
    const auto line = [&](const QString &arrow, const QColor &color, double rate) {
        const QString value = on ? ReadableSize(qint64(std::max(0.0, rate))) + QStringLiteral("/s") : emdash();
        return QStringLiteral("<span style=\"font-family:'%1';color:%2;font-size:15px;font-weight:600\">%3</span>&nbsp;&nbsp;<span style=\"font-family:'%4';color:%5;font-size:16px;font-weight:600\">%6</span>")
            .arg(family, color.name(), arrow, family, ink, value.toHtmlEscaped());
    };
    if (m_up) m_up->setText(line(QStringLiteral("↑"), Mikan::accent().mark, m_rateUp));
    if (m_down) m_down->setText(line(QStringLiteral("↓"), QColor(46, 164, 214), m_rateDown));
}

void MikanHome::placeDrawer() {
    if (!m_lower || !m_drawer) return;
    const QRect area = m_lower->geometry();
    if (area.isEmpty()) return;
    const int x = area.left() + qRound((1.0 - m_drawerProgress) * (area.width() + 28));
    m_drawer->setGeometry(x, area.top(), area.width(), area.height());
    m_drawer->setVisible(m_drawerProgress > 0.001);
    if (m_drawer->isVisible()) m_drawer->raise();
}

void MikanHome::showDetails(bool open) {
    if (!m_drawer || open == m_detailsOpen) return;
    m_detailsOpen = open;
    if (!m_drawerAnim) {
        m_drawerAnim = new QVariantAnimation(this);
        m_drawerAnim->setDuration(280);
        m_drawerAnim->setEasingCurve(softCurve());
        connect(m_drawerAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_drawerProgress = value.toReal();
            placeDrawer();
        });
        connect(m_drawerAnim, &QVariantAnimation::finished, this, [this] {
            if (!m_detailsOpen) m_drawer->hide();
            placeDrawer();
        });
    }
    if (open) {
        m_drawer->show();
        m_drawer->raise();
    }
    m_drawerAnim->stop();
    m_drawerAnim->setStartValue(m_drawerProgress);
    m_drawerAnim->setEndValue(open ? 1.0 : 0.0);
    m_drawerAnim->start();
}

void MikanHome::selectTab(int index, bool animated) {
    if (index < 0 || index >= m_tabs.size()) return;
    const bool opening = animated && !m_detailsOpen;
    if (opening) showDetails(true);
    for (int i = 0; i < m_tabs.size(); ++i) m_tabs[i]->setSelected(i == index);
    const int statsIndex = m_tabStats.value(index, -1);
    const bool changed = statsIndex >= 0 && mw->ui->stats_widget->currentIndex() != statsIndex;
    if (statsIndex >= 0) {
        m_syncingTabs = true;
        mw->ui->stats_widget->setCurrentIndex(statsIndex);
        m_syncingTabs = false;
    }
    if (animated && changed && !opening && m_statsLayout && m_statsLayout->parentWidget()) Mikan::rise(m_statsLayout->parentWidget(), 12, 220);
}

void MikanHome::groupChanged() {
    if (!m_attached) return;
    refreshGroupHeader();
    const int current = Configs::dataManager->settingsRepo->current_group;
    // Only a change of group rises; the first render is simply there.
    if (m_renderedGroup != -1 && m_renderedGroup != current && !m_detailsOpen) Mikan::rise(m_list, 14, 240);
    m_renderedGroup = current;
    // The classic table scrolls to the chosen server; so does the list, once the group's rows are in.
    QTimer::singleShot(0, m_list, [this] { if (m_list->currentIndex().isValid()) m_list->scrollTo(m_list->currentIndex(), QAbstractItemView::PositionAtCenter); });
}

void MikanHome::updateNotice() {
    if (!m_attached) return;
    const bool show = !mw->ui->data_view->toPlainText().trimmed().isEmpty();
    if (show == m_notice->isVisible() && !(m_noticeAnim && m_noticeAnim->state() == QAbstractAnimation::Running)) return;
    // Appearing or leaving, the card changes height gradually: the list under it never jumps.
    if (!m_noticeAnim) {
        m_noticeAnim = new QVariantAnimation(this);
        m_noticeAnim->setDuration(220);
        m_noticeAnim->setEasingCurve(softCurve());
        connect(m_noticeAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { m_notice->setMaximumHeight(v.toInt()); });
    }
    const int full = std::max(48, mw->ui->data_view->minimumHeight() + 8);
    m_noticeAnim->stop();
    disconnect(m_noticeAnim, &QVariantAnimation::finished, this, nullptr);
    m_notice->show();
    m_noticeAnim->setStartValue(m_notice->maximumHeight());
    m_noticeAnim->setEndValue(show ? full : 0);
    connect(m_noticeAnim, &QVariantAnimation::finished, this, [this, show] {
        if (show) m_notice->setMaximumHeight(QWIDGETSIZE_MAX);
        else m_notice->hide();
        m_notice->setContentsMargins(0, 0, 0, show ? 6 : 0);
    });
    if (!isVisible()) { m_notice->setMaximumHeight(show ? QWIDGETSIZE_MAX : 0); m_notice->setVisible(show); return; }
    m_noticeAnim->start();
}

void MikanHome::refreshGroupHeader() {
    const auto group = Configs::dataManager->groupsRepo->CurrentGroup();
    if (!group) return;
    const auto &sub = group->sub_info;
    const bool subscription = !group->url.isEmpty();
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
        .arg(group->id).arg(group->name).arg(group->sub_last_update).arg(sub.used()).arg(sub.total).arg(sub.expire)
        .arg(sub.announce, sub.support_url).arg(int(mw_sub_updating) * 2 + int(m_announceOpen));
    m_refresh->setSpinning(subscription && mw_sub_updating);
    if (key == m_headerKey) return;
    m_headerKey = key;
    m_chip->setText(group->name);
    m_chip->setToolTip(group->name);
    m_groupName->setText(group->name);
    m_groupDate->setVisible(subscription && group->sub_last_update > 0);
    if (group->sub_last_update > 0) m_groupDate->setText(QLocale().toString(QDateTime::fromSecsSinceEpoch(group->sub_last_update), QLocale::ShortFormat));
    m_refresh->setVisible(subscription);
    m_traffic->setVisible(subscription);
    m_quota->setText(sub.valid && sub.has_quota
        ? ReadableSize(sub.used()) + QStringLiteral(" / ") + (sub.total > 0 ? ReadableSize(sub.total) : QStringLiteral("∞"))
        : T("Traffic not provided"));
    m_support->setVisible(!sub.support_url.isEmpty());
    m_expiry->setVisible(subscription && sub.expire > 0);
    if (sub.expire > 0) m_expiry->setText(T("Expiry: %1").arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(sub.expire).date(), QLocale::ShortFormat)));
    m_announce->setVisible(subscription && !sub.announce.isEmpty());
    // Two lines until clicked, so a long note does not hide the servers.
    QString note = sub.announce;
    if (!m_announceOpen) {
        const QFontMetrics fm(m_announce->font());
        const QStringList lines = note.split(QLatin1Char('\n'));
        note = lines.mid(0, 2).join(QLatin1Char('\n'));
        if (lines.size() > 2 || fm.horizontalAdvance(note) > 2 * std::max(200, m_announce->width() - 28)) {
            note = fm.elidedText(note.simplified(), Qt::ElideRight, 2 * std::max(200, m_announce->width() - 28) - 20);
        }
    }
    m_announce->setText(note);
    const bool alone = Configs::dataManager->groupsRepo->GetGroupsTabOrder().size() <= 1;
    m_list->emptyText = alone && !subscription
        ? T("To use the app, add a proxy server and/or subscription to the server list.")
        : T("This group has no servers yet.");
}

void MikanHome::refresh() {
    if (!m_attached) return;
    auto *button = mw->ui->toolButton_startstop;
    const auto state = button->state();
    const bool connected = state == StartStopButton::State::Running;
    const bool connecting = state == StartStopButton::State::Connecting;
    if (connected && !m_wasRunning) m_connectedFor.start();
    m_wasRunning = connected;
    QString status;
    if (connecting) status = T("Connecting…");
    else if (state == StartStopButton::State::Disconnecting) status = T("Stopping…");
    else if (connected) status = T("Connected");
    const qint64 seconds = connected && m_connectedFor.isValid() ? m_connectedFor.elapsed() / 1000 : 0;
    const QString elapsed = QStringLiteral("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    m_orb->setState(connected, connecting, status, elapsed);
    setHeroPace(m_hero, connecting ? 1 : connected ? 2 : 0);
    m_orb->setEnabled(state != StartStopButton::State::Disabled && state != StartStopButton::State::Disconnecting);
    if (m_status) m_status->setText(status.isEmpty() ? T("Click to connect") : status);
    if (m_dot) static_cast<StatusDot *>(m_dot)->setOn(connected);
    if (m_uptime) m_uptime->setText(connected ? elapsed : emdash());

    const auto &settings = Configs::dataManager->settingsRepo;
    m_tun->setOn(settings->spmode_vpn);
    m_proxy->setOn(settings->spmode_system_proxy);
    updateRates(connected);

    // The check of the current connection: its result for a moment, then the stored latency again.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString prefix = QCoreApplication::translate("MainWindow", "Test Result") + QStringLiteral(": ");
    const QString runningText = mw->ui->label_running->text();
    if (m_checkSince > 0) {
        if (runningText.startsWith(prefix)) {
            const QString result = runningText.mid(prefix.size());
            static const QRegularExpression ms(QStringLiteral("^(\\d+) ms$"));
            const auto match = ms.match(result);
            m_checkResult = match.hasMatch() ? QStringLiteral("%1 ms").arg(match.captured(1)) : T("n/a");
            m_checkShownAt = now;
            m_checkSince = 0;
        } else if (now - m_checkSince > 20000) {
            m_checkSince = 0;
        }
    }
    if (m_checkShownAt > 0 && now - m_checkShownAt > 2500) { m_checkShownAt = 0; m_checkResult.clear(); }
    if (!connected) { m_checkSince = 0; m_checkShownAt = 0; m_checkResult.clear(); }

    const bool testLine = runningText.startsWith(prefix);
    if (!testLine && m_running->text() != runningText) m_running->setText(runningText);
    m_running->setVisible(!testLine && (runningText.contains(QLatin1Char('\n')) || runningText.contains(QLatin1Char('*'))));
    if (m_inbound->text() != mw->ui->label_inbound->text()) m_inbound->setText(mw->ui->label_inbound->text());
    m_inbound->setToolTip(mw->ui->label_inbound->toolTip());

    const auto running = mw->runningSnapshot();
    QString place;
    QString leading;
    if (running && running->outbound) {
        place = running->outbound->DisplayName().trimmed();
        leading = Mikan::leadingFlag(place);
        if (!leading.isEmpty()) place = place.mid(leading.size()).trimmed();
        if (m_via) m_via->setText(T("via %1").arg(place));
        const auto parts = running->outbound->DisplayProtocol().split(QStringLiteral(" / "));
        if (m_server) m_server->setText(place.isEmpty() ? parts.value(0) : place);
        if (m_protocol) m_protocol->setText(parts.value(0).isEmpty() ? emdash() : parts.value(0));
        if (m_transport) m_transport->setText(parts.size() > 1 ? parts.value(1) : emdash());
    } else {
        if (const auto group = Configs::dataManager->groupsRepo->CurrentGroup()) {
            place = group->name;
            leading = Mikan::leadingFlag(place);
            if (!leading.isEmpty()) place = place.mid(leading.size()).trimmed();
        }
        if (m_via) m_via->setText(place);
        if (m_server) m_server->setText(emdash());
        if (m_protocol) m_protocol->setText(emdash());
        if (m_transport) m_transport->setText(emdash());
    }
    const QPixmap art = Mikan::flag(leading, 18, devicePixelRatioF());
    if (m_flag) {
        m_flag->setPixmap(art);
        m_flag->setVisible(!art.isNull());
    }
    if (m_serverFlag) {
        const bool showFlag = running && !art.isNull();
        m_serverFlag->setPixmap(showFlag ? art : QPixmap());
        m_serverFlag->setVisible(showFlag);
    }
    if (m_latency) {
        if (!m_checkResult.isEmpty() && m_checkResult != T("n/a")) m_latency->setText(m_checkResult);
        else if (running && running->latency > 0) m_latency->setText(QStringLiteral("%1 ms").arg(running->latency));
        else m_latency->setText(emdash());
    }
    QString check;
    if (!connected) check = emdash();
    else if (m_checkSince > 0) check = T("Checking…");
    else if (!m_checkResult.isEmpty()) check = m_checkResult;
    else if (running && running->latency > 0) check = QStringLiteral("%1 ms").arg(running->latency);
    else check = T("Check connection");
    if (m_check && m_check->text() != check) m_check->setText(check);
    if (m_latencyRow) m_latencyRow->setCursor(connected ? Qt::PointingHandCursor : Qt::ArrowCursor);

    const int runningId = running ? running->id : -1;
    if (m_list->rowDelegate->runningId != runningId) {
        m_list->rowDelegate->runningId = runningId;
        m_list->viewport()->update();
    }
    refreshGroupHeader();
    m_empty->setVisible(m_list->model() && m_list->model()->rowCount() == 0 && m_search->text().isEmpty());
    if (m_renderedGroup == -1) m_renderedGroup = settings->current_group;
}

void MikanHome::checkCurrentConnection() {
    if (m_checkSince > 0) return;
    m_checkResult.clear();
    m_checkShownAt = 0;
    m_checkSince = QDateTime::currentMSecsSinceEpoch();
    mw->url_test_current();
    refresh();
}

void MikanHome::showGroupPicker() {
    auto *popup = new GlassPopup(this);
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: none; }"));
    auto *list = new QWidget;
    clearPlate(list);
    auto *rows = new QVBoxLayout(list);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(6);

    const int current = Configs::dataManager->settingsRepo->current_group;
    for (const int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
        const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
        if (!group) continue;
        auto *row = new MikanTile(MikanTile::Text);
        row->setFilled(true);
        row->setText(group->name);
        row->setSubtitle(group->url.isEmpty() ? T("Local group") : T("Subscription"));
        row->setSelected(gid == current);
        row->setFixedHeight(60);
        QObject::connect(row, &QAbstractButton::clicked, popup, [this, popup, gid] {
            popup->close();
            mw->ui->tabWidget->setCurrentIndex(groupId2TabIndex(gid));
        });
        rows->addWidget(row);
    }
    scroll->setWidget(list);
    const int listHeight = list->sizeHint().height();
    if (listHeight <= 0) scroll->hide();
    else scroll->setFixedHeight(std::min(listHeight, 480));
    layout->addWidget(scroll);

    const int width = std::max(m_chip->width(), list->sizeHint().width() + 32);
    popup->setFixedWidth(std::min(std::max(width, 220), 560));
    popup->adjustSize();
    const QPoint anchor = m_chip->mapToGlobal(m_chip->rect().center());
    const QRect avail = screenAvailableAt(anchor);
    QPoint origin = m_chip->mapToGlobal(QPoint(0, m_chip->height() + 8));
    if (origin.x() + popup->width() > avail.right() - 8) origin.setX(avail.right() - popup->width() - 8);
    if (origin.x() < avail.left() + 8) origin.setX(avail.left() + 8);
    if (origin.y() + popup->height() > avail.bottom() - 8) {
        const int above = m_chip->mapToGlobal(QPoint(0, 0)).y() - popup->height() - 8;
        origin.setY(above >= avail.top() + 8 ? above : std::max(avail.top() + 8, avail.bottom() - popup->height() - 8));
    }
    bindPopupToScreen(popup, anchor);
    popup->move(origin);
    popup->show();
    popup->move(origin);
}

void MikanHome::showAppMenu(QWidget *anchor) {
    // Everything the classic menu bar held, in one sheet. Groups and servers stay one level down.
    auto *menu = new QMenu(anchor);
    auto *ui = mw->ui;
    ui->menu_spmode->setTitle(T("Service mode"));
    menu->addMenu(ui->menu_spmode);
    menu->addMenu(ui->menuRouting_Menu);
    menu->addMenu(ui->menuTesting);
    menu->addMenu(ui->menu_server);
    menu->addMenu(ui->menuTools);
    menu->addSeparator();
    menu->addMenu(ui->menu_preferences);
    menu->addMenu(ui->menu_program);
    if (ui->actionCheck_For_Update->isEnabled()) menu->addAction(ui->actionCheck_For_Update);
    openSheet(anchor, menu, true, true);
}

void MikanHome::showSubscriptionMenu() {
    auto *menu = new QMenu(m_more);
    menu->setTitle(T("Subscription"));
    auto *ui = mw->ui;
    ui->actionDelete_Group->setEnabled(Configs::dataManager->groupsRepo->GetAllGroupIds().size() > 1);
    menu->addAction(ui->actionEdit_Group);
    menu->addAction(ui->menu_update_subscription);
    menu->addAction(ui->actionUpdate_All_Subscriptions);
    menu->addSeparator();
    menu->addAction(ui->actionAdd_New_Group);
    menu->addAction(ui->menu_manage_groups);
    menu->addAction(ui->actionDelete_Group);
    menu->addSeparator();
    menu->addMenu(ui->menu_clean_up);
    menu->addAction(ui->menu_resolve_domain);
    menu->addAction(ui->actionResolve_Out_IP);
    menu->addAction(ui->menu_clear_test_result);
    openSheet(m_more, menu, true, false);
}

void MikanHome::showAddMenu() {
    auto *menu = new QMenu(m_add);
    auto *ui = mw->ui;
    menu->addAction(ui->menu_add_from_clipboard);
    menu->addAction(ui->menu_add_from_input);
    menu->addAction(ui->actionAdd_profile_from_File);
    menu->addAction(ui->menu_scan_qr);
    menu->addSeparator();
    menu->addAction(ui->actionAdd_New_Group);
    openSheet(m_add, menu, true, false);
}

void MikanHome::showGroupMenu() {
    auto *menu = new QMenu(m_groupMore);
    auto *ui = mw->ui;
    const auto group = Configs::dataManager->groupsRepo->CurrentGroup();
    if (!group) { delete menu; return; }
    // The sort the classic table's column headers offer.
    auto *sort = menu->addMenu(T("Sort"));
    const QList<std::pair<QString, GroupSortMethod::GroupSortMethod>> methods{
        {T("By name"), GroupSortMethod::ByName}, {T("By type"), GroupSortMethod::ByType},
        {T("By address"), GroupSortMethod::ByAddress}, {T("By latency"), GroupSortMethod::ByLatency},
        {T("By test result"), GroupSortMethod::ByTestResult}, {T("By traffic"), GroupSortMethod::ByTraffic}};
    for (const auto &[text, method] : methods) {
        auto *item = sort->addAction(text);
        item->setCheckable(true);
        item->setChecked(group->sort.method == method);
        const auto chosen = method;
        connect(item, &QAction::triggered, this, [this, chosen] {
            const auto current = Configs::dataManager->groupsRepo->CurrentGroup();
            if (!current) return;
            GroupSortAction action{chosen, chosen == GroupSortMethod::ByTraffic || chosen == GroupSortMethod::ByTestResult};
            if (current->sort.method == chosen) action.descending = !current->sort.descending;
            mw->sortCurrentGroup(action);
        });
    }
    if (group->KeepsSorted()) {
        sort->addSeparator();
        connect(sort->addAction(T("Stop sorting")), &QAction::triggered, this, [this] { mw->stopSortingCurrentGroup(); });
    }
    menu->addSeparator();
    emit ui->menuTesting->aboutToShow();
    for (auto *action : ui->menuTesting->actions()) {
        if (action == ui->menu_stop_testing) continue;
        menu->addAction(action);
    }
    if (Engine::host()->tests()->isRunning()) menu->addAction(ui->menu_stop_testing);
    openSheet(m_groupMore, menu, true, true);
}

void MikanHome::paintEvent(QPaintEvent *) {
    QPainter p(this);
    Mikan::paintBackdrop(p, rect());
}

void MikanHome::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    placeDrawer();
}

void MikanHome::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    placeDrawer();
}

Qt::Edges MikanHome::edgesAt(const QPoint &pos) const {
    if (window()->isMaximized() || window()->isFullScreen()) return {};
    const int grip = 6;
    Qt::Edges edges;
    if (pos.x() <= grip) edges |= Qt::LeftEdge;
    if (pos.x() >= width() - grip) edges |= Qt::RightEdge;
    if (pos.y() <= grip) edges |= Qt::TopEdge;
    if (pos.y() >= height() - grip) edges |= Qt::BottomEdge;
    return edges;
}

void MikanHome::mouseMoveEvent(QMouseEvent *e) {
    const Qt::Edges edges = edgesAt(e->pos());
    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) setCursor(Qt::SizeFDiagCursor);
    else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) setCursor(Qt::SizeBDiagCursor);
    else if (edges & (Qt::LeftEdge | Qt::RightEdge)) setCursor(Qt::SizeHorCursor);
    else if (edges & (Qt::TopEdge | Qt::BottomEdge)) setCursor(Qt::SizeVerCursor);
    else unsetCursor();
    QWidget::mouseMoveEvent(e);
}

void MikanHome::mousePressEvent(QMouseEvent *e) {
    const Qt::Edges edges = edgesAt(e->pos());
    if (e->button() == Qt::LeftButton && window()->windowHandle()) {
        if (edges) window()->windowHandle()->startSystemResize(edges);
        // The empty margins move the window like the title bar.
        else if (e->pos().y() < 60) window()->windowHandle()->startSystemMove();
    }
    QWidget::mousePressEvent(e);
}

void MikanHome::leaveEvent(QEvent *e) {
    unsetCursor();
    QWidget::leaveEvent(e);
}

bool MikanHome::eventFilter(QObject *watched, QEvent *event) {
    // A label ignores the press, so its release would go to the parent: the press is the click.
    if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && m_latencyRow && (widget == m_latencyRow || widget->parentWidget() == m_latencyRow)) {
            if (mw->ui->toolButton_startstop->state() == StartStopButton::State::Running) checkCurrentConnection();
            return true;
        }
        if (watched == m_announce) { m_announceOpen = !m_announceOpen; refreshGroupHeader(); return true; }
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------- switching interfaces

#ifdef Q_OS_WIN
// Clearing FramelessWindowHint recreates the window, but Windows keeps the old non-client
// area when that happens from a maximized or just-closed window: the caption bits never land.
static void repairNativeCaption(QWidget *window) {
    if (!window->testAttribute(Qt::WA_WState_Created) || window->isFullScreen()) return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd) return;
    using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static const auto set = [] {
        const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    }();
    const DWORD preference = 0; // DWMWCP_DEFAULT. The frameless window asked for round corners; a caption needs the system frame.
    if (set) set(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &preference, sizeof preference);
    const LONG_PTR need = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & need) != need) SetWindowLongPtrW(hwnd, GWL_STYLE, style | need);
    // FRAMECHANGED on a minimized window can restore it. The style bits above are enough until it is.
    if (window->isMinimized()) return;
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
}
#endif

void MainWindow::applyMikanInterfaceStyle(bool animated) {
    const bool wantMikan = Mikan::active();
    const bool wantNew = !wantMikan && NewStyle::active();
    QLabel *cover = nullptr;
    // A change of look: the old picture stays on top until the new one is ready, then fades.
    if (animated && isVisible() && !isMinimized()) {
        cover = new QLabel(this);
        cover->setPixmap(grab());
        cover->setGeometry(rect());
        cover->show();
        cover->raise();
    }
    if (!rootStack) {
        rootStack = new QStackedWidget;
        QWidget *classic = takeCentralWidget();
        rootStack->addWidget(classic);
        setCentralWidget(rootStack);
    }
    if (wantMikan) {
        if (!mikanHome) {
            mikanHome = new MikanHome(this);
            rootStack->addWidget(mikanHome);
        }
        if (newHome) newHome->detach();
        mikanHome->attach();
        rootStack->setCurrentWidget(mikanHome);
        mikanHome->update();
    } else if (wantNew) {
        if (!newHome) {
            newHome = new NewHome(this);
            rootStack->addWidget(newHome);
        }
        if (mikanHome) mikanHome->detach();
        newHome->attach();
        rootStack->setCurrentWidget(newHome);
        newHome->update();
    } else {
        if (newHome) newHome->detach();
        if (mikanHome) mikanHome->detach();
        rootStack->setCurrentIndex(0);
    }
    const bool frameless = wantMikan || wantNew;
    const bool frameDiffers = bool(windowFlags() & Qt::FramelessWindowHint) != frameless;
    // Changing the frame re-creates the window and would hide an open dialog with it.
    // Doing it inside the dialog's finished() is still too early: the owner window is torn down
    // mid-hide and Windows drops the caption. Wait until that close has returned to the event loop.
    if (frameDiffers) {
        QList<QDialog *> dialogs;
        for (auto *dialog : findChildren<QDialog *>())
            if (dialog->isVisible()) dialogs.append(dialog);
        if (!dialogs.isEmpty()) {
            if (!m_frameChangeDeferred) {
                m_frameChangeDeferred = true;
                for (auto *dialog : dialogs) {
                    connect(dialog, &QDialog::finished, this, [this] {
                        QTimer::singleShot(0, this, [this] {
                            m_frameChangeDeferred = false;
                            applyMikanInterfaceStyle(false);
                        });
                    }, Qt::SingleShotConnection);
                }
            }
        } else {
            const bool shown = isVisible();
            const bool minimized = isMinimized();
            const bool maximized = isMaximized();
            const bool full = isFullScreen();
            // A maximized frameless window keeps its frame margins. Drop that state in Qt only —
            // the native window stays put until setWindowFlags recreates it — or the restored
            // caption is clipped off the top of the screen. A hidden window keeps its state for the next show.
            if (!frameless && shown && (minimized || maximized || full))
                overrideWindowState(Qt::WindowNoState);
            Qt::WindowFlags flags = windowFlags();
            flags.setFlag(Qt::FramelessWindowHint, frameless);
            if (!frameless) {
                flags.setFlag(Qt::CustomizeWindowHint, false);
                flags |= Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                    | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint;
            }
            setWindowFlags(flags);
            if (shown) {
                if (full) showFullScreen();
                else if (maximized) showMaximized();
                else show();
                // Minimized-from-maximized keeps both bits; showMinimized() does not clear them.
                if (minimized) showMinimized();
            }
            // After show(): a corner preference set on the pre-show handle is lost when the window is created.
            if (frameless) Mikan::roundCorners(this, false);
#ifdef Q_OS_WIN
            else if (shown) repairNativeCaption(this);
#endif
            if (!frameless) {
                // Recreating the window drops the theme that was just applied. Apply it again once
                // the new frame has settled. That restyle posts its own polish, so the caption is
                // put back on the turn after, once those events have run.
                QTimer::singleShot(0, this, [this] {
                    if (Mikan::active()) return;
                    const QString theme = themeManager()->current_theme;
                    if (!Mikan::isTheme(theme)) themeManager()->ApplyTheme(theme, true);
#ifdef Q_OS_WIN
                    QTimer::singleShot(0, this, [this] {
                        if (!Mikan::active() && !windowFlags().testFlag(Qt::FramelessWindowHint))
                            repairNativeCaption(this);
                    });
#endif
                });
            }
        }
    }
    if (cover) {
        cover->raise();
        auto *effect = new QGraphicsOpacityEffect(cover);
        cover->setGraphicsEffect(effect);
        auto *fade = new QVariantAnimation(cover);
        fade->setDuration(220);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        connect(fade, &QVariantAnimation::valueChanged, cover, [effect](const QVariant &v) { effect->setOpacity(v.toReal()); });
        connect(fade, &QVariantAnimation::finished, cover, &QObject::deleteLater);
        QTimer::singleShot(260, cover, [fade] { fade->start(); });
    }
}
