#include "include/ui/widget/NewHome.hpp"
#include "include/ui/widget/NewIcons.h"

#include "include/ui/mainwindow.h"
#include "include/ui/mainWindow/MainWindowInternal.h"
#include "include/ui/setting/ThemeManager.hpp"
#include "include/ui/utils/ProfilesTableModel.h"
#include "include/ui/widget/StartStopButton.hpp"
#include "include/database/GroupsRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/global/RunningProfiles.hpp"
#include "include/database/ProfilesRepo.h"
#include "include/engine/EngineHost.h"
#include "include/engine/connection/ConnectionManager.h"
#include "include/engine/services/StatsService.h"
#include "include/engine/tests/TestRunner.h"
#include "include/global/Utils.hpp"

#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QClipboard>
#include <QComboBox>
#include <QMenuBar>
#include <QStyledItemDelegate>
#include <QGroupBox>
#include <QProgressBar>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTabWidget>
#include <QSpinBox>
#include <QFocusEvent>
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
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QKeyEvent>
#include <QFocusFrame>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QRegularExpression>
#include <QRegion>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextBrowser>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <QWindow>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static QString T(const char *text) { return QCoreApplication::translate("NewHome", text); }

static int frameInterval(const QWidget *widget) {
    const auto rate = widget->screen() ? widget->screen()->refreshRate() : 60.0;
    return qMax(4, int(1000.0 / qMax(30.0, rate)));
}

// The mobile app's motion curve: PathInterpolator(.2, 0, 0, 1).
static QEasingCurve softCurve() {
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment(QPointF(.2, 0), QPointF(0, 1), QPointF(1, 1));
    return curve;
}

// ---------------------------------------------------------------- style

namespace {
    QString g_theme;
    QString g_family;

    QColor withAlpha(QColor c, int alpha) { c.setAlpha(alpha); return c; }
    QColor blend(const QColor &a, const QColor &b, qreal k) { return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * k, a.greenF() + (b.greenF() - a.greenF()) * k, a.blueF() + (b.blueF() - a.blueF()) * k); }

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

    const NewIconData *findIcon(const QString &name) {
        for (const auto &icon : newIcons) if (name == QLatin1String(icon.name)) return &icon;
        return nullptr;
    }

    // In the new style every control that acts gets the system's pointing hand; the classic one keeps the arrow.
    void giveHand(QWidget *widget) {
        // A list's own rows ignore the sheet and draw a square frame; styled rows follow it.
        if (auto *combo = qobject_cast<QComboBox *>(widget); combo && combo->itemDelegate()->inherits("QComboBoxDelegate")) {
            combo->setItemDelegate(new QStyledItemDelegate(combo));
        }
        if (widget->testAttribute(Qt::WA_SetCursor)) return;
        if (qobject_cast<QAbstractButton *>(widget) || qobject_cast<QComboBox *>(widget) || qobject_cast<QTabBar *>(widget) ||
            qobject_cast<QMenu *>(widget) || qobject_cast<QMenuBar *>(widget) || qobject_cast<QSlider *>(widget)) {
            widget->setCursor(Qt::PointingHandCursor);
            widget->setProperty("newHand", true);
        }
    }

    // A stylesheet draws a round border in pieces that do not join cleanly, so the sheet keeps the border clear
    // and each control gets its outline here, as one smooth stroke over what it painted.
    bool g_keyFocus = false;
    constexpr int menuRadius = 9;

    bool outlined(const QWidget *w) {
        return qobject_cast<const QPushButton *>(w) || qobject_cast<const QLineEdit *>(w) || qobject_cast<const QComboBox *>(w) ||
               qobject_cast<const QAbstractSpinBox *>(w) || qobject_cast<const QPlainTextEdit *>(w) || qobject_cast<const QTextEdit *>(w) ||
               qobject_cast<const QGroupBox *>(w) || qobject_cast<const QProgressBar *>(w) || qobject_cast<const QMenu *>(w) ||
               qobject_cast<const QTabBar *>(w) || qobject_cast<const QTabWidget *>(w);
    }

    void paintOutline(QWidget *w) {
        if (w->styleSheet().contains(QLatin1String("border"))) return;
        const QColor ink = NewStyle::text();
        const QColor rim = NewStyle::light() ? QColor(0, 0, 0, 22) : QColor(255, 255, 255, 26);
        const QColor lit = withAlpha(ink, 120);
        QPainter p(w);
        p.setRenderHint(QPainter::Antialiasing);
        if (!w->isEnabled()) p.setOpacity(.5);
        const auto stroke = [&p](QRectF r, qreal radius, const QColor &colour) {
            r.adjust(.5, .5, -.5, -.5);
            radius = std::min(radius, std::min(r.width(), r.height()) / 2);
            p.setPen(QPen(colour, 1.35));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, radius, radius);
        };
        const auto arrow = [&p, &ink](QPointF c, bool up) {
            const qreal d = up ? -1.75 : 1.75;
            p.setPen(QPen(withAlpha(ink, 200), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(QPolygonF{c + QPointF(-3.5, -d), c + QPointF(0, d), c + QPointF(3.5, -d)});
        };
        const auto *focus = QApplication::focusWidget();
        const QColor field = focus && (focus == w || w->isAncestorOf(focus)) ? lit : rim;
        const QRectF r = w->rect();
        if (auto *button = qobject_cast<QPushButton *>(w)) {
            if (!button->isFlat()) stroke(r, 14, button->isDefault() ? lit : rim);
        } else if (qobject_cast<QLineEdit *>(w)) {
            // The field inside a spin box or an editable list is part of that control.
            if (!qobject_cast<QAbstractSpinBox *>(w->parentWidget()) && !qobject_cast<QComboBox *>(w->parentWidget())) stroke(r, 12, field);
        } else if (qobject_cast<QComboBox *>(w)) {
            stroke(r, 12, field);
            arrow(QPointF(r.right() - 14, r.center().y()), false);
        } else if (auto *spin = qobject_cast<QAbstractSpinBox *>(w)) {
            stroke(r, 12, field);
            if (spin->buttonSymbols() != QAbstractSpinBox::NoButtons) {
                arrow(QPointF(r.right() - 13, r.center().y() - 5), true);
                arrow(QPointF(r.right() - 13, r.center().y() + 5), false);
            }
        } else if (qobject_cast<QPlainTextEdit *>(w) || qobject_cast<QTextEdit *>(w)) {
            stroke(r, 12, field);
        } else if (qobject_cast<QGroupBox *>(w)) {
            stroke(r.adjusted(0, 14, 0, 0), 18, rim);
        } else if (qobject_cast<QProgressBar *>(w)) {
            stroke(r, 8, rim);
        } else if (qobject_cast<QMenu *>(w)) {
            stroke(r.adjusted(1, 1, -1, -1), menuRadius - 1, NewStyle::outline());
        } else if (auto *bar = qobject_cast<QTabBar *>(w)) {
            for (int i = 0; i < bar->count(); ++i) {
                if (bar->isTabVisible(i)) stroke(QRectF(bar->tabRect(i)).adjusted(2, 2, -2, -2), 12, i == bar->currentIndex() ? withAlpha(ink, 90) : rim);
            }
        } else if (auto *tabs = qobject_cast<QTabWidget *>(w)) {
            if (tabs->documentMode() || tabs->objectName() == QLatin1String("stats_widget")) return;
            QRectF pane = r;
            if (const auto *strip = tabs->tabBar(); strip->isVisible()) {
                if (tabs->tabPosition() == QTabWidget::North) pane.setTop(strip->geometry().bottom() + 1);
                else if (tabs->tabPosition() == QTabWidget::South) pane.setBottom(strip->geometry().top());
                else return;
            }
            stroke(pane, 18, rim);
        }
    }

    void clipRoundedWindow(QWidget *window, int radius) {
        QPainterPath shape;
        shape.addRoundedRect(QRectF(window->rect()), radius, radius);
        window->setMask(QRegion(shape.toFillPolygon().toPolygon()));
    }

#ifdef Q_OS_WIN
    bool handleWindowsFrame(QWidget *window, const MSG *msg, qintptr *result) {
        if (msg->message == WM_STYLECHANGING && static_cast<int>(msg->wParam) == GWL_STYLE &&
            window->property("rawlineSavedCaptionWindow").toULongLong() == quint64(reinterpret_cast<quintptr>(msg->hwnd))) {
            // Qt can reapply the window style after the handle was configured.
            reinterpret_cast<STYLESTRUCT *>(msg->lParam)->styleNew &= ~DWORD(WS_CAPTION);
            return false;
        }
        if (msg->message == WM_NCCALCSIZE) {
            if (msg->wParam && IsZoomed(msg->hwnd)) {
                RECT &client = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam)->rgrc[0];
                MONITORINFO monitor{sizeof(MONITORINFO)};
                if (GetMonitorInfoW(MonitorFromRect(&client, MONITOR_DEFAULTTONEAREST), &monitor))
                    IntersectRect(&client, &client, &monitor.rcWork);
            }
            *result = 0;
            return true;
        }
        if (msg->message == WM_NCPAINT || msg->message == WM_NCACTIVATE) {
            *result = msg->message == WM_NCACTIVATE ? TRUE : 0;
            return true;
        }
        if (msg->message != WM_NCHITTEST) return false;
        *result = HTCLIENT;
        if (window->isMaximized() || window->isFullScreen() || window->minimumSize() == window->maximumSize()) return true;
        const POINT point{short(LOWORD(msg->lParam)), short(HIWORD(msg->lParam))};
        RECT bounds{};
        if (GetWindowRect(msg->hwnd, &bounds)) {
            const int edge = qRound(6 * window->devicePixelRatioF());
            const bool left = point.x < bounds.left + edge, right = point.x >= bounds.right - edge;
            const bool top = point.y < bounds.top + edge, bottom = point.y >= bounds.bottom - edge;
            if (top) *result = left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
            else if (bottom) *result = left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
            else if (left || right) *result = left ? HTLEFT : HTRIGHT;
        }
        return true;
    }

    void recalculateWindowsFrame(QWidget *window) {
        const auto id = window->internalWinId();
        if (!id) return;
        const auto hwnd = reinterpret_cast<HWND>(id);
        const auto oldStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        auto newStyle = oldStyle;
        constexpr LONG_PTR captionMask = WS_CAPTION;
        const auto savedWindow = window->property("rawlineSavedCaptionWindow").toULongLong();
        if (NewStyle::active()) {
            if (savedWindow != quint64(id)) {
                window->setProperty("rawlineSavedCaptionWindow", quint64(id));
                window->setProperty("rawlineSavedCaptionBits", quint32(oldStyle & captionMask));
            }
            // DefWindowProc can draw WS_CAPTION on title/activation changes without sending WM_NCPAINT.
            newStyle &= ~captionMask;
        } else if (savedWindow == quint64(id)) {
            newStyle |= window->property("rawlineSavedCaptionBits").toUInt();
            window->setProperty("rawlineSavedCaptionWindow", QVariant());
            window->setProperty("rawlineSavedCaptionBits", QVariant());
        }
        if (newStyle != oldStyle) SetWindowLongPtrW(hwnd, GWL_STYLE, newStyle);
        if (NewStyle::active() || savedWindow == quint64(id)) {
            using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
            static const auto set = [] {
                const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
                return dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
            }();
            // The same HWND retains its former rendering mode across a style change.
            // Explicitly give the system caption back to DWM before recalculating the frame.
            const DWORD policy = NewStyle::active() ? 1 /* DWMNCRP_DISABLED */ : 2 /* DWMNCRP_ENABLED */;
            if (set) set(hwnd, 2 /* DWMWA_NCRENDERING_POLICY */, &policy, sizeof policy);
        }
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        NewStyle::themeFrame(window);
    }

    void clipNativeFrame(QWidget *window, HWND hwnd, bool themed) {
        const auto previous = window->property("rawlineNativeFrameRegion").toRect();
        if (!themed || window->isMaximized() || window->isFullScreen()) {
            if (previous.isValid()) {
                window->setProperty("rawlineNativeFrameRegion", QVariant());
                if (!SetWindowRgn(hwnd, nullptr, TRUE)) window->setProperty("rawlineNativeFrameRegion", previous);
            }
            return;
        }
        if (window->isMinimized()) return;
        RECT outer{};
        if (!GetWindowRect(hwnd, &outer)) return;
        RECT visible = outer;
        using GetAttribute = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
        static const auto get = [] {
            const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
            return dwm ? reinterpret_cast<GetAttribute>(GetProcAddress(dwm, "DwmGetWindowAttribute")) : nullptr;
        }();
        RECT frame{};
        if (!qobject_cast<MainWindow *>(window) && !window->property("newFrame").toBool() &&
            get && SUCCEEDED(get(hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &frame, sizeof frame)) &&
            frame.left >= outer.left && frame.top >= outer.top && frame.right <= outer.right && frame.bottom <= outer.bottom &&
            frame.left < frame.right && frame.top < frame.bottom) visible = frame;
        const QRect bounds(visible.left - outer.left, visible.top - outer.top, visible.right - visible.left, visible.bottom - visible.top);
        const int diameter = qRound(14 * window->devicePixelRatioF());
        if (!bounds.isValid() || (previous == bounds && window->property("rawlineNativeFrameDiameter").toInt() == diameter)) return;
        const HRGN shape = CreateRoundRectRgn(bounds.x(), bounds.y(), bounds.x() + bounds.width(), bounds.y() + bounds.height(), diameter, diameter);
        if (!shape) return;
        window->setProperty("rawlineNativeFrameRegion", bounds);
        window->setProperty("rawlineNativeFrameDiameter", diameter);
        if (!SetWindowRgn(hwnd, shape, TRUE)) {
            DeleteObject(shape);
            window->setProperty("rawlineNativeFrameRegion", previous);
            window->setProperty("rawlineNativeFrameDiameter", QVariant());
        }
    }
#endif

    // A dialog in the new style is drawn like the main window: no system frame, our own title bar, a round body.
    // Windows 10 never rounds a framed window, so the corners come only from painting them.
    constexpr int kDialogBar = 36, kDialogRadius = 14, kDialogGrip = 6;

    void frameDialog(QDialog *dialog) {
        if (dialog->property("newFrame").toBool()) return;
        const bool shown = dialog->isVisible();
        dialog->setProperty("newFrame", true);
#ifndef Q_OS_WIN
        dialog->setProperty("newWindowFlags", QVariant::fromValue(dialog->windowFlags()));
        dialog->setWindowFlags(dialog->windowFlags() | Qt::FramelessWindowHint);
        // See-through, so only the round body shows; this also stops the sheet's square background.
        dialog->setAttribute(Qt::WA_TranslucentBackground);
        dialog->setMouseTracking(true);
#endif
        const QMargins margins = dialog->contentsMargins();
        dialog->setContentsMargins(margins.left(), margins.top() + kDialogBar, margins.right(), margins.bottom());
        auto *bar = new NewTitleBar(dialog);
        bar->setObjectName(QStringLiteral("newDialogBar"));
        // Keep maximise available for long forms and update notes.
        const auto buttons = bar->findChildren<QAbstractButton *>();
        if (!buttons.isEmpty()) buttons.first()->hide();
        bar->follow(dialog);
        if (shown) {
#ifdef Q_OS_WIN
            recalculateWindowsFrame(dialog);
            bar->show();
#else
            dialog->show();
#endif
        }
    }

    Qt::Edges dialogEdges(QWidget *dialog, const QPoint &pos) {
        if (dialog->minimumSize() == dialog->maximumSize() || dialog->isMaximized()) return {};
        Qt::Edges edges;
        if (pos.x() <= kDialogGrip) edges |= Qt::LeftEdge;
        if (pos.x() >= dialog->width() - kDialogGrip) edges |= Qt::RightEdge;
        if (pos.y() <= kDialogGrip) edges |= Qt::TopEdge;
        if (pos.y() >= dialog->height() - kDialogGrip) edges |= Qt::BottomEdge;
        return edges;
    }

    void paintDialogBody(QWidget *dialog) {
        QPainter p(dialog);
#ifdef Q_OS_WIN
        p.fillRect(dialog->rect(), NewStyle::background());
#else
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(NewStyle::light() ? QColor(0, 0, 0, 40) : QColor(255, 255, 255, 40), 1));
        p.setBrush(NewStyle::background());
        p.drawRoundedRect(QRectF(dialog->rect()).adjusted(.5, .5, -.5, -.5), kDialogRadius, kDialogRadius);
#endif
    }

    // A menu gets round corners, which a stylesheet radius only shows on a window without a background of its own;
    // a list popup and a tooltip get the system's round corners.
    class MenuCorners : public QObject
#ifdef Q_OS_WIN
        , public QAbstractNativeEventFilter
#endif
    {
    public:
        using QObject::QObject;
#ifdef Q_OS_WIN
        bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override {
            if (!NewStyle::active() || eventType != "windows_generic_MSG") return false;
            const auto *msg = static_cast<const MSG *>(message);
            if (msg->message != WM_NCCALCSIZE && msg->message != WM_NCPAINT &&
                msg->message != WM_NCACTIVATE && msg->message != WM_NCHITTEST && msg->message != WM_STYLECHANGING) return false;
            auto *window = QWidget::find(reinterpret_cast<WId>(msg->hwnd));
            return window && window->property("newFrame").toBool() && handleWindowsFrame(window, msg, result);
        }
#endif
    protected:
        bool eventFilter(QObject *watched, QEvent *event) override {
            if (!watched->isWidgetType()) return false;
            auto *widget = static_cast<QWidget *>(watched);
            if (event->type() == QEvent::Show) NewStyle::themeFrame(widget);
#ifdef Q_OS_WIN
            const auto type = event->type();
            if (widget->isWindow() && (type == QEvent::Resize || type == QEvent::WindowStateChange ||
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
                type == QEvent::DevicePixelRatioChange ||
#endif
                type == QEvent::WinIdChange)) {
                if (event->type() == QEvent::WinIdChange) widget->setProperty("rawlineNativeFrameRegion", QVariant());
                if (widget->isVisible() && widget->internalWinId() &&
                    (widget->windowFlags() & Qt::WindowTitleHint) && !(widget->windowFlags() & Qt::FramelessWindowHint))
                    clipNativeFrame(widget, reinterpret_cast<HWND>(widget->internalWinId()), NewStyle::active());
            }
#endif
            if (!NewStyle::active()) return false;
            if ((event->type() == QEvent::Show || event->type() == QEvent::Resize) && widget->isWindow() && qobject_cast<QMenu *>(widget)) {
                // QMenu sizes to its actions after its initial window events.
                QTimer::singleShot(0, widget, [widget] {
                    if (NewStyle::active() && widget->isVisible()) clipRoundedWindow(widget, menuRadius);
                });
            }
            if (widget->property("newFrame").toBool()) {
                // The framed dialog's body, its title bar's place and its edges for resizing.
                if (event->type() == QEvent::Paint && !m_painting) {
                    paintDialogBody(widget);
                    m_painting = true;
                    watched->event(event);
                    m_painting = false;
                    return true;
                }
                if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
#ifdef Q_OS_WIN
                    if (event->type() == QEvent::Show) recalculateWindowsFrame(widget);
#else
                    NewStyle::roundCorners(widget, false);
#endif
                    if (auto *bar = widget->findChild<QWidget *>(QStringLiteral("newDialogBar"), Qt::FindDirectChildrenOnly)) {
                        bar->setGeometry(10, 0, widget->width() - 16, kDialogBar);
                        bar->raise();
                    }
                }
                if (event->type() == QEvent::MouseMove) {
                    const Qt::Edges edges = dialogEdges(widget, static_cast<QMouseEvent *>(event)->position().toPoint());
                    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) widget->setCursor(Qt::SizeFDiagCursor);
                    else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) widget->setCursor(Qt::SizeBDiagCursor);
                    else if (edges & (Qt::LeftEdge | Qt::RightEdge)) widget->setCursor(Qt::SizeHorCursor);
                    else if (edges) widget->setCursor(Qt::SizeVerCursor);
                    else widget->unsetCursor();
                }
                if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                    const Qt::Edges edges = dialogEdges(widget, static_cast<QMouseEvent *>(event)->position().toPoint());
                    if (edges && widget->windowHandle()) { widget->windowHandle()->startSystemResize(edges); return true; }
                }
            }
            if (event->type() == QEvent::Paint) {
                if (m_painting || !outlined(widget)) return false;
                if (widget->isWindow() && qobject_cast<QMenu *>(widget) && widget->mask().boundingRect() != widget->rect())
                    clipRoundedWindow(widget, menuRadius);
                // The control paints itself first; the outline goes over it within the same paint.
                m_painting = true;
                watched->event(event);
                paintOutline(widget);
                m_painting = false;
                return true;
            }
            if (event->type() == QEvent::MouseButtonPress) {
                g_keyFocus = false;
                if (auto *focused = QApplication::focusWidget()) focused->update();
            } else if (event->type() == QEvent::FocusIn) {
                const auto reason = static_cast<QFocusEvent *>(event)->reason();
                g_keyFocus = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
            } else if (event->type() == QEvent::Polish) {
                giveHand(widget);
                if (auto *dialog = qobject_cast<QDialog *>(watched); dialog && dialog->isWindow()) frameDialog(dialog);
            } else if (event->type() == QEvent::Show && widget->isWindow() &&
                       (widget->inherits("QComboBoxPrivateContainer") || widget->inherits("QTipLabel"))) {
                NewStyle::roundCorners(widget, true);
            } else if (auto *view = qobject_cast<QAbstractItemView *>(widget); view && event->type() == QEvent::Show && widget->isWindow()) {
                // A completer's list: the sheet makes views see-through, which a window of its own cannot be.
                const auto rgba = [](const QColor &c) { return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()); };
                const QString sheet = QStringLiteral(
                    "QAbstractItemView { background: %1; color: %2; border: none; padding: 4px; outline: 0; }"
                    "QAbstractItemView::item { min-height: 24px; padding: 2px 8px; border-radius: 8px; }"
                    "QAbstractItemView::item:hover { background: %3; }"
                    "QAbstractItemView::item:selected { background: %4; color: %2; }")
                    .arg(rgba(NewStyle::solid()), rgba(NewStyle::text()), rgba(NewStyle::overlay(22)), rgba(withAlpha(NewStyle::accent(), NewStyle::pastel() ? 70 : 40)));
                if (view->styleSheet() != sheet) view->setStyleSheet(sheet);
                view->setProperty("newPopup", true);
                NewStyle::roundCorners(widget, true);
            }
            return false;
        }
    private:
        bool m_painting = false;
    };

    // A second press on the button that opened a popup closes it, rather than closing and opening it again.
    class ReopenGuard : public QObject {
    public:
        ReopenGuard(QWidget *popup, QWidget *anchor) : QObject(popup), anchor(anchor) {
            setObjectName(QStringLiteral("newReopenGuard"));
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
        if (auto *guard = popup->findChild<QObject *>(QStringLiteral("newReopenGuard"), Qt::FindDirectChildrenOnly)) static_cast<ReopenGuard *>(guard)->anchor = anchor;
        else new ReopenGuard(popup, anchor);
    }

    // Fusion under the new palette, with check boxes, radio buttons and menu ticks drawn smooth in the theme's ink.
    class NewProxyStyle : public QProxyStyle {
    public:
        NewProxyStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}
        void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *p, const QWidget *widget) const override {
            const bool on = option->state & State_On, partial = option->state & State_NoChange;
            const bool hover = option->state & State_MouseOver;
            const QColor ink = NewStyle::text();
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
                    p->setPen(QPen(on ? ink : withAlpha(NewStyle::muted(), hover ? 235 : 175), 1.3));
                    p->setBrush(hover ? NewStyle::overlay(16) : QColor(Qt::transparent));
                }
                if (radio) p->drawEllipse(box);
                else p->drawRoundedRect(box, box.width() * .3, box.width() * .3);
                const qreal x = box.left(), y = box.top(), s = box.width();
                if (radio && on) {
                    p->setPen(Qt::NoPen);
                    p->setBrush(ink);
                    p->drawEllipse(box.center(), s * .26, s * .26);
                } else if (on) {
                    p->setPen(QPen(NewStyle::background(), std::max<qreal>(1.4, s * .13), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    p->setBrush(Qt::NoBrush);
                    p->drawPolyline(QPolygonF{QPointF(x + s * .26, y + s * .53), QPointF(x + s * .43, y + s * .70), QPointF(x + s * .75, y + s * .33)});
                } else if (partial) {
                    p->setPen(QPen(NewStyle::background(), std::max<qreal>(1.4, s * .13), Qt::SolidLine, Qt::RoundCap));
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
            case PE_IndicatorTabClose: {
                // A thin cross in the theme's ink, with a soft round light under the pointer, not the stock red square.
                const qreal side = std::min(option->rect.width(), option->rect.height());
                QRectF box(0, 0, side, side);
                box.moveCenter(QRectF(option->rect).center());
                const bool lit = option->state & (State_Raised | State_Sunken);
                p->save();
                p->setRenderHint(QPainter::Antialiasing);
                if (!(option->state & State_Enabled)) p->setOpacity(.45);
                if (lit) {
                    p->setPen(Qt::NoPen);
                    p->setBrush(NewStyle::overlay(option->state & State_Sunken ? 44 : 24));
                    p->drawEllipse(box.adjusted(.5, .5, -.5, -.5));
                }
                const qreal arm = side * .2;
                const QPointF c = box.center();
                p->setPen(QPen(lit ? ink : NewStyle::muted(), 1.4, Qt::SolidLine, Qt::RoundCap));
                p->drawLine(c + QPointF(-arm, -arm), c + QPointF(arm, arm));
                p->drawLine(c + QPointF(arm, -arm), c + QPointF(-arm, arm));
                p->restore();
                return;
            }
            default:
                QProxyStyle::drawPrimitive(element, option, p, widget);
            }
        }
    };
}

QStyle *NewStyle::createStyle() { return new NewProxyStyle; }

void NewStyle::roundCorners(QWidget *window, bool compact) {
    if (window && (window->windowFlags() & Qt::FramelessWindowHint)) {
        if (window->isMaximized() || window->isFullScreen()) window->clearMask();
        else clipRoundedWindow(window, compact ? 10 : 14);
    }
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

void NewStyle::themeFrame(QWidget *window) {
#ifdef Q_OS_WIN
    using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static const auto set = [] {
        const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    }();
    if (!window || !window->isWindow() || !window->internalWinId() ||
        !(window->windowFlags() & Qt::WindowTitleHint) || (window->windowFlags() & Qt::FramelessWindowHint)) return;
    const HWND hwnd = reinterpret_cast<HWND>(window->internalWinId());
    const bool themed = active();
    clipNativeFrame(window, hwnd, themed);
    if (!set) return;
    const BOOL dark = themed ? !light() : qApp->palette().color(QPalette::Window).lightness() < 128;
    set(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    // Windows 11 paints the caption in the theme's own ground and ink; older systems ignore these.
    const auto ref = [](const QColor &c) { return COLORREF(RGB(c.red(), c.green(), c.blue())); };
    constexpr COLORREF useDefault = 0xffffffff;
    const COLORREF caption = themed ? ref(background()) : useDefault;
    const COLORREF ink = themed ? ref(text()) : useDefault;
    constexpr COLORREF noBorder = 0xfffffffe; // DWMWA_COLOR_NONE
    const COLORREF border = themed ? noBorder : useDefault;
    set(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    set(hwnd, 36 /* DWMWA_TEXT_COLOR */, &ink, sizeof ink);
    set(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
    const DWORD cornerPreference = 0; // DWMWCP_DEFAULT
    set(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPreference, sizeof cornerPreference);
#else
    Q_UNUSED(window)
#endif
}

bool NewStyle::isTheme(const QString &theme) {
    return theme == QLatin1String(OledTheme) || theme == QLatin1String(WhiteTheme) ||
           theme == QLatin1String(PastelTheme) || theme == QLatin1String(PastelLightTheme) ||
           theme == QLatin1String(GreyTheme);
}

void NewStyle::setTheme(const QString &theme) {
    static MenuCorners *corners = nullptr;
    if (!corners) {
        corners = new MenuCorners(qApp);
        qApp->installEventFilter(corners);
#ifdef Q_OS_WIN
        qApp->installNativeEventFilter(corners);
#endif
    }
    const bool leaving = isTheme(g_theme) && !isTheme(theme);
    const bool entering = !isTheme(g_theme) && isTheme(theme);
    // Restoring a visible dialog sends Show synchronously; it must already see the incoming theme.
    g_theme = theme;
    if (leaving) {
        // Back in the classic look: menus get their own background again, and every control its own cursor.
        for (auto *widget : QApplication::allWidgets()) {
            if (auto *menu = qobject_cast<QMenu *>(widget)) menu->clearMask();
            if (widget->property("newHand").toBool()) { widget->unsetCursor(); widget->setProperty("newHand", QVariant()); }
            if (widget->property("newPopup").toBool()) { widget->setStyleSheet(QString()); widget->setProperty("newPopup", QVariant()); }
            // A dialog kept between uses gets its system frame back.
            if (widget->property("newFrame").toBool()) {
                widget->setProperty("newFrame", QVariant());
                if (auto *bar = widget->findChild<QWidget *>(QStringLiteral("newDialogBar"), Qt::FindDirectChildrenOnly)) {
                    bar->hide();
                    bar->setObjectName(QString());
                    // allWidgets() still holds the title bar and its children during this traversal.
                    bar->deleteLater();
                }
                const QMargins m = widget->contentsMargins();
                widget->setContentsMargins(m.left(), std::max(0, m.top() - kDialogBar), m.right(), m.bottom());
#ifdef Q_OS_WIN
                recalculateWindowsFrame(widget);
#else
                widget->setAttribute(Qt::WA_TranslucentBackground, false);
                widget->setAttribute(Qt::WA_NoSystemBackground, false);
                widget->clearMask();
                const bool shown = widget->isVisible();
                widget->setWindowFlags(widget->property("newWindowFlags").value<Qt::WindowFlags>());
                widget->setProperty("newWindowFlags", QVariant());
                if (shown) widget->show();
#endif
            }
        }
    }
    // Controls made before the switch are polished already.
    if (entering) for (auto *widget : QApplication::allWidgets()) {
        giveHand(widget);
        if (auto *dialog = qobject_cast<QDialog *>(widget); dialog && dialog->isWindow()) frameDialog(dialog);
    }
}

bool NewStyle::active() { return isTheme(g_theme); }
bool NewStyle::light() { return g_theme == QLatin1String(WhiteTheme) || g_theme == QLatin1String(PastelLightTheme); }
bool NewStyle::pastel() { return g_theme == QLatin1String(PastelTheme) || g_theme == QLatin1String(PastelLightTheme); }

static bool greyPalette() { return g_theme == QLatin1String(NewStyle::GreyTheme); }

QColor NewStyle::background() {
    if (greyPalette()) return QColor(35, 37, 41);
    if (pastel()) return light() ? QColor(255, 228, 214) : QColor(22, 20, 31);
    return light() ? QColor(255, 255, 255) : QColor(0, 0, 0);
}
QColor NewStyle::text() {
    if (greyPalette()) return QColor(241, 242, 244);
    if (pastel()) return light() ? QColor(27, 28, 42) : QColor(244, 242, 250);
    return light() ? QColor(10, 10, 11) : QColor(245, 245, 247);
}
QColor NewStyle::muted() {
    if (greyPalette()) return QColor(173, 177, 184);
    if (pastel()) return light() ? QColor(110, 108, 136) : QColor(176, 170, 198);
    return light() ? QColor(105, 105, 112) : QColor(142, 142, 150);
}
QColor NewStyle::solid() {
    if (greyPalette()) return QColor(45, 48, 53);
    if (pastel()) return light() ? QColor(255, 247, 252) : QColor(36, 31, 51);
    return light() ? QColor(243, 243, 246) : QColor(21, 21, 24);
}
QColor NewStyle::accent() { return pastel() ? QColor(124, 107, 242) : text(); }
QColor NewStyle::outline() { return withAlpha(muted(), 55); }
QColor NewStyle::overlay(int alpha) { return light() ? QColor(0, 0, 0, alpha / 2) : QColor(255, 255, 255, alpha); }

QString NewStyle::fontFamily() {
    if (g_family.isEmpty()) {
        for (const auto *weight : {"regular", "medium", "bold"}) {
            const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/newstyle/font/geologica_%1.ttf").arg(QLatin1String(weight)));
            if (id >= 0 && g_family.isEmpty()) g_family = QFontDatabase::applicationFontFamilies(id).value(0);
        }
        if (g_family.isEmpty()) g_family = qApp->font().family();
    }
    return g_family;
}

QFont NewStyle::font(qreal px, int weight) {
    const auto &chosen = Configs::dataManager->settingsRepo->font;
    QFont f(chosen.isEmpty() ? fontFamily() : chosen);
    f.setPixelSize(qRound(px));
    f.setWeight(QFont::Weight(weight));
    return f;
}

QPalette NewStyle::palette() {
    const QColor bg = background(), ink = text(), dim = muted();
    const QColor highlight = pastel() ? (light() ? blend(solid(), accent(), .15) : QColor(72, 60, 120))
                                      : light() ? QColor(222, 222, 228) : QColor(52, 52, 58);
    const QColor link = pastel() ? (light() ? QColor(82, 65, 168) : QColor(170, 158, 255))
                                 : light() ? QColor(30, 100, 200) : QColor(120, 180, 255);
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
        p.setColor(group, QPalette::Highlight, highlight);
        p.setColor(group, QPalette::HighlightedText, ink);
        p.setColor(group, QPalette::Link, link);
        p.setColor(group, QPalette::LinkVisited, link);
        p.setColor(group, QPalette::PlaceholderText, dim);
        p.setColor(group, QPalette::Light, solid().lighter(130));
        p.setColor(group, QPalette::Midlight, solid().lighter(115));
        p.setColor(group, QPalette::Mid, outline());
        p.setColor(group, QPalette::Dark, dim);
        p.setColor(group, QPalette::Shadow, bg);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        p.setColor(group, QPalette::Accent, accent());
#endif
    }
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::HighlightedText}) {
        p.setColor(QPalette::Disabled, role, withAlpha(dim, 160));
    }
    return p;
}

QString NewStyle::styleSheet() {
    const auto rgba = [](const QColor &c) {
        return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
    };
    const bool day = light();
    const QColor ink = text(), dim = muted(), line = outline();
    // Pastel's rows are plum plates, as on the phone; the others are glass.
    const QColor glassTop = pastel() && !day ? QColor(46, 39, 68, 240) : day ? QColor(255, 255, 255, 205) : QColor(255, 255, 255, 15);
    const QColor glassBottom = pastel() && !day ? QColor(40, 34, 60, 235) : day ? QColor(248, 248, 250, 160) : QColor(255, 255, 255, 9);
    const QColor well = day ? QColor(0, 0, 0, 10) : QColor(255, 255, 255, 14);
    const QColor hover = overlay(22), press = overlay(44);
    const QColor tint = withAlpha(accent(), pastel() ? 70 : day ? 30 : 40), tintRim = withAlpha(accent(), pastel() ? 200 : 90);
    const QColor menu = greyPalette() ? withAlpha(solid(), 250)
        : pastel() && !day ? QColor(30, 26, 44, 250)
        : day ? QColor(250, 250, 252, 250) : QColor(14, 14, 16, 250);
    const QString glass = QStringLiteral("qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %1,stop:1 %2)").arg(rgba(glassTop), rgba(glassBottom));
    return QStringLiteral(
        "QMainWindow, QDialog { background: %1; }\n"
        "QDialog[newFrame=\"true\"] { background: transparent; }\n"
        "QToolTip { background: %2; color: %3; border: 1px solid %4; padding: 6px 10px; }\n"
        "QMenu { background: %5; border: 1px solid transparent; padding: 6px; }\n"
        "QMenu::item { padding: 7px 26px 7px 14px; border-radius: 12px; background: transparent; color: %3; }\n"
        "QMenu::item:selected { background: %6; }\n"
        "QMenu::item:disabled { color: %7; }\n"
        "QMenu::separator { height: 1px; background: %4; margin: 5px 10px; }\n"
        "QMenu::icon { padding-left: 6px; }\n"
        "QPushButton { background: %8; border: 1px solid transparent; border-radius: 14px; padding: 7px 16px; color: %3; }\n"
        "QPushButton:hover { background: %6; }\n"
        "QPushButton:pressed { background: %9; }\n"
        "QPushButton:disabled { color: %7; }\n"
        "QToolButton { background: transparent; border: none; border-radius: 10px; padding: 4px; }\n"
        "QToolButton:hover { background: %6; }\n"
        "QToolButton:checked, QToolButton:pressed { background: %9; }\n"
        "QLineEdit, QPlainTextEdit, QTextEdit, QAbstractSpinBox, QComboBox, QKeySequenceEdit { background: %10; border: 1px solid transparent; border-radius: 12px; padding: 5px 10px; selection-background-color: %11; selection-color: %3; }\n"
        "QAbstractSpinBox QLineEdit, QComboBox QLineEdit { background: transparent; border: none; border-radius: 0; padding: 0; }\n"
        "QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { border: none; background: transparent; width: 20px; }\n"
        "QAbstractSpinBox::up-arrow, QAbstractSpinBox::down-arrow, QComboBox::down-arrow { image: none; width: 0; height: 0; }\n"
        "QComboBox::drop-down { border: none; width: 22px; }\n"
        "QComboBox QAbstractItemView { background: %2; border: none; selection-background-color: %6; outline: 0; padding: 4px; }\n"
        "QComboBox QAbstractItemView::item { min-height: 26px; padding: 2px 8px; border-radius: 8px; border: none; color: %3; }\n"
        "QComboBox QAbstractItemView::item:hover { background: %6; }\n"
        "QComboBox QAbstractItemView::item:selected { background: %11; color: %3; }\n"
        "QTabWidget::pane { border: 1px solid transparent; border-radius: 18px; background: transparent; }\n"
        "QTabBar::tab { background: transparent; border: 1px solid transparent; border-radius: 12px; padding: 5px 11px; margin: 2px 2px; color: %7; }\n"
        "QTabBar::tab:selected { background: %11; color: %3; }\n"
        "QTabBar::tab:hover:!selected { background: %6; }\n"
        "QHeaderView { background: transparent; }\n"
        "QHeaderView::section { background: transparent; border: none; border-bottom: 1px solid %4; padding: 6px; color: %7; }\n"
        "QTableView, QTreeView, QListView, QListWidget, QTableWidget, QTreeWidget { background: transparent; border: none; alternate-background-color: %10; selection-background-color: %11; selection-color: %3; gridline-color: %4; outline: 0; }\n"
        "QScrollArea { background: transparent; border: none; }\n"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }\n"
        "QScrollBar::handle:vertical { background: %13; border-radius: 3px; min-height: 30px; margin: 0 2px; }\n"
        "QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }\n"
        "QScrollBar::handle:horizontal { background: %13; border-radius: 3px; min-width: 30px; margin: 2px 0; }\n"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }\n"
        "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }\n"
        "QGroupBox { border: 1px solid transparent; border-radius: 18px; margin-top: 14px; padding-top: 8px; }\n"
        "QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 4px; color: %7; }\n"
        "QProgressBar { background: %10; border: 1px solid transparent; border-radius: 8px; text-align: center; }\n"
        "QProgressBar::chunk { background: %12; border-radius: 7px; }\n"
        "QSplitter::handle { background: transparent; }\n"
        "#stats_widget::pane { border: none; }\n"
        "QLabel[newRole=\"muted\"] { color: %7; }\n"
        "QLabel[newRole=\"link\"] { color: %7; }\n"
        "QLabel[newRole=\"link\"]:hover { color: %3; }\n"
    ).arg(rgba(background()), rgba(solid()), rgba(ink), rgba(line), rgba(menu), rgba(hover), rgba(dim), glass, rgba(press))
     .arg(rgba(well), rgba(tint), rgba(tintRim), rgba(withAlpha(dim, 110)));
}

void NewStyle::paintBackdrop(QPainter &p, const QRect &r) {
    p.fillRect(r, background());
    if (greyPalette()) {
        QLinearGradient wash(r.topLeft(), r.bottomRight());
        wash.setColorAt(0, QColor(44, 46, 51));
        wash.setColorAt(.54, background());
        wash.setColorAt(1, QColor(39, 41, 46));
        p.fillRect(r, wash);
        QRadialGradient light(QPointF(r.left() + r.width() * .88, r.top() + r.height() * .08),
                              std::max(r.width(), r.height()) * .7);
        light.setColorAt(0, QColor(185, 190, 200, 16));
        light.setColorAt(1, QColor(185, 190, 200, 0));
        p.fillRect(r, light);
        return;
    }
    if (pastel()) {
        // The mobile Pastel wash: plum to blue, with warm and lilac light in the corners.
        const qreal w = r.width(), h = r.height();
        QLinearGradient wash(r.topLeft(), r.bottomRight());
        if (light()) {
            wash.setColorAt(0, QColor(255, 220, 208));
            wash.setColorAt(.34, QColor(248, 228, 244));
            wash.setColorAt(.68, QColor(226, 214, 248));
            wash.setColorAt(1, QColor(198, 216, 252));
            p.fillRect(r, wash);
            return;
        }
        wash.setColorAt(0, QColor(92, 58, 96)); wash.setColorAt(.46, QColor(46, 40, 78)); wash.setColorAt(1, QColor(36, 62, 112));
        p.fillRect(r, wash);
        for (const auto &[x, y, radius, colour] : {std::tuple{.08, .02, .8, QColor(255, 150, 130, 120)},
                 std::tuple{.96, .12, .85, QColor(170, 130, 255, 110)}, std::tuple{.2, .96, .9, QColor(90, 150, 255, 100)}}) {
            QRadialGradient g(QPointF(r.left() + w * x, r.top() + h * y), std::max(w, h) * radius);
            g.setColorAt(0, colour);
            g.setColorAt(1, withAlpha(colour, 0));
            p.fillRect(r, g);
        }
        return;
    }
    if (!light()) return;
    // Soft light for the glass to lie on, as the mobile backdrop draws it: white stays the base.
    const qreal w = r.width(), h = r.height();
    for (const auto &[x, y, radius] : {std::tuple{.92, .1, .85}, std::tuple{.02, .48, .8}, std::tuple{.8, .95, .9}}) {
        QRadialGradient g(QPointF(w * x, h * y), w * radius);
        g.setColorAt(0, QColor(70, 70, 90, 26));
        g.setColorAt(1, QColor(70, 70, 90, 0));
        p.fillRect(r, g);
    }
}

void NewStyle::paintGlass(QPainter &p, const QRectF &r0, qreal radius, const QColor &tint) {
    const bool day = light();
    const QRectF r = r0.adjusted(.5, .5, -.5, -.5);
    const qreal corner = std::min({radius, r.height() / 2, r.width() / 2});
    QColor high, low;
    if (pastel()) {
        // As the phone's selectedShape: the plum plate a step towards lilac, solid, with a 1.5 px lilac edge.
        // The chosen row asks for the text colour, which in Pastel is the lilac accent.
        const QColor plate = day ? QColor(Qt::white) : QColor(42, 36, 62);
        if (tint.isValid()) {
            const QColor mark = tint == text() ? accent() : tint;
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(withAlpha(mark, day ? 200 : 230), 1.5));
            p.setBrush(blend(plate, mark, day ? .12 : .22));
            const QRectF edge = r0.adjusted(.75, .75, -.75, -.75);
            const qreal round = std::min({radius, edge.height() / 2, edge.width() / 2});
            p.drawRoundedRect(edge, round, round);
            p.restore();
            return;
        }
        high = day ? plate : QColor(46, 39, 68);
        low = day ? plate : QColor(40, 34, 60);
    } else if (tint.isValid()) { high = withAlpha(tint, day ? 36 : 44); low = withAlpha(tint, day ? 20 : 26); }
    else { high = day ? QColor(255, 255, 255, 205) : QColor(255, 255, 255, 15); low = day ? QColor(248, 248, 250, 160) : QColor(255, 255, 255, 9); }
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

void NewStyle::paintIcon(QPainter &p, const QString &name, const QRectF &box, const QColor &color) {
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

QIcon NewStyle::icon(const QString &name, const QColor &color) {
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

QString NewStyle::takeProfileFlag(QString &name) {
    static const QRegularExpression flag(QStringLiteral(
        "[\\x{1F1E6}-\\x{1F1FF}]{2}|[\\x{1F3F3}\\x{1F3F4}\\x{1F6A9}\\x{1F38C}\\x{2690}\\x{2691}]\\x{FE0F}?(?:(?:\\x{200D}\\p{So}\\x{FE0F}?)+|[\\x{E0061}-\\x{E007F}]+)?"));
    const auto match = flag.match(name);
    if (!match.hasMatch()) return {};
    const QString emoji = match.captured();
    name.remove(match.capturedStart(), match.capturedLength()).replace(QStringLiteral("  "), QStringLiteral(" "));
    name = name.trimmed();
    return emoji;
}

QPixmap NewStyle::flag(const QString &leading, int size, qreal dpr) {
    if (leading.isEmpty()) return {};
    static QHash<QString, QPixmap> cache;
    const QString key = leading + QString::number(size) + QLatin1Char('@') + QString::number(dpr);
    if (const auto it = cache.constFind(key); it != cache.constEnd()) return it.value();
    const int px = qRound(size * dpr);
    QPixmap art;
    const auto ucs = leading.toUcs4();
    if (ucs.size() >= 2 && ucs[0] >= 0x1F1E6 && ucs[0] <= 0x1F1FF && ucs[1] >= 0x1F1E6 && ucs[1] <= 0x1F1FF) {
        const QString code = QString(QChar('a' + int(ucs[0] - 0x1F1E6))) + QChar('a' + int(ucs[1] - 0x1F1E6));
        art = QPixmap(QStringLiteral(":/newstyle/flags/%1.png").arg(code));
        if (!art.isNull()) art = art.scaled(px, px, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (art.isNull()) {
        const bool plain = ucs.size() == 1 || (ucs.size() == 2 && ucs[1] == 0xFE0F);
        const bool black = plain && ucs[0] == 0x1F3F4;
        art = QPixmap(px, px);
        art.fill(black ? Qt::black : Qt::transparent);
        QPainter p(&art);
        if (plain && ucs[0] == 0x1F3F3) {
            // The system emoji font can draw a hollow flag on Windows.
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(px / 24.0, px / 24.0);
            QPainterPath cloth;
            cloth.moveTo(5, 5);
            cloth.cubicTo(10, 1, 15, 8, 20, 4);
            cloth.lineTo(20, 14);
            cloth.cubicTo(15, 18, 10, 11, 5, 15);
            cloth.closeSubpath();
            p.setBrush(Qt::white);
            p.setPen(QPen(QColor(110, 110, 116), 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(cloth);
            p.setPen(QPen(QColor(164, 164, 172), 1.4, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(5, 4), QPointF(5, 21));
        } else if (!black) {
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

void NewStyle::rise(QWidget *widget, int distance, int duration) {
    if (!widget || !widget->isVisible() || widget->property("newRising").toBool()) return;
    widget->setProperty("newRising", true);
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
        widget->setProperty("newRising", false);
        if (auto *parent = widget->parentWidget(); parent && parent->layout()) {
            parent->layout()->invalidate();
            parent->layout()->activate();
        }
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

// ---------------------------------------------------------------- tiles

NewTile::NewTile(Kind kind, QWidget *parent) : QAbstractButton(parent), m_kind(kind) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
}

void NewTile::setSpinning(bool spinning) {
    if (spinning == (m_spin != nullptr)) return;
    if (!spinning) { delete m_spin; m_spin = nullptr; m_angle = 0; update(); return; }
    m_spin = new QTimer(this);
    m_spin->setTimerType(Qt::PreciseTimer);
    m_spinClock.restart();
    connect(m_spin, &QTimer::timeout, this, [this] {
        const int interval = window()->isActiveWindow() ? frameInterval(this) : 120;
        if (m_spin->interval() != interval) m_spin->setInterval(interval);
        m_angle = std::fmod(m_spinClock.elapsed() * .4, 360.0);
        if (isVisible()) update();
    });
    m_spin->start(frameInterval(this));
}

QSize NewTile::sizeHint() const {
    const QFontMetrics fm(NewStyle::font(m_textSize, m_kind == Chip ? QFont::Bold : QFont::Normal));
    switch (m_kind) {
    case Icon: return m_flat ? QSize(40, 48) : QSize(48, 48);
    case Chip: return {fm.horizontalAdvance(text()) + 16 + 12 + 30, 48};
    case Tab: return {m_compactTab ? 44 : tabLabelWidth(), 44};
    case Text: return m_subtitle.isEmpty() ? QSize(fm.horizontalAdvance(text()) + 24, 52) : QSize(fm.horizontalAdvance(text()) + 32, 60);
    case Pill: return {QFontMetrics(NewStyle::font(m_textSize, QFont::Medium)).horizontalAdvance(text()) + 32, 40};
    }
    return {48, 48};
}

QSize NewTile::minimumSizeHint() const {
    if (m_kind == Tab && !m_icon.isEmpty()) return {44, 44};
    return m_kind == Text ? QSize(40, sizeHint().height()) : sizeHint();
}

int NewTile::tabLabelWidth() const {
    return QFontMetrics(NewStyle::font(m_textSize, QFont::Medium)).horizontalAdvance(text()) + 36;
}

void NewTile::setCompactTab(bool compact) {
    if (m_compactTab == compact) return;
    m_compactTab = compact;
    updateGeometry();
    update();
}

void NewTile::animatePress(qreal to, int duration) {
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

void NewTile::enterEvent(QEnterEvent *e) { m_hover = true; update(); QAbstractButton::enterEvent(e); }
void NewTile::leaveEvent(QEvent *e) { m_hover = false; update(); QAbstractButton::leaveEvent(e); }
void NewTile::mousePressEvent(QMouseEvent *e) { if (e->button() == Qt::LeftButton) animatePress(1, 90); QAbstractButton::mousePressEvent(e); }
void NewTile::mouseReleaseEvent(QMouseEvent *e) { animatePress(0, 260); QAbstractButton::mouseReleaseEvent(e); }

void NewTile::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) p.setOpacity(.4);
    const QRectF r = rect();
    const qreal radius = (m_kind == Icon && m_flat) || m_kind == Pill ? r.height() / 2 : 20;
    if (m_kind == Pill) {
        // Solid, so whatever lies under the bar never shows through it.
        p.setPen(QPen(NewStyle::outline(), 1));
        p.setBrush(NewStyle::solid());
        p.drawRoundedRect(r.adjusted(.5, .5, -.5, -.5), radius, radius);
    } else if (!m_flat) NewStyle::paintGlass(p, r, radius, m_selected ? NewStyle::text() : QColor());
    const int lift = (m_hover && isEnabled() ? 12 : 0) + qRound(m_press * 40);
    const bool ring = hasFocus() && g_keyFocus;
    if (lift > 0 || ring) {
        const bool softFocus = m_kind == Icon && m_icon == QLatin1String("more");
        p.setPen(ring && !m_hover && !softFocus ? QPen(NewStyle::text(), 1.5) : Qt::NoPen);
        p.setBrush(NewStyle::overlay(std::max(lift, ring && softFocus ? 24 : 0)));
        const qreal c = std::min(radius, r.height() / 2);
        if (m_flat) p.drawEllipse(r.center(), std::min(r.width(), r.height()) / 2 - 2, std::min(r.width(), r.height()) / 2 - 2);
        else p.drawRoundedRect(r.adjusted(.5, .5, -.5, -.5), c, c);
    }
    const QColor ink = NewStyle::text();
    switch (m_kind) {
    case Icon: {
        const qreal size = m_icon == QLatin1String("add") ? (m_flat ? 18 : 22) : (m_flat ? 22 : 24);
        QRectF box(0, 0, size, size);
        box.moveCenter(r.center());
        if (m_spin) {
            p.translate(r.center());
            p.rotate(m_angle);
            p.translate(-r.center());
        }
        NewStyle::paintIcon(p, m_icon, box, ink);
        break;
    }
    case Chip: {
        p.setFont(NewStyle::font(m_textSize, QFont::Bold));
        p.setPen(ink);
        const QRectF textBox = r.adjusted(16, 0, -40, 0);
        p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(text(), Qt::ElideRight, int(textBox.width())));
        const QRectF arrow(r.right() - 12 - 22, r.center().y() - 11, 22, 22);
        p.translate(arrow.center());
        p.rotate(90);
        p.translate(-arrow.center());
        NewStyle::paintIcon(p, QStringLiteral("chevron"), arrow, ink);
        break;
    }
    case Tab:
        if (m_compactTab) {
            QRectF box(0, 0, 22, 22);
            box.moveCenter(r.center());
            NewStyle::paintIcon(p, m_icon, box, m_selected || m_hover ? ink : NewStyle::muted());
            break;
        }
        [[fallthrough]];
    case Text: {
        if (!m_subtitle.isEmpty()) {
            p.setFont(NewStyle::font(15, QFont::Bold));
            p.setPen(ink);
            const QRectF line = r.adjusted(16, 8, -12, -r.height() / 2);
            p.drawText(line, Qt::AlignLeft | Qt::AlignBottom, p.fontMetrics().elidedText(text(), Qt::ElideRight, int(line.width())));
            p.setFont(NewStyle::font(12));
            p.setPen(NewStyle::muted());
            p.drawText(r.adjusted(16, r.height() / 2 + 2, -12, -8), Qt::AlignLeft | Qt::AlignTop, m_subtitle);
            break;
        }
        p.setFont(NewStyle::font(m_textSize, m_kind == Tab && m_selected ? QFont::Medium : QFont::Normal));
        p.setPen(m_kind == Tab && !m_selected ? NewStyle::muted() : ink);
        if (m_kind == Tab && m_hover) p.setPen(ink);
        p.drawText(r.adjusted(10, 0, -10, 0), Qt::AlignCenter, p.fontMetrics().elidedText(text(), Qt::ElideRight, int(r.width() - 20)));
        break;
    }
    case Pill:
        p.setFont(NewStyle::font(m_textSize, QFont::Medium));
        p.setPen(ink);
        p.drawText(r.adjusted(8, 0, -8, 0), Qt::AlignCenter, p.fontMetrics().elidedText(text(), Qt::ElideRight, int(r.width() - 16)));
        break;
    }
}

NewSwitch::NewSwitch(const QString &label, QWidget *parent) : QAbstractButton(parent) {
    setText(label);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setMinimumHeight(52);
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(180);
    m_anim->setEasingCurve(softCurve());
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { m_knob = v.toReal(); update(); });
}

void NewSwitch::setOn(bool on) {
    if (on == m_on) return;
    m_on = on;
    m_anim->stop();
    m_anim->setStartValue(m_knob);
    m_anim->setEndValue(on ? 1.0 : 0.0);
    if (isVisible()) m_anim->start();
    else { m_knob = on ? 1 : 0; update(); }
}

QSize NewSwitch::sizeHint() const { return {240, 52}; }
void NewSwitch::enterEvent(QEnterEvent *e) { m_hover = true; update(); QAbstractButton::enterEvent(e); }
void NewSwitch::leaveEvent(QEvent *e) { m_hover = false; update(); QAbstractButton::leaveEvent(e); }

void NewSwitch::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect();
    NewStyle::paintGlass(p, r, 20);
    const bool ring = hasFocus() && g_keyFocus;
    if (m_hover || isDown() || ring) {
        p.setPen(ring && !m_hover ? QPen(NewStyle::text(), 1.5) : Qt::NoPen);
        p.setBrush(NewStyle::overlay(isDown() ? 40 : 12));
        p.drawRoundedRect(r.adjusted(.5, .5, -.5, -.5), 20, 20);
    }
    const QRectF track(r.right() - 10 - 46, r.center().y() - 13, 46, 26);
    p.setFont(NewStyle::font(14));
    p.setPen(NewStyle::text());
    p.drawText(QRectF(14, 2, track.left() - 20, r.height() - 4), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, text());
    const QColor ink = NewStyle::text();
    const QColor off = NewStyle::overlay(34);
    QColor fill = QColor::fromRgbF(off.redF() + (ink.redF() - off.redF()) * m_knob, off.greenF() + (ink.greenF() - off.greenF()) * m_knob,
                                   off.blueF() + (ink.blueF() - off.blueF()) * m_knob, off.alphaF() + (.86 - off.alphaF()) * m_knob);
    p.setPen(QPen(NewStyle::outline(), 1));
    p.setBrush(fill);
    p.drawRoundedRect(track, 13, 13);
    const qreal knob = 20;
    const QPointF centre(track.left() + 3 + knob / 2 + (track.width() - 6 - knob) * m_knob, track.center().y());
    p.setPen(Qt::NoPen);
    p.setBrush(m_knob > .5 ? NewStyle::background() : (NewStyle::light() ? QColor(255, 255, 255) : QColor(205, 205, 210)));
    p.drawEllipse(centre, knob / 2, knob / 2);
}

// ---------------------------------------------------------------- the connect button

NewOrb::NewOrb(QWidget *parent) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setMinimumHeight(190);
    setMaximumHeight(190);
    m_clock.start();
    m_frame = new QTimer(this);
    m_frame->setTimerType(Qt::PreciseTimer);
    connect(m_frame, &QTimer::timeout, this, [this] {
        if (window()->isMinimized()) return;
        // Full rate in front, a slow wave is enough behind other windows.
        if (!window()->isActiveWindow() && m_clock.elapsed() - m_lastFrame < 120) return;
        const int interval = frameInterval(this);
        if (m_frame->interval() != interval) m_frame->setInterval(interval);
        update();
    });
}

void NewOrb::setState(bool connected, bool connecting, const QString &status, const QString &elapsed) {
    if (m_connected == connected && m_connecting == connecting && m_status == status && m_elapsed == elapsed) return;
    m_connected = connected;
    m_connecting = connecting;
    m_status = status;
    m_elapsed = elapsed;
    update();
}

void NewOrb::showEvent(QShowEvent *e) { m_frame->start(16); QAbstractButton::showEvent(e); }
void NewOrb::hideEvent(QHideEvent *e) { m_frame->stop(); QAbstractButton::hideEvent(e); }

bool NewOrb::hitButton(const QPoint &pos) const {
    const qreal rest = std::min(width(), height()) * .27;
    return QLineF(pos, QPointF(width() / 2.0, rest * 1.6)).length() <= rest * 1.25;
}

void NewOrb::animatePress(qreal to, int duration) {
    if (!m_pressAnim) {
        m_pressAnim = new QVariantAnimation(this);
        connect(m_pressAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { m_pressScale = v.toReal(); });
    }
    m_pressAnim->stop();
    m_pressAnim->setDuration(duration);
    m_pressAnim->setStartValue(m_pressScale);
    m_pressAnim->setEndValue(to);
    m_pressAnim->setEasingCurve(QEasingCurve::OutQuad);
    m_pressAnim->start();
}

void NewOrb::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && hitButton(e->pos())) animatePress(.92, 90);
    QAbstractButton::mousePressEvent(e);
}

void NewOrb::mouseReleaseEvent(QMouseEvent *e) {
    animatePress(1, 150);
    QAbstractButton::mouseReleaseEvent(e);
}

void NewOrb::paintEvent(QPaintEvent *) {
    const qint64 now = m_clock.elapsed();
    const qreal dt = m_lastFrame == 0 ? 8.0 : qreal(std::clamp<qint64>(now - m_lastFrame, 1, 100));
    m_lastFrame = now;
    const auto ease = [dt](qreal value, qreal target, qreal ms) {
        const qreal next = value + (target - value) * (1 - std::exp(-dt / ms));
        return std::abs(next - target) < .004 ? target : next;
    };
    m_wave = ease(m_wave, m_connected ? 1 : m_connecting ? .6 : .3, 320);
    m_spin = ease(m_spin, m_connecting ? 1 : 0, 240);
    m_lit = ease(m_lit, m_connected || m_connecting ? 1 : 0, 260);
    m_opacity = ease(m_opacity, isEnabled() ? 1.0 : .55, 120);
    m_angle = std::fmod(m_angle + dt * (.14 + .24 * m_spin), 360);
    m_phase = std::fmod(m_phase + dt * (.00035 + .001 * m_spin), 2 * M_PI);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);
    const int size = std::min(width(), height());
    const qreal rest = size * .27, base = rest * m_pressScale;
    const qreal cx = width() / 2.0, cy = rest * 1.6;
    const QColor ink = NewStyle::text();
    const bool day = NewStyle::light();

    // The rings leave the edge and fade before they reach the caption.
    if (m_lit > .01) {
        p.setBrush(Qt::NoBrush);
        const qreal cycle = 3400;
        for (int index = 0; index < 3; ++index) {
            const qreal travel = std::fmod(std::fmod(now, cycle) / cycle + index / 3.0, 1.0);
            p.setPen(QPen(withAlpha(ink, int((1 - travel) * (1 - travel) * 70 * m_wave)), 1));
            p.drawEllipse(QPointF(cx, cy), base * (1.08 + .5 * travel), base * (1.08 + .5 * travel));
        }
        // A soft light behind the disc grows with the connection.
        QRadialGradient glow(QPointF(cx, cy), base * 1.55);
        glow.setColorAt(0, withAlpha(ink, 34));
        glow.setColorAt(1, withAlpha(ink, 0));
        p.save();
        p.setOpacity(p.opacity() * m_lit);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(QPointF(cx, cy), base * 1.55, base * 1.55);
        p.restore();
    }
    struct WavePoint { QPointF unit; qreal sin8, cos8, sin3, cos3; };
    static const auto points = [] {
        std::array<WavePoint, 144> samples;
        for (size_t i = 0; i < samples.size(); ++i) {
            const qreal theta = i * 2.0 * M_PI / samples.size();
            samples[i] = {{std::cos(theta), std::sin(theta)}, std::sin(8 * theta), std::cos(8 * theta),
                          std::sin(3 * theta), std::cos(3 * theta)};
        }
        return samples;
    }();
    const qreal sin8 = std::sin(m_phase * 8), cos8 = std::cos(m_phase * 8);
    const qreal sin3 = std::sin(m_phase * 3), cos3 = std::cos(m_phase * 3);
    QPainterPath edge;
    edge.reserve(points.size() + 1);
    for (const auto &sample : points) {
        const qreal swell = 1 + m_wave * (.06 * (sample.sin8 * cos8 + sample.cos8 * sin8)
                                       + .02 * (sample.sin3 * cos3 - sample.cos3 * sin3));
        const QPointF point = QPointF(cx, cy) + sample.unit * (base * swell);
        if (&sample == &points.front()) edge.moveTo(point); else edge.lineTo(point);
    }
    edge.closeSubpath();
    // A drop: thin at the top, gathering light low in its body, a glint high on the left.
    p.save();
    p.setPen(Qt::NoPen);
    if (!day) p.setOpacity(p.opacity() * (127 + 128 * m_lit) / 255);
    QRadialGradient drop(QPointF(cx, cy + base * .62), base * 1.5);
    drop.setColorAt(0, day ? QColor(255, 255, 255, 235) : QColor(255, 255, 255, 60));
    drop.setColorAt(1, day ? QColor(232, 232, 240, 120) : QColor(255, 255, 255, 16));
    p.setBrush(drop);
    p.drawPath(edge);
    const qreal light = 241 * M_PI / 180;
    QRadialGradient glint(QPointF(cx + base * .7 * std::cos(light), cy + base * .7 * std::sin(light)), base * .8);
    glint.setColorAt(0, QColor(255, 255, 255, day ? 200 : 60));
    glint.setColorAt(1, QColor(255, 255, 255, 0));
    p.setBrush(glint);
    p.drawPath(edge);
    QRadialGradient caustic(QPointF(cx, cy + base * 1.05), base * .85);
    caustic.setColorAt(0, day ? QColor(0, 0, 0, 22) : QColor(255, 255, 255, 80));
    caustic.setColorAt(1, day ? QColor(0, 0, 0, 0) : QColor(255, 255, 255, 0));
    p.setBrush(caustic);
    p.drawPath(edge);
    p.restore();
    QLinearGradient rim(0, cy - base, 0, cy + base);
    rim.setColorAt(0, day ? QColor(0, 0, 0, 34) : QColor(255, 255, 255, 190));
    rim.setColorAt(1, day ? QColor(0, 0, 0, 16) : QColor(255, 255, 255, 46));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(rim), 1.2));
    p.drawPath(edge);
    if (hasFocus() && g_keyFocus) {
        p.setPen(QPen(ink, 2));
        p.drawEllipse(QPointF(cx, cy), base * 1.2, base * 1.2);
    }
    // The stroke that runs round while connecting: from a clear tail to a bright head.
    if (m_spin > .01) {
        const qreal ring = base * 1.14;
        constexpr int sweep = 225;
        QConicalGradient trail(QPointF(cx, cy), -m_angle * 2 - sweep);
        const auto head = withAlpha(ink, qRound(255 * m_spin));
        trail.setColorAt(0, head);
        trail.setColorAt(sweep / 360.0, withAlpha(ink, 0));
        trail.setColorAt(1, head);
        p.setPen(QPen(QBrush(trail), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(cx - ring, cy - ring, ring * 2, ring * 2), qRound(-m_angle * 32), -sweep * 16);
    }
    // Power symbol.
    const qreal glyph = base * .34;
    const QColor dim = NewStyle::muted();
    const QColor glyphColor = QColor::fromRgbF(dim.redF() + (ink.redF() - dim.redF()) * m_lit, dim.greenF() + (ink.greenF() - dim.greenF()) * m_lit,
                                               dim.blueF() + (ink.blueF() - dim.blueF()) * m_lit);
    p.setPen(QPen(glyphColor, base * .055, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(cx - glyph, cy - glyph, glyph * 2, glyph * 2), 55 * 16, -290 * 16);
    p.drawLine(QPointF(cx, cy - glyph * 1.3), QPointF(cx, cy - glyph * .15));
    // Caption under the waves.
    // The compact button keeps the caption and the time inside it, at the same proportions: the two lines take 51 px.
    const qreal fit = std::clamp((height() - cy - rest) / 51.0, .6, 1.0);
    const qreal top = cy + rest + 22 * fit;
    p.setFont(NewStyle::font(qRound(13 * fit)));
    p.setPen(dim);
    const QString caption = p.fontMetrics().elidedText(m_status.isEmpty() ? T("Click to connect") : m_status, Qt::ElideRight, width() - 8);
    p.drawText(QPointF(cx - p.fontMetrics().horizontalAdvance(caption) / 2.0, top), caption);
    if (m_connected) {
        p.setFont(NewStyle::font(qRound(16 * fit), QFont::Bold));
        p.setPen(ink);
        p.drawText(QPointF(cx - p.fontMetrics().horizontalAdvance(m_elapsed) / 2.0, top + 25 * fit), m_elapsed);
    }
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
            const QColor foreground = NewStyle::active() ? NewStyle::text() : palette().color(QPalette::WindowText);
            const auto highlight = m_kind == Close ? QColor(220, 66, 61) : foreground;
            if (m_hover || isDown()) {
                p.setPen(Qt::NoPen);
                p.setBrush(withAlpha(highlight, isDown() ? 45 : 25));
                p.drawRoundedRect(r.adjusted(2, 1, -2, -1), 14, 14);
            }
            const QColor ink = m_kind == Close && m_hover ? QColor(235, 90, 85) : foreground;
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

    // A glass card: the notice above the list and the log, connections and traffic pages.
    class GlassCard : public QWidget {
    public:
        explicit GlassCard(qreal radius, bool plain = false, QWidget *parent = nullptr) : QWidget(parent), m_radius(radius), m_plain(plain) {}
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            if (!m_plain) { NewStyle::paintGlass(p, rect(), m_radius); return; }
            // The pages inside paint the window's own ground, so the card takes it too and shows no square within.
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(NewStyle::light() ? QColor(0, 0, 0, 22) : QColor(255, 255, 255, 26), 1.35));
            p.setBrush(NewStyle::background());
            p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), m_radius, m_radius);
        }
    private:
        qreal m_radius;
        bool m_plain;
    };

    // The subscriptions sheet: switch between them, add one or manage them.
    class GlassPopup : public QWidget {
    public:
        explicit GlassPopup(QWidget *parent) : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
            setAttribute(Qt::WA_TranslucentBackground);
            setAttribute(Qt::WA_DeleteOnClose);
        }
        void showAt(QPoint pos) {
            // Kept on the screen it opens on, above its anchor when there is no room below.
            if (const QScreen *screen = QGuiApplication::screenAt(pos)) {
                const QRect room = screen->availableGeometry();
                pos.setX(std::clamp(pos.x(), room.left(), std::max(room.left(), room.right() + 1 - width())));
                pos.setY(std::clamp(pos.y(), room.top(), std::max(room.top(), room.bottom() + 1 - height())));
            }
            captureBackdrop(QRect(pos, size()).adjusted(0, 0, 0, 12));
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
            const QRectF body = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
            QPainterPath shape;
            shape.addRoundedRect(body, 28, 28);
            // Frosted: the window under the sheet, blurred, under a dense veil of the theme's ground.
            p.save();
            p.setClipPath(shape);
            p.fillRect(rect(), NewStyle::background());
            if (!m_backdrop.isNull()) p.drawPixmap(m_backdropOrigin - mapToGlobal(QPoint()), m_backdrop);
            const QColor veil = greyPalette() ? NewStyle::solid()
                : NewStyle::pastel() && !NewStyle::light() ? QColor(30, 26, 44)
                : NewStyle::light() ? QColor(250, 250, 252) : QColor(12, 12, 14);
            p.fillRect(rect(), withAlpha(veil, 200));
            p.restore();
            p.setPen(QPen(NewStyle::outline(), 1));
            p.setBrush(Qt::NoBrush);
            p.drawPath(shape);
        }
    private:
        void captureBackdrop(const QRect &global) {
            QWidget *under = parentWidget() ? parentWidget()->window() : nullptr;
            if (!under) return;
            const QPixmap shot = under->grab(QRect(under->mapFromGlobal(global.topLeft()), global.size()));
            if (shot.isNull()) return;
            // Two halvings down to a sixteenth and two smooth steps back: a cheap, soft blur.
            QImage image = shot.toImage();
            const QSize full = image.size();
            for (const int k : {4, 16}) image = image.scaled((full / k).expandedTo(QSize(1, 1)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            for (const int k : {4, 1}) image = image.scaled((full / k).expandedTo(QSize(1, 1)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            m_backdrop = QPixmap::fromImage(image);
            m_backdrop.setDevicePixelRatio(shot.devicePixelRatio());
            m_backdropOrigin = global.topLeft();
        }
        QPixmap m_backdrop;
        QPoint m_backdropOrigin;
    };

    QLabel *label(const QString &text, qreal px, bool muted, int weight = QFont::Normal) {
        auto *l = new QLabel(text);
        l->setFont(NewStyle::font(px, weight));
        if (muted) l->setProperty("newRole", "muted");
        return l;
    }
}

namespace {
    void changeWindowSize(QWidget *window, bool minimize) {
#ifdef Q_OS_WIN
        if (const auto id = window->internalWinId()) {
            // Use Windows' caption commands so the shell animates the transition with its current settings.
            const auto hwnd = reinterpret_cast<HWND>(id);
            const WPARAM command = minimize ? SC_MINIMIZE : IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE;
            SendMessageW(hwnd, WM_SYSCOMMAND, command, 0);
            return;
        }
#endif
        if (minimize) window->showMinimized();
        else if (window->isMaximized()) window->showNormal();
        else window->showMaximized();
    }
}

NewTitleBar::NewTitleBar(QWidget *parent) : QWidget(parent) {
    setFixedHeight(36);
    setMouseTracking(true);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(2, 4, 0, 0);
    row->setSpacing(2);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(16, 16);
    m_title = new QLabel(this);
    m_title->setFont(NewStyle::font(12));
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
    connect(minimize, &QAbstractButton::clicked, this, [this] { changeWindowSize(window(), true); });
    connect(maximize, &QAbstractButton::clicked, this, [this] { changeWindowSize(window(), false); });
    connect(close, &QAbstractButton::clicked, this, [this] { window()->close(); });
    row->addWidget(minimize);
    row->addWidget(maximize);
    row->addWidget(close);
}

void NewTitleBar::follow(QWidget *window) {
    if (m_window == window) return;
    m_window = window;
    window->installEventFilter(this);
    m_icon->setPixmap(window->windowIcon().pixmap(16, 16));
    m_title->setText(window->windowTitle());
}

bool NewTitleBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_window && event->type() == QEvent::WindowTitleChange) m_title->setText(m_window->windowTitle());
    if (watched == m_window && event->type() == QEvent::WindowIconChange) m_icon->setPixmap(m_window->windowIcon().pixmap(16, 16));
    return QWidget::eventFilter(watched, event);
}

void NewTitleBar::paintEvent(QPaintEvent *) {
#ifdef Q_OS_WIN
    if (NewStyle::active() && m_window && qobject_cast<MainWindow *>(m_window)) {
        QPainter painter(this);
        NewStyle::paintBackdrop(painter, QRect(QPoint(), m_window->size()));
    }
#endif
}

bool NewTitleBar::onTopEdge(const QPoint &pos) const {
    return pos.y() <= 5 && !window()->isMaximized() && !window()->isFullScreen();
}

void NewTitleBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !window()->windowHandle()) return;
    if (onTopEdge(e->pos())) window()->windowHandle()->startSystemResize(Qt::TopEdge);
    else window()->windowHandle()->startSystemMove();
}

void NewTitleBar::mouseMoveEvent(QMouseEvent *e) {
    if (onTopEdge(e->pos())) setCursor(Qt::SizeVerCursor);
    else unsetCursor();
    QWidget::mouseMoveEvent(e);
}

void NewTitleBar::mouseDoubleClickEvent(QMouseEvent *e) {
    // A dialog's bar only moves it.
    if (qobject_cast<QDialog *>(window())) return;
    if (e->button() == Qt::LeftButton) changeWindowSize(window(), false);
}

// ---------------------------------------------------------------- the server list

QRect NewProfileDelegate::chevronRect(const QRect &row) {
    return {row.right() - 5 - 6 - 32, row.top() + (row.height() - 3 - 32) / 2, 32, 32};
}

QSize NewProfileDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const { return {200, 55}; }

void NewProfileDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF row = QRectF(option.rect).adjusted(5, 0, -5, -3);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hover = option.state & QStyle::State_MouseOver;
    const int id = index.data(ProfilesTableModel::ProfileIdRole).toInt();
    const QColor ink = NewStyle::text(), dim = NewStyle::muted();
    NewStyle::paintGlass(*p, row, 20, id == (pendingId >= 0 ? pendingId : runningId) ? ink : QColor());
    if (hover || selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(NewStyle::overlay(hover && selected ? 16 : 10));
        p->drawRoundedRect(row.adjusted(.5, .5, -.5, -.5), 20, 20);
    }
    QString name = index.siblingAtColumn(ProfilesTableModel::ColName).data().toString().trimmed();
    const auto emoji = NewStyle::takeProfileFlag(name);
    const QRectF badge(row.left() + 10, row.center().y() - 14, 28, 28);
    p->setPen(Qt::NoPen);
    p->setBrush(NewStyle::solid());
    p->drawRoundedRect(badge, 10, 10);
    const QPixmap flag = NewStyle::flag(emoji, 28, p->device()->devicePixelRatioF());
    if (!flag.isNull()) {
        QPainterPath clip;
        clip.addRoundedRect(badge, 10, 10);
        p->save();
        p->setClipPath(clip);
        p->drawPixmap(badge.topLeft(), flag);
        p->restore();
        // White in a flag would melt into the light theme without an edge, and black into the dark one.
        p->setPen(QPen(withAlpha(dim, NewStyle::light() ? 70 : 110), 1));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(badge.adjusted(.5, .5, -.5, -.5), 10, 10);
    } else {
        NewStyle::paintIcon(*p, QStringLiteral("globe"), badge.adjusted(5, 5, -5, -5), dim);
    }
    const QRect chevron = chevronRect(option.rect);
    NewStyle::paintIcon(*p, QStringLiteral("chevron"), QRectF(chevron).adjusted(6, 6, -6, -6), withAlpha(ink, 200));
    // The test result: a spinner while it runs, then the latency in its colour.
    const QString result = index.siblingAtColumn(ProfilesTableModel::ColTestResult).data().toString();
    const int testState = index.siblingAtColumn(ProfilesTableModel::ColTestResult).data(ProfilesTableModel::TestStateRole).toInt();
    const bool measuring = testState == 2 && index.siblingAtColumn(ProfilesTableModel::ColTestResult).data(ProfilesTableModel::TestCountryRole).toString().isEmpty();
    p->setFont(NewStyle::font(12));
    const int resultWidth = testState == 1 || measuring ? 38 : result.isEmpty() ? 0 : std::clamp(p->fontMetrics().horizontalAdvance(result) + 8, 0, 200);
    QRectF resultBox(chevron.left() - resultWidth, row.center().y() - 16, resultWidth, 32);
    if (testState == 1) {
        p->setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap));
        p->setBrush(Qt::NoBrush);
        const QRectF spin(resultBox.center().x() - 9, resultBox.center().y() - 9, 18, 18);
        p->drawArc(spin, int(-spinAngle * 16), 270 * 16);
    }
    if (testState != 1 && !result.isEmpty()) {
        const QVariant colour = index.siblingAtColumn(ProfilesTableModel::ColTestResult).data(Qt::ForegroundRole);
        p->setPen(colour.isValid() ? colour.value<QColor>() : dim);
        p->drawText(resultBox, Qt::AlignCenter, p->fontMetrics().elidedText(result, Qt::ElideRight, int(resultBox.width())));
    }
    if (measuring) {
        // The speed test: a gauge whose needle sweeps, beside the latency the row already shows.
        if (!result.isEmpty()) resultBox = QRectF(resultBox.left() - 30, resultBox.top(), 30, resultBox.height());
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
    }
    const qreal textLeft = badge.right() + 10;
    qreal textRight = resultBox.left() - 4;
    if (index.data(ProfilesTableModel::PinnedRole).toBool()) {
        NewStyle::paintIcon(*p, QStringLiteral("pin"), QRectF(textRight - 16, row.center().y() - 8, 16, 16), dim);
        textRight -= 20;
    }
    p->setFont(NewStyle::font(15, QFont::Bold));
    p->setPen(ink);
    const qreal width = textRight - textLeft;
    p->drawText(QRectF(textLeft, row.top() + 6, width, 22), Qt::AlignLeft | Qt::AlignBottom, p->fontMetrics().elidedText(name, Qt::ElideRight, int(width)));
    p->setFont(NewStyle::font(12));
    p->setPen(dim);
    const QString type = index.siblingAtColumn(ProfilesTableModel::ColType).data().toString();
    p->drawText(QRectF(textLeft, row.top() + 29, width, 18), Qt::AlignLeft | Qt::AlignTop, p->fontMetrics().elidedText(type, Qt::ElideRight, int(width)));
    p->restore();
}

NewProfileList::NewProfileList(QWidget *backdrop) : QListView(backdrop), m_backdrop(backdrop) {
    m_activation = new QTimer(this);
    m_activation->setSingleShot(true);
    m_activation->setTimerType(Qt::PreciseTimer);
    connect(m_activation, &QTimer::timeout, this, [this] {
        if (isVisible() && m_pendingActivation.isValid() && activateRequested) activateRequested(m_pendingActivation);
    });
    rowDelegate = new NewProfileDelegate(this);
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
    m_spinner->setTimerType(Qt::PreciseTimer);
    m_spinClock.start();
    connect(m_spinner, &QTimer::timeout, this, [this] {
        if (!model() || !isVisible()) { m_spinner->setInterval(250); return; }
        // Repaints only while a visible row is being tested.
        const QModelIndex first = indexAt(QPoint(10, 2));
        const int from = first.isValid() ? first.row() : 0;
        const int to = std::min(model()->rowCount(), from + viewport()->height() / 55 + 2);
        for (int row = from; row < to; ++row) {
            if (model()->index(row, ProfilesTableModel::ColTestResult).data(ProfilesTableModel::TestStateRole).toInt() != 0) {
                const int interval = window()->isActiveWindow() ? frameInterval(this) : 120;
                if (m_spinner->interval() != interval) m_spinner->setInterval(interval);
                rowDelegate->spinAngle = std::fmod(m_spinClock.elapsed() * .27, 360.0);
                viewport()->update();
                return;
            }
        }
        m_spinner->setInterval(250);
    });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, viewport(), qOverload<>(&QWidget::update));
    m_spinner->start(250);
}

QModelIndex NewProfileList::tileAt(const QPointF &position) const {
    const auto index = indexAt(position.toPoint());
    if (!index.isValid()) return {};
    QPainterPath tile;
    tile.addRoundedRect(QRectF(visualRect(index)).adjusted(5, 0, -5, -3), 20, 20);
    return tile.contains(position) ? index : QModelIndex{};
}

void NewProfileList::queueActivation(const QModelIndex &index) {
    m_pendingActivation = index;
    m_activation->start(QApplication::doubleClickInterval());
}

void NewProfileList::mousePressEvent(QMouseEvent *e) {
    m_skipRelease = false;
    if (e->button() == Qt::LeftButton) {
        const auto index = tileAt(e->position());
        if (!index.isValid() || NewProfileDelegate::chevronRect(visualRect(index)).contains(e->pos())) {
            m_activation->stop();
            m_skipRelease = true;
            if (index.isValid()) {
                if (editRequested) editRequested(index);
            } else if (auto *selection = selectionModel()) {
                selection->clear();
            }
            e->accept();
            return;
        }
    }
    QListView::mousePressEvent(e);
}

void NewProfileList::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        QListView::mouseDoubleClickEvent(e);
        return;
    }
    const auto index = tileAt(e->position());
    if (!index.isValid()) return;
    m_activation->stop();
    m_skipRelease = true;
    e->accept();
    if (editRequested) editRequested(index);
}

void NewProfileList::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && m_skipRelease) {
        m_skipRelease = false;
        e->accept();
        return;
    }
    QListView::mouseReleaseEvent(e);
}

void NewProfileList::mouseMoveEvent(QMouseEvent *e) {
    const QModelIndex index = indexAt(e->pos());
    viewport()->setCursor(index.isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QListView::mouseMoveEvent(e);
}

void NewProfileList::leaveEvent(QEvent *e) {
    viewport()->update();
    QListView::leaveEvent(e);
}

void NewProfileList::paintEvent(QPaintEvent *e) {
    QListView::paintEvent(e);
    if (model() && model()->rowCount() == 0 && !emptyText.isEmpty()) {
        QPainter p(viewport());
        p.setFont(NewStyle::font(14));
        p.setPen(NewStyle::muted());
        p.drawText(viewport()->rect().adjusted(24, 0, -24, -80), Qt::AlignCenter | Qt::TextWordWrap, emptyText);
        return;
    }
    QPainter p(viewport());
    paintEdgeFades(p);
}

void NewProfileList::scrollContentsBy(int dx, int dy) {
    QListView::scrollContentsBy(dx, dy);
    const auto r = viewport()->rect();
    // Repaint the two fades and their old positions after Qt scrolls the backing store.
    const int depth = std::min(r.height(), 22 + qAbs(dy));
    viewport()->update(0, 0, r.width(), depth);
    viewport()->update(0, r.height() - depth, r.width(), depth);
}

void NewProfileList::paintEdgeFades(QPainter &p) {
    const auto *bar = verticalScrollBar();
    const bool top = bar->value() > bar->minimum();
    const bool bottom = bar->value() < bar->maximum();
    const int depth = std::min(22, viewport()->height() / 2);
    if ((!top && !bottom) || depth == 0 || viewport()->width() == 0) return;
    const QRect bounds(viewport()->mapTo(m_backdrop, QPoint()), viewport()->size());
    const auto ground = NewStyle::background();
    const qreal dpr = viewport()->devicePixelRatioF();
    if (m_fadeBounds != bounds || m_fadeBackdropSize != m_backdrop->size() ||
        m_fadeGround != ground || m_fades[0].devicePixelRatio() != dpr || m_fades[0].isNull()) {
        m_fadeBounds = bounds;
        m_fadeBackdropSize = m_backdrop->size();
        m_fadeGround = ground;
        // Keep only two small copies of the backdrop, with the same gradient as the window behind them.
        for (int edge = 0; edge < 2; ++edge) {
            const int y = edge == 0 ? 0 : bounds.height() - depth;
            QPixmap strip(qCeil(bounds.width() * dpr), qCeil(depth * dpr));
            strip.setDevicePixelRatio(dpr);
            strip.fill(Qt::transparent);
            {
                QPainter mask(&strip);
                mask.save();
                mask.translate(-bounds.topLeft() - QPoint(0, y));
                NewStyle::paintBackdrop(mask, m_backdrop->rect());
                mask.restore();
                QLinearGradient opacity(0, edge == 0 ? 0 : depth, 0, edge == 0 ? depth : 0);
                opacity.setColorAt(0, QColor(0, 0, 0, 245));
                opacity.setColorAt(1, Qt::transparent);
                mask.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                mask.fillRect(QRectF(0, 0, strip.width() / dpr, strip.height() / dpr), opacity);
            }
            m_fades[edge] = std::move(strip);
        }
    }
    if (top) p.drawPixmap(QPoint(0, 0), m_fades[0]);
    if (bottom) p.drawPixmap(QPoint(0, bounds.height() - depth), m_fades[1]);
}

void NewProfileList::wheelEvent(QWheelEvent *event) {
    auto *bar = m_joinedScroll ? m_joinedScroll : verticalScrollBar();
    // Touchpads already supply smooth pixel motion, including their own momentum.
    if (!event->pixelDelta().isNull() || event->angleDelta().y() == 0 || event->modifiers()) {
        if (m_scroll) m_scroll->stop();
        if (m_joinedScroll && !event->pixelDelta().isNull() && !event->modifiers()) {
            const int old = bar->value();
            bar->setValue(old - event->pixelDelta().y());
            event->setAccepted(bar->value() != old);
            return;
        }
        QListView::wheelEvent(event);
        return;
    }
    if (!m_scroll) {
        m_scroll = new QVariantAnimation(this);
        m_scroll->setDuration(180);
        m_scroll->setEasingCurve(QEasingCurve::OutQuad);
        connect(m_scroll, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            (m_joinedScroll ? m_joinedScroll : verticalScrollBar())->setValue(qRound(value.toReal()));
        });
        connect(verticalScrollBar(), &QScrollBar::sliderPressed, m_scroll, &QVariantAnimation::stop);
    }
    const int from = m_scroll->state() == QAbstractAnimation::Running ? m_scroll->endValue().toInt() : bar->value();
    const int delta = qRound(event->angleDelta().y() / 120.0 * QApplication::wheelScrollLines() * bar->singleStep());
    const int target = std::clamp(from - delta, bar->minimum(), bar->maximum());
    if (m_joinedScroll && target == bar->value()) { event->ignore(); return; }
    m_scroll->stop();
    m_scroll->setStartValue(bar->value());
    m_scroll->setEndValue(target);
    m_scroll->start();
    event->accept();
}

void NewProfileList::setJoinedScrollBar(QScrollBar *bar) {
    if (m_joinedScroll == bar) return;
    if (m_scroll) m_scroll->stop();
    if (m_joinedScroll) disconnect(m_joinedScroll, nullptr, this, nullptr);
    m_joinedScroll = bar;
    if (bar) connect(bar, &QScrollBar::sliderPressed, this, [this] { if (m_scroll) m_scroll->stop(); });
    setVerticalScrollBarPolicy(bar ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
}

void NewProfileList::dropEvent(QDropEvent *event) {
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

NewHome::NewHome(MainWindow *window) : QWidget(nullptr), mw(window) {
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 0, 16, 16);
    root->setSpacing(0);
#ifndef Q_OS_WIN
    m_titleBar = new NewTitleBar(this);
    root->addWidget(m_titleBar);
#endif
    m_contentScroll = new QScrollArea(this);
    m_contentScroll->setFrameShape(QFrame::NoFrame);
    m_contentScroll->viewport()->setAutoFillBackground(false);
    m_contentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_contentScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_contentScroll->setWidgetResizable(true);
    auto *content = new QWidget;
    m_columns = new QHBoxLayout(content);
    m_columns->setSizeConstraint(QLayout::SetMinimumSize);
    m_columns->setSpacing(12);
    m_contentScroll->setWidget(content);
    content->setAutoFillBackground(false);
    root->addWidget(m_contentScroll, 1);

    // The header and the connect button in a side column, so the server list keeps the full height.
    m_side = new QWidget(this);
    m_side->setFixedWidth(300);
    m_sideLayout = new QVBoxLayout(m_side);
    m_sideLayout->setContentsMargins(0, 0, 0, 0);
    m_sideLayout->setSpacing(0);
    m_sideLayout->setSizeConstraint(QLayout::SetMinimumSize);
    buildSide(m_sideLayout);
    m_columns->addWidget(m_side);

    m_bodyLayout = new QVBoxLayout;
    m_bodyLayout->setSpacing(0);
    buildBody(m_bodyLayout);
    m_columns->addLayout(m_bodyLayout, 1);
    applyArrangement();

    connect(Engine::host()->stats(), &Engine::StatsService::trafficSampled, this, [this](const Stats::TrafficSample &s) {
        m_relaySlots = s.relaySlots;
        m_rateDown = s.coreRunning ? s.proxyDown : 0;
        m_rateUp = s.coreRunning ? s.proxyUp : 0;
        m_down->setText(QStringLiteral("↓ ") + ReadableSize(qint64(m_rateDown)) + QStringLiteral("/s"));
        m_up->setText(QStringLiteral("↑ ") + ReadableSize(qint64(m_rateUp)) + QStringLiteral("/s"));
    });
    connect(mw->ui->tabWidget, &QTabWidget::currentChanged, this, [this] { groupChanged(); });
    connect(mw->ui->toolButton_startstop, &StartStopButton::stateChanged, this, &NewHome::refreshConnectionOrb);
    connect(mw->connection(), &Engine::ConnectionManager::busyChanged, this, &NewHome::refreshConnectionOrb);
    connect(themeManager(), &ThemeManager::themeChanged, this, [this] {
        const auto family = NewStyle::font(14).family();
        for (auto *text : findChildren<QLabel *>()) {
            auto font = text->font();
            font.setFamily(family);
            text->setFont(font);
        }
        if (m_attached) mw->applyTopBarMetrics();
        updateTabPresentation();
    });
    connect(mw->ui->data_view, &QTextBrowser::textChanged, this, [this] { updateNotice(); });
    connect(mw->ui->stats_widget, &QTabWidget::currentChanged, this, [this](int index) {
        if (!m_syncingTabs && m_attached && m_pages->currentIndex() == 1) selectTab(index + 1);
    });
    // Asked for from a menu (the runtime statistics): the page comes forward.
    connect(mw->ui->actionRuntime_Stats, &QAction::triggered, this, [this] { if (m_attached) selectTab(mw->ui->stats_widget->indexOf(mw->ui->runtime_tab) + 1); });
    auto *find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, [this] {
        selectTab(0);
        m_search->setVisible(!m_search->isVisible() || !m_search->hasFocus());
        if (m_search->isVisible()) m_search->setFocus();
        else m_search->clear();
    });

    m_tick = new QTimer(this);
    connect(m_tick, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
    m_tick->start(250);
}

void NewHome::buildSide(QVBoxLayout *side) {
    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    m_more = new NewTile(NewTile::Icon);
    m_more->setIconName("more");
    m_more->setToolTip(T("Menu"));
    connect(m_more, &QAbstractButton::clicked, this, [this] { showMoreMenu(); });
    auto *settings = new NewTile(NewTile::Icon);
    settings->setIconName("settings");
    settings->setToolTip(T("Settings"));
    connect(settings, &QAbstractButton::clicked, this, [this, settings] {
        guardReopen(mw->ui->menu_preferences, settings);
        mw->ui->menu_preferences->popup(settings->mapToGlobal(QPoint(0, settings->height() + 6)));
    });
    auto *title = label(QStringLiteral("Rawline"), 18, false, QFont::Bold);
    title->setAlignment(Qt::AlignCenter);
    header->addWidget(m_more);
    header->addWidget(title, 1);
    header->addWidget(settings);
    header->setContentsMargins(0, 0, 0, 0);
    m_header = new QWidget;
    m_header->setLayout(header);
    side->addWidget(m_header);

    // The group in use, as one card with a list behind it, and adding beside it.
    auto *chipRow = new QHBoxLayout;
    chipRow->setSpacing(8);
    m_chip = new NewTile(NewTile::Chip);
    m_chip->setTextSize(16);
    m_chip->setMinimumWidth(0);
    m_chip->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(m_chip, &QAbstractButton::clicked, this, [this] { showGroupPicker(); });
    m_chip->installEventFilter(this);
    m_add = new NewTile(NewTile::Icon);
    m_add->setIconName("add");
    m_add->setToolTip(T("Add"));
    connect(m_add, &QAbstractButton::clicked, this, [this] { showAddMenu(); });
    chipRow->addWidget(m_chip, 1);
    chipRow->addWidget(m_add);
    chipRow->setContentsMargins(0, 14, 0, 0);
    m_chipBox = new QWidget;
    m_chipBox->setLayout(chipRow);
    side->addWidget(m_chipBox);

    // The connect button, the check under it and the switches move as one block.
    m_controls = new QWidget;
    auto *controls = new QVBoxLayout(m_controls);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(0);
    controls->setSizeConstraint(QLayout::SetMinimumSize);
    m_orb = new NewOrb;
    m_orb->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_orb, &QAbstractButton::clicked, this, [this] { mw->ui->toolButton_startstop->click(); });
    connect(m_orb, &QWidget::customContextMenuRequested, this, [this] { mw->show_startstop_menu(); });
    controls->addSpacing(4);
    controls->addWidget(m_orb);

    // The check and the speed sit together under the button, in fixed boxes so nothing jumps.
    auto *status = new QHBoxLayout;
    status->setContentsMargins(10, 0, 6, 0);
    status->setSpacing(0);
    m_check = label(T("Check current\nconnection"), 12, true);
    m_check->setProperty("newRole", "link");
    m_check->setCursor(Qt::PointingHandCursor);
    m_check->installEventFilter(this);
    m_down = label(QStringLiteral("↓ 0 B/s"), 12, true);
    m_up = label(QStringLiteral("↑ 0 B/s"), 12, true);
    auto *speeds = new QVBoxLayout;
    speeds->setSpacing(0);
    for (auto *speed : {m_down, m_up}) {
        speed->setFixedWidth(96);
        speed->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        speeds->addWidget(speed);
    }
    status->addWidget(m_check, 1);
    status->addLayout(speeds);
    auto *statusBox = new QWidget;
    statusBox->setFixedHeight(40);
    statusBox->setLayout(status);
    controls->addWidget(statusBox);

    m_tun = new NewSwitch(T("TUN mode"));
    m_proxy = new NewSwitch(T("System proxy"));
    connect(m_tun, &QAbstractButton::clicked, this, [this] { mw->connection()->setTun(!Configs::dataManager->settingsRepo->spmode_vpn); });
    connect(m_proxy, &QAbstractButton::clicked, this, [this] { mw->connection()->setSystemProxy(!Configs::dataManager->settingsRepo->spmode_system_proxy); });
    controls->addSpacing(10);
    m_switches = new QVBoxLayout;
    m_switches->setSpacing(6);
    m_switches->addWidget(m_tun);
    m_switches->addWidget(m_proxy);
    controls->addLayout(m_switches);
    side->addWidget(m_controls);
    side->addStretch(1);

    // What the classic status bar said: the server in use and where the proxy listens.
    m_running = label({}, 12, true);
    m_running->setWordWrap(true);
    m_inbound = label({}, 12, true);
    m_inbound->setWordWrap(true);
    m_running->setContentsMargins(4, 0, 4, 2);
    m_inbound->setContentsMargins(4, 0, 4, 0);
    side->addWidget(m_running);
    side->addWidget(m_inbound);
}

void NewHome::buildBody(QVBoxLayout *body) {
    // The desktop's own pages beside the servers: log, connections, traffic and runtime.
    m_tabsBar = new QWidget;
    m_tabsBar->installEventFilter(this);
    auto *tabs = new QHBoxLayout(m_tabsBar);
    tabs->setContentsMargins(0, 0, 0, 0);
    tabs->setSpacing(6);
    QStringList titles{T("Servers")};
    const QStringList icons{"subscriptions", "logs", "link", "chart", "speedtest"};
    for (int i = 0; i < mw->ui->stats_widget->count(); ++i) titles << mw->ui->stats_widget->tabText(i).remove(QLatin1Char('&'));
    for (int i = 0; i < titles.size(); ++i) {
        auto *tab = new NewTile(NewTile::Tab);
        tab->setText(titles[i]);
        tab->setToolTip(titles[i]);
        tab->setIconName(icons.value(i, QStringLiteral("info")));
        tab->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        connect(tab, &QAbstractButton::clicked, this, [this, i] { selectTab(i); });
        m_tabs << tab;
        tabs->addWidget(tab);
    }
    tabs->addStretch(1);
    // On the log page: the mobile app's "send logs".
    m_sendLogs = new NewTile(NewTile::Icon);
    m_sendLogs->setIconName("share");
    m_sendLogs->setToolTip(QCoreApplication::translate("MainWindow", "Send logs"));
    m_sendLogs->setFixedSize(36, 36);
    m_sendLogs->hide();
    connect(m_sendLogs, &QAbstractButton::clicked, this, [this] { mw->sendLogs(); });
    tabs->addWidget(m_sendLogs);
    // Reserve room for the compact tabs and the log action even while the action is hidden.
    int compactWidth = m_sendLogs->minimumWidth() + m_tabs.size() * tabs->spacing();
    for (const auto *tab : m_tabs) compactWidth += tab->minimumSizeHint().width();
    m_tabsBar->setMinimumWidth(compactWidth);
    body->addSpacing(2);
    body->addWidget(m_tabsBar);
    body->addSpacing(10);

    m_pages = new QStackedWidget;
    body->addWidget(m_pages, 1);

    auto *servers = new QWidget;
    auto *serversLayout = new QVBoxLayout(servers);
    serversLayout->setContentsMargins(0, 0, 0, 0);
    serversLayout->setSpacing(0);

    m_notice = new GlassCard(20);
    m_noticeLayout = new QVBoxLayout(m_notice);
    m_noticeLayout->setContentsMargins(12, 4, 12, 4);
    m_notice->setMaximumHeight(0);
    m_notice->hide();
    serversLayout->addWidget(m_notice);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    m_profileColumn = new QWidget(servers);
    m_profileColumn->installEventFilter(this);
    auto *column = new QVBoxLayout(m_profileColumn);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    row->addWidget(m_profileColumn, 1);
    m_subscriptionScroll = new QScrollBar(Qt::Vertical, servers);
    m_subscriptionScroll->hide();
    row->addWidget(m_subscriptionScroll);
    serversLayout->addLayout(row, 1);

    m_subscriptionClip = new QWidget(m_profileColumn);
    m_subscriptionClip->installEventFilter(this);
    column->addWidget(m_subscriptionClip);
    m_subscriptionHeader = new QWidget(m_subscriptionClip);
    m_subscriptionHeader->installEventFilter(this);
    auto *subscription = new QVBoxLayout(m_subscriptionHeader);
    subscription->setContentsMargins(0, 0, 0, 0);
    subscription->setSpacing(0);
    auto *title = new QHBoxLayout;
    title->setContentsMargins(12, 4, 0, 0);
    title->setSpacing(0);
    auto *names = new QVBoxLayout;
    names->setContentsMargins(10, 8, 4, 8);
    names->setSpacing(3);
    m_groupName = label({}, 17, false);
    m_groupName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_groupDate = label({}, 10, true);
    names->addWidget(m_groupName);
    names->addWidget(m_groupDate);
    m_groupHeading = new QWidget;
    m_groupHeading->setLayout(names);
    m_groupHeading->setCursor(Qt::PointingHandCursor);
    for (auto *widget : {m_groupHeading, static_cast<QWidget *>(m_groupName), static_cast<QWidget *>(m_groupDate)}) widget->installEventFilter(this);
    title->addWidget(m_groupHeading, 1);
    m_groupHeading->setAccessibleName(T("Groups"));
    (new QFocusFrame(m_groupHeading))->setWidget(m_groupHeading);
    const auto action = [this, title](const QString &icon, const QString &tip, const std::function<void()> &callback) {
        auto *button = new NewTile(NewTile::Icon);
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
    subscription->addLayout(title);
    m_routeLine = label({}, 11, true);
    m_routeBox = new QWidget;
    auto *routeRow = new QHBoxLayout(m_routeBox);
    routeRow->setContentsMargins(22, 0, 12, 8);
    routeRow->setSpacing(6);
    auto *routeIcon = new NewTile(NewTile::Icon);
    routeIcon->setIconName("route");
    routeIcon->setFlat(true);
    routeIcon->setFixedSize(24, 24);
    routeIcon->setToolTip(T("Routing"));
    connect(routeIcon, &QAbstractButton::clicked, this, [this, routeIcon] {
        mw->ui->menuRouting_Menu->popup(routeIcon->mapToGlobal(QPoint(0, routeIcon->height())));
    });
    routeRow->addWidget(routeIcon);
    routeRow->addWidget(m_routeLine, 1);
    m_routeLine->setCursor(Qt::PointingHandCursor);
    // Wraps rather than cutting the label off; wrapped, a long profile name never widens the panel either.
    m_routeLine->setWordWrap(true);
    m_routeLine->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_routeLine->installEventFilter(this);
    subscription->addWidget(m_routeBox);

    m_traffic = new QWidget;
    auto *traffic = new QHBoxLayout(m_traffic);
    traffic->setContentsMargins(12, 0, 12, 8);
    traffic->setSpacing(8);
    auto *info = new NewTile(NewTile::Icon);
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
    m_quota->setStyleSheet(QStringLiteral("QLabel { border-radius: 14px; padding: 0 8px; }"));
    auto *bar = new GlassCard(14);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->addWidget(m_quota);
    m_support = new NewTile(NewTile::Icon);
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
    subscription->addWidget(m_traffic);
    m_expiry = label({}, 11, true);
    m_expiry->setContentsMargins(14, 0, 14, 8);
    subscription->addWidget(m_expiry);
    m_announce = label({}, 12, true);
    m_announce->setWordWrap(true);
    m_announce->setContentsMargins(14, 0, 14, 10);
    m_announce->setCursor(Qt::PointingHandCursor);
    m_announce->installEventFilter(this);
    subscription->addWidget(m_announce);

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
    subscription->addLayout(searchBox);

    m_list = new NewProfileList(this);
    m_list->setModel(mw->profilesFilterModel);
    m_list->setModelColumn(ProfilesTableModel::ColName);
    // One selection for both views: every menu acts on the servers chosen here.
    m_list->setSelectionModel(mw->ui->profilesTableView->selectionModel());
    m_list->rowsSwapped = [this](int from, int to) { if (mw->ui->profilesTableView->rowsSwapped) mw->ui->profilesTableView->rowsSwapped(from, to); };
    m_list->toSourceRow = [this](int row) { return mw->profilesFilterModel->toSourceRow(row); };
    m_list->editRequested = [this](const QModelIndex &index) { mw->on_profilesTableView_doubleClicked(index); };
    m_list->activateRequested = [this](const QModelIndex &index) {
        const int id = index.data(ProfilesTableModel::ProfileIdRole).toInt();
        if (!mw->select_mode && (m_list->rowDelegate->pendingId >= 0 || id != m_list->rowDelegate->runningId))
            mw->profile_start(id);
    };
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex &index) {
        if (QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) return;
        if (mw->select_mode) { mw->on_profilesTableView_doubleClicked(index); return; }
        m_list->queueActivation(index);
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        mw->ui->menu_server->popup(m_list->viewport()->mapToGlobal(pos));
    });
    column->addWidget(m_list, 1);
    connect(m_subscriptionScroll, &QScrollBar::valueChanged, this, [this] {
        if (!m_updatingSubscriptionScroll) scrollSubscription();
    });
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (!m_updatingSubscriptionScroll && Configs::dataManager->settingsRepo->new_layout_join_subscription &&
            (value > 0 || m_subscriptionScroll->value() > m_subscriptionHeader->height()))
            m_subscriptionScroll->setValue(m_subscriptionHeader->height() + value);
    });
    connect(m_list->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] {
        if (!m_updatingSubscriptionScroll) QTimer::singleShot(0, this, [this] { updateSubscriptionScroll(); });
    });

    m_empty = new QWidget;
    auto *imports = new QHBoxLayout(m_empty);
    imports->setContentsMargins(5, 8, 5, 0);
    imports->setSpacing(16);
    const auto importButton = [imports](const QString &text, QAction *action) {
        auto *button = new NewTile(NewTile::Text);
        button->setText(text);
        button->setFixedHeight(48);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        QObject::connect(button, &QAbstractButton::clicked, action, &QAction::trigger);
        imports->addWidget(button, 1);
    };
    importButton(T("Clipboard"), mw->ui->menu_add_from_clipboard);
    importButton(T("QR code"), mw->ui->menu_scan_qr);
    m_empty->hide();
    serversLayout->addWidget(m_empty);
    m_pages->addWidget(servers);

    auto *stats = new GlassCard(28, true);
    // The desktop pages inside keep their classic minimums; they must not stop the window narrowing.
    stats->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_statsLayout = new QVBoxLayout(stats);
    m_statsLayout->setContentsMargins(10, 10, 10, 10);
    m_pages->addWidget(stats);
}

void NewHome::attach() {
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
    if (m_titleBar) m_titleBar->follow(mw);
    m_dataSlot = find(mw->ui->data_view);
    m_statsSlot = find(mw->ui->stats_widget);
    m_noticeLayout->addWidget(mw->ui->data_view);
    m_statsLayout->addWidget(mw->ui->stats_widget);
    mw->ui->stats_widget->tabBar()->hide();
    mw->ui->stats_widget->setDocumentMode(true);
    m_attached = true;
    selectTab(0, false);
    m_renderedGroup = -1;
    m_headerKey.clear();
    updateNotice();
    refresh();
}

void NewHome::detach() {
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

void NewHome::selectTab(int index, bool animated) {
    if (index < 0 || index >= m_tabs.size()) return;
    for (int i = 0; i < m_tabs.size(); ++i) m_tabs[i]->setSelected(i == index);
    m_sendLogs->setVisible(index > 0 && mw->ui->stats_widget->widget(index - 1) == mw->ui->Logs);
    updateTabPresentation();
    const int page = index == 0 ? 0 : 1;
    const bool changed = m_pages->currentIndex() != page || (page == 1 && mw->ui->stats_widget->currentIndex() != index - 1);
    m_pages->setCurrentIndex(page);
    if (page == 1) {
        m_syncingTabs = true;
        mw->ui->stats_widget->setCurrentIndex(index - 1);
        m_syncingTabs = false;
    }
    if (animated && changed) NewStyle::rise(m_pages->currentWidget(), 12, 220);
}

void NewHome::groupChanged() {
    if (!m_attached) return;
    refreshGroupHeader();
    const int current = Configs::dataManager->settingsRepo->current_group;
    // Only a change of group rises; the first render is simply there.
    if (m_renderedGroup != -1 && m_renderedGroup != current && m_pages->currentIndex() == 0) NewStyle::rise(m_list, 14, 240);
    m_renderedGroup = current;
    // The classic table scrolls to the chosen server; so does the list, once the group's rows are in.
    QTimer::singleShot(0, m_list, [this] { if (m_list->currentIndex().isValid()) m_list->scrollTo(m_list->currentIndex(), QAbstractItemView::PositionAtCenter); });
}

void NewHome::updateNotice() {
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

// The order BuildSingBoxConfig decides it in: the subscription's Happ / Incy routing, else its own or the global profile.
QString NewHome::routeText(const std::shared_ptr<Configs::Group> &group, bool *fromProvider) {
    const auto &options = group->sub_options;
    *fromProvider = options.provider_routing_enabled && !options.routing_provider.isEmpty() && !options.provider_routing.isEmpty();
    // A full Xray config keeps its own routing: the provider's lists are not applied, and with the client's
    // routing only its rules go first, the rest follows the config.
    const auto ids = group->Profiles();
    const auto first = ids.isEmpty() ? nullptr : Configs::dataManager->profilesRepo->GetProfile(ids.first());
    const bool xrayFull = first && first->outbound && first->outbound->IsXrayFullConfig();
    if (*fromProvider) {
        if (xrayFull) return T("Routing of the provider's Xray config");
        if (options.provider_routing == "off") return T("Provider turned routing off: all through the server");
        const auto name = QString2QJsonObject(options.provider_routing).value("Name").toString();
        return T("Provider routing: %1").arg(name.isEmpty() ? options.routing_provider : name);
    }
    const int routeId = Configs::RouteIdFor(group->id);
    const auto route = Configs::dataManager->routesRepo->GetRouteProfile(routeId);
    const QString name = route ? route->name : QString();
    const QString rest = xrayFull ? T(", the rest by the Xray config") : QString();
    if (routeId == group->route_id && routeId != Configs::dataManager->settingsRepo->current_route_id) return T("Own routing: %1").arg(name) + rest;
    return group->url.isEmpty() ? QString() : T("Routing: %1").arg(name) + rest;
}

void NewHome::refreshGroupHeader() {
    const auto group = Configs::dataManager->groupsRepo->CurrentGroup();
    if (!group) return;
    const auto &sub = group->sub_info;
    const bool subscription = !group->url.isEmpty();
    const auto route = routeText(group, &m_routeFromProvider);
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10")
        .arg(group->id).arg(group->name).arg(group->sub_last_update).arg(sub.used()).arg(sub.total).arg(sub.expire)
        .arg(sub.announce, sub.support_url).arg(int(mw_sub_updating) * 2 + int(m_announceOpen)).arg(route);
    m_routeBox->setVisible(!route.isEmpty());
    m_routeLine->setToolTip(route);
    m_routeLine->setText(route);
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

void NewHome::refreshConnectionOrb() {
    if (!m_attached) return;
    const int pendingId = mw->connection()->pendingProfileId();
    if (m_list->rowDelegate->pendingId != pendingId) {
        m_list->rowDelegate->pendingId = pendingId;
        m_list->viewport()->update();
    }
    auto *button = mw->ui->toolButton_startstop;
    const auto state = button->state();
    const bool connected = state == StartStopButton::State::Running;
    const bool connecting = state == StartStopButton::State::Connecting;
    if (connected && !m_wasRunning) m_connectedFor.start();
    if (!connected) m_relaySlots.clear();
    m_wasRunning = connected;
    QString status;
    if (connecting) status = T("Connecting…");
    else if (state == StartStopButton::State::Disconnecting) status = T("Stopping…");
    else if (connected) status = T("Connected");
    const qint64 seconds = connected && m_connectedFor.isValid() ? m_connectedFor.elapsed() / 1000 : 0;
    QString elapsed = QStringLiteral("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    if (connected && !m_relaySlots.isEmpty()) elapsed += QStringLiteral("  ") + m_relaySlots;
    m_orb->setState(connected, connecting, status, elapsed);
    m_orb->setEnabled(state != StartStopButton::State::Disabled && state != StartStopButton::State::Disconnecting);
}

void NewHome::refresh() {
    if (!m_attached) return;
    refreshConnectionOrb();
    const bool connected = mw->ui->toolButton_startstop->state() == StartStopButton::State::Running;

    const auto &settings = Configs::dataManager->settingsRepo;
    m_tun->setOn(settings->spmode_vpn);
    m_proxy->setOn(settings->spmode_system_proxy);
    // The row keeps its height, so the list beside it does not jump on connecting.
    for (auto *speed : {m_down, m_up}) speed->setVisible(connected && !settings->disable_traffic_stats);

    // The check of the current connection: its result for a moment, then the invitation again.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_checkSince > 0) {
        const QString prefix = QCoreApplication::translate("MainWindow", "Test Result") + QStringLiteral(": ");
        const QString running = mw->ui->label_running->text();
        if (running.startsWith(prefix)) {
            const QString result = running.mid(prefix.size());
            static const QRegularExpression ms(QStringLiteral("^(\\d+) ms$"));
            const auto match = ms.match(result);
            m_checkResult = match.hasMatch() ? T("Success: %1 ms").arg(match.captured(1)) : T("n/a");
            m_checkShownAt = now;
            m_checkSince = 0;
        } else if (now - m_checkSince > 20000) {
            m_checkSince = 0;
        }
    }
    if (m_checkShownAt > 0 && now - m_checkShownAt > 2500) { m_checkShownAt = 0; m_checkResult.clear(); }
    if (!connected) { m_checkSince = 0; m_checkShownAt = 0; m_checkResult.clear(); }
    const QString check = m_checkSince > 0 ? T("Checking ping") : !m_checkResult.isEmpty() ? m_checkResult : T("Check current\nconnection");
    if (m_check->text() != check) m_check->setText(check);
    m_check->setVisible(connected);

    const QString runningText = mw->ui->label_running->text();
    if (!runningText.startsWith(QCoreApplication::translate("MainWindow", "Test Result") + QStringLiteral(": ")) &&
        runningText != QCoreApplication::translate("MainWindow", "Testing") && m_running->text() != runningText) {
        m_running->setText(runningText);
    }
    if (m_inbound->text() != mw->ui->label_inbound->text()) m_inbound->setText(mw->ui->label_inbound->text());
    m_inbound->setToolTip(mw->ui->label_inbound->toolTip());

    const auto running = mw->runningSnapshot();
    const int runningId = running ? running->id : -1;
    if (m_list->rowDelegate->runningId != runningId) {
        m_list->rowDelegate->runningId = runningId;
        m_list->viewport()->update();
    }
    refreshGroupHeader();
    m_empty->setVisible(m_list->model() && m_list->model()->rowCount() == 0 && m_search->text().isEmpty());
    if (m_renderedGroup == -1) m_renderedGroup = settings->current_group;
}

void NewHome::checkCurrentConnection() {
    if (m_checkSince > 0) return;
    m_checkResult.clear();
    m_checkShownAt = 0;
    m_checkSince = QDateTime::currentMSecsSinceEpoch();
    mw->url_test_current();
    refresh();
}

void NewHome::showGroupPicker() {
    auto *popup = new GlassPopup(this);
    popup->setObjectName(QStringLiteral("newHomeGroupPicker"));
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    // No WA_TranslucentBackground here: on a child of a see-through window its clear fill cuts a hole down to the
    // desktop, which left the list area of the sheet unfrosted. Not filling at all is enough.
    auto *list = new QWidget;
    scroll->viewport()->setAutoFillBackground(false);
    auto *rows = new QVBoxLayout(list);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(6);
    const int current = Configs::dataManager->settingsRepo->current_group;
    int height = 0;
    for (const int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
        const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
        if (!group) continue;
        auto *row = new NewTile(NewTile::Text);
        row->setText(group->name);
        row->setSubtitle(group->url.isEmpty() ? T("Group") : T("Subscription"));
        row->setSelected(gid == current);
        row->setFixedHeight(60);
        connect(row, &QAbstractButton::clicked, this, [this, popup, gid] {
            popup->close();
            mw->ui->tabWidget->setCurrentIndex(groupId2TabIndex(gid));
        });
        rows->addWidget(row);
        height += 66;
    }
    rows->addStretch();
    scroll->setWidget(list);
    // setWidget turns filling on, which in a see-through window copies a clear fill straight through.
    list->setAutoFillBackground(false);
    scroll->setFixedHeight(std::min(height, 420));
    layout->addWidget(scroll);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(12);
    int buttonsWidth = 32 + 12;
    for (const auto &[text, action] : {std::pair{T("Add group or subscription"), mw->ui->actionAdd_New_Group},
                                       std::pair{T("Manage groups and subscriptions"), mw->ui->menu_manage_groups}}) {
        auto *button = new NewTile(NewTile::Text);
        button->setText(text);
        button->setFixedHeight(52);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        buttonsWidth += button->sizeHint().width();
        QAction *target = action;
        connect(button, &QAbstractButton::clicked, this, [popup, target] { popup->close(); target->trigger(); });
        actions->addWidget(button, 1);
    }
    layout->addLayout(actions);
    popup->setFixedWidth(std::max({440, m_chip->width() + 56, buttonsWidth}));
    popup->adjustSize();
    QWidget *anchor = m_chip;
    guardReopen(popup, anchor);
    popup->showAt(anchor->mapToGlobal(QPoint(0, anchor->height() + 6)));
}

void NewHome::showMoreMenu() {
    // The new style has no tool bar: one corner menu reaches every menu of the classic window.
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
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
    if (ui->actionCheck_For_Update->isEnabled()) {
        menu->addSeparator();
        menu->addAction(ui->actionCheck_For_Update);
    }
    guardReopen(menu, m_more);
    menu->popup(m_more->mapToGlobal(QPoint(0, m_more->height() + 6)));
}

void NewHome::showAddMenu() {
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto *ui = mw->ui;
    menu->addAction(ui->menu_add_from_clipboard);
    menu->addAction(ui->menu_add_from_input);
    menu->addAction(ui->actionAdd_profile_from_File);
    menu->addAction(ui->menu_scan_qr);
    menu->addSeparator();
    menu->addAction(ui->actionAdd_New_Group);
    guardReopen(menu, m_add);
    menu->popup(m_add->mapToGlobal(QPoint(0, m_add->height() + 6)));
}

void NewHome::showGroupMenu() {
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto *ui = mw->ui;
    const auto group = Configs::dataManager->groupsRepo->CurrentGroup();
    if (!group) return;
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
    ui->actionDelete_Group->setEnabled(Configs::dataManager->groupsRepo->GetAllGroupIds().size() > 1);
    for (auto *action : ui->menuTesting->actions()) {
        if (action == ui->menu_stop_testing) continue;
        menu->addAction(action);
    }
    if (Engine::host()->tests()->isRunning()) menu->addAction(ui->menu_stop_testing);
    guardReopen(menu, m_groupMore);
    menu->popup(m_groupMore->mapToGlobal(QPoint(m_groupMore->width() - menu->sizeHint().width(), m_groupMore->height() + 2)));
}

namespace {
    // The arranging mode over the window: each movable block in a breathing dashed frame that the mouse drags to
    // its place, small buttons on the parts that change, the tutorial link and Done. The window takes no other clicks.
    class ArrangeOverlay : public QWidget {
    public:
        struct Block { QPointer<QWidget> widget; std::function<void(QPoint)> drop; std::function<void()> twice; };
        struct Button { NewTile *tile; QPointer<QWidget> anchor; };
        ArrangeOverlay(QWidget *parent, QScrollArea *scroll, std::function<void()> done, std::function<void()> tutorial)
            : QWidget(parent), m_contentScroll(scroll) {
            m_done = new NewTile(NewTile::Pill, this);
            m_done->setText(QCoreApplication::translate("NewHome", "Done"));
            m_done->setFixedSize(std::max(120, m_done->sizeHint().width()), 40);
            connect(m_done, &QAbstractButton::clicked, this, std::move(done));
            m_tutorial = new NewTile(NewTile::Pill, this);
            m_tutorial->setText(QCoreApplication::translate("NewHome", "Tutorial"));
            m_tutorial->setFixedSize(std::max(120, m_tutorial->sizeHint().width()), 40);
            connect(m_tutorial, &QAbstractButton::clicked, this, std::move(tutorial));
            auto *timer = new QTimer(this);
            connect(timer, &QTimer::timeout, this, [this] { m_phase = (m_phase + 1) % 32; place(); update(); });
            timer->start(60);
            setMouseTracking(true);
            parent->installEventFilter(this);
            connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { place(); update(); });
        }
        // [twice]: what a double click on the block changes, as two taps do in the mobile editor.
        void addBlock(QWidget *widget, std::function<void(QPoint)> drop, std::function<void()> twice = {}) { m_blocks << Block{widget, std::move(drop), std::move(twice)}; }
        void addButton(QWidget *anchor, const QString &glyph, const QString &tip, std::function<void()> action) {
            auto *tile = new NewTile(NewTile::Pill, this);
            tile->setObjectName(QStringLiteral("arrangeBlockButton"));
            tile->setText(glyph);
            tile->setToolTip(tip);
            tile->setFixedSize(30, 30);
            connect(tile, &QAbstractButton::clicked, this, [this, action = std::move(action)] { action(); rebuild(); });
            m_buttons << Button{tile, anchor};
            place();
        }
        void clearButtons() {
            for (auto &button : m_buttons) { button.tile->hide(); button.tile->deleteLater(); }
            m_buttons.clear();
        }
        std::function<void()> rebuild = [] {};
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            p.fillRect(rect(), NewStyle::light() ? QColor(255, 255, 255, 60) : QColor(0, 0, 0, 70));
            // Marching dashes that also breathe, as the mobile editor's frames do.
            const qreal pulse = 0.55 + 0.45 * std::sin(m_phase * M_PI / 16);
            QPen pen(withAlpha(NewStyle::text(), int(110 + 145 * pulse)), 1.6, Qt::DashLine);
            pen.setDashOffset(m_phase / 2.0);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            for (const auto &block : m_blocks) {
                if (!block.widget || !block.widget->isVisible()) continue;
                const QRect visible = visibleAnchor(block.widget, 0);
                if (visible.isEmpty()) continue;
                p.save();
                p.setClipRect(visible, Qt::IntersectClip);
                p.drawRoundedRect(QRectF(frameOf(block)).adjusted(1, 1, -1, -1), 16, 16);
                p.restore();
            }
            if (m_dragged >= 0 && !m_ghost.isNull()) {
                // The block follows the mouse, lifted and see-through, until it is dropped.
                p.setOpacity(.82);
                const QRect at(m_mouse - m_grab, m_ghost.size() / m_ghost.devicePixelRatio());
                p.fillRect(at.translated(0, 6), withAlpha(Qt::black, 40));
                p.drawPixmap(at, m_ghost);
                p.setOpacity(1);
                p.setPen(QPen(NewStyle::text(), 2, Qt::DashLine));
                p.drawRoundedRect(QRectF(at).adjusted(-4, -4, 4, 4), 16, 16);
            }
        }
        void resizeEvent(QResizeEvent *) override { place(); }
        void wheelEvent(QWheelEvent *event) override {
            if (m_dragged >= 0) return;
            event->ignore();
            QApplication::sendEvent(m_contentScroll->viewport(), event);
        }
        bool eventFilter(QObject *watched, QEvent *event) override {
            if (watched == parentWidget() && event->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
            return false;
        }
        // The innermost block under the mouse: the panel holds the others.
        int blockAt(QPoint at) const {
            int best = -1;
            for (int i = 0; i < m_blocks.size(); ++i) {
                const auto &block = m_blocks[i];
                if (!block.widget || !block.widget->isVisible() || !frameOf(block).contains(at)) continue;
                if (best < 0 || frameOf(block).width() * frameOf(block).height() < frameOf(m_blocks[best]).width() * frameOf(m_blocks[best]).height()) best = i;
            }
            return best;
        }
        void mouseDoubleClickEvent(QMouseEvent *event) override {
            event->accept();
            m_dragged = -1;
            m_ghost = {};
            const int best = blockAt(event->pos());
            if (best >= 0 && m_blocks[best].twice) { m_blocks[best].twice(); rebuild(); }
            update();
        }
        void mousePressEvent(QMouseEvent *event) override {
            event->accept();
            const int best = blockAt(event->pos());
            if (best < 0) return;
            m_dragged = best;
            m_ghost = m_blocks[best].widget->grab();
            m_grab = event->pos() - frameOf(m_blocks[best]).topLeft();
            m_mouse = event->pos();
            // The block's own place stays as a faint outline while its picture is carried.
            auto *faint = new QGraphicsOpacityEffect(m_blocks[best].widget);
            faint->setOpacity(.22);
            m_blocks[best].widget->setGraphicsEffect(faint);
            setCursor(Qt::ClosedHandCursor);
            update();
        }
        void mouseMoveEvent(QMouseEvent *event) override {
            if (m_dragged < 0) {
                bool over = false;
                for (const auto &block : m_blocks) over |= block.widget && block.widget->isVisible() && frameOf(block).contains(event->pos());
                setCursor(over ? Qt::OpenHandCursor : Qt::ArrowCursor);
                return;
            }
            m_mouse = event->pos();
            // As on the phone, the window rearranges while the block is carried, not only once it is let go.
            if (const auto &drop = m_blocks[m_dragged].drop) drop(event->pos());
            update();
        }
        void mouseReleaseEvent(QMouseEvent *event) override {
            if (m_dragged < 0) return;
            const auto block = m_blocks[m_dragged];
            m_dragged = -1;
            m_ghost = {};
            if (block.widget) block.widget->setGraphicsEffect(nullptr);
            setCursor(Qt::OpenHandCursor);
            if (block.drop) block.drop(event->pos());
            rebuild();
            update();
        }
    private:
        QRect frameOf(const Block &block) const { return QRect(mapFromGlobal(block.widget->mapToGlobal(QPoint())), block.widget->size()); }
        QRect visibleAnchor(QWidget *anchor, int inset = 5) const {
            QRect visible = rect();
            for (auto *widget = anchor; widget && widget != parentWidget(); widget = widget->parentWidget()) {
                if (widget->isHidden()) return {};
                visible &= QRect(widget->mapTo(parentWidget(), QPoint()), widget->size());
            }
            return visible.adjusted(inset, inset, -inset, -inset);
        }
        void place() {
            for (const auto &button : m_buttons) {
                const QRect frame = button.anchor ? visibleAnchor(button.anchor) : QRect();
                const QSize size = button.tile->size();
                const bool fits = frame.width() >= size.width() && frame.height() >= size.height();
                button.tile->setVisible(fits);
                if (fits) button.tile->move(frame.right() - size.width() + 1, frame.top());
            }
            m_done->move(width() - m_done->width() - 24, height() - m_done->height() - 24);
            m_tutorial->move(24, height() - m_tutorial->height() - 24);
            m_done->raise();
            m_tutorial->raise();
        }
        QList<Block> m_blocks;
        QList<Button> m_buttons;
        QScrollArea *m_contentScroll;
        NewTile *m_done;
        NewTile *m_tutorial;
        int m_phase = 0;
        int m_dragged = -1;
        QPixmap m_ghost;
        QPoint m_grab, m_mouse;
    };

    // The arranging tutorial: a small window played by a cursor, one line under it; skipped, finished or closed.
    class ArrangeTutorial : public QWidget {
    public:
        ArrangeTutorial(QWidget *parent, std::function<void()> closed) : QWidget(parent), m_closed(std::move(closed)) {
            setAttribute(Qt::WA_DeleteOnClose);
            setObjectName(QStringLiteral("arrangeTutorial"));
            m_skip = new NewTile(NewTile::Pill, this);
            m_skip->setText(QCoreApplication::translate("NewHome", "Skip"));
            m_skip->setFixedSize(std::max(120, m_skip->sizeHint().width()), 38);
            connect(m_skip, &QAbstractButton::clicked, this, [this] { finish(); });
            m_next = new NewTile(NewTile::Pill, this);
            m_next->setFixedSize(120, 38);
            connect(m_next, &QAbstractButton::clicked, this, [this] { if (m_step == 2) finish(); else { m_step++; m_clock.restart(); updateText(); } });
            auto *timer = new QTimer(this);
            connect(timer, &QTimer::timeout, this, [this] { update(); });
            timer->start(16);
            m_clock.start();
            parent->installEventFilter(this);
            setGeometry(parent->rect());
            setFocusPolicy(Qt::StrongFocus);
            updateText();
            show();
            raise();
            setFocus();
        }
    protected:
        bool eventFilter(QObject *watched, QEvent *event) override {
            if (watched == parentWidget() && event->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
            return false;
        }
        void keyPressEvent(QKeyEvent *event) override { if (event->key() == Qt::Key_Escape) finish(); }
        void mousePressEvent(QMouseEvent *event) override { event->accept(); }
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            p.fillRect(rect(), QColor(0, 0, 0, 175));
            const qreal t = std::fmod(m_clock.elapsed() / 2800.0, 1.0);
            const auto span = [t](qreal a, qreal b) { return std::clamp((t - a) / (b - a), 0.0, 1.0); };
            const auto ease = [](qreal x) { return x * x * (3 - 2 * x); };
            const QRectF demo(width() / 2.0 - 220, height() / 2.0 - 190, 440, 280);
            QPointF cursor;
            qreal press = 0;
            p.setPen(Qt::NoPen);
            p.setBrush(NewStyle::solid());
            p.drawRoundedRect(demo, 18, 18);
            const QColor surface = NewStyle::light() ? QColor(0, 0, 0, 18) : QColor(255, 255, 255, 22);
            const QColor accent = NewStyle::pastel() ? NewStyle::accent() : QColor(80, 140, 255);
            QRectF panel(demo.left() + 16, demo.top() + 16, 150, demo.height() - 32);
            QRectF list(demo.left() + 182, demo.top() + 16, demo.width() - 198, demo.height() - 32);
            if (m_step == 2) {
                // The panel taken to the middle: it keeps only its own height and the servers go under it.
                const qreal move = ease(span(.25, .7));
                const qreal left = demo.left() + 16 + (demo.center().x() - 75 - demo.left() - 16) * move;
                panel = QRectF(left, demo.top() + 16, 150, (demo.height() - 32) - 60 * move);
                const QRectF under(demo.center().x() - 120, panel.bottom() + 12, 240, demo.bottom() - 16 - panel.bottom() - 12);
                list = QRectF(list.left() + (under.left() - list.left()) * move, list.top() + (under.top() - list.top()) * move,
                              list.width() + (under.width() - list.width()) * move, list.height() + (under.height() - list.height()) * move);
                cursor = panel.center() + QPointF(0, -30);
                press = span(.15, .25) * (1 - span(.75, .85));
            }
            if (m_step == 0) {
                // The panel taken to the other side, the servers moving over to where it stood.
                const qreal move = ease(span(.25, .7));
                panel.moveLeft(demo.left() + 16 + (demo.width() - 32 - 150) * move);
                list.moveLeft(demo.left() + 182 - 166 * move);
                cursor = panel.center() + QPointF(0, -40);
                press = span(.15, .25) * (1 - span(.75, .85));
            }
            p.save(); p.setClipRect(list);
            for (int i = 0; i < 6; ++i) { p.setBrush(surface); p.drawRoundedRect(QRectF(list.left(), list.top() + i * 42, list.width(), 34), 9, 9); }
            p.restore();
            if (m_step == 1) {
                cursor = QPointF(panel.right() - 4, panel.top() + 6);
            }
            p.setBrush(surface);
            p.drawRoundedRect(panel, 14, 14);
            QPen dash(withAlpha(accent, 200), 1.6, Qt::DashLine);
            p.setPen(dash); p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(panel.adjusted(-4, -4, 4, 4), 16, 16);
            p.setPen(Qt::NoPen);
            // The connect button, smaller after its size button.
            const qreal shrink = m_step == 1 ? ease(span(.45, .6)) : 0;
            const qreal r = 44 - 12 * shrink;
            p.setBrush(accent);
            p.drawEllipse(QPointF(panel.center().x(), panel.top() + 70), r, r);
            // The two switches, always there.
            p.setBrush(surface);
            p.drawRoundedRect(QRectF(panel.left() + 10, panel.top() + 130, panel.width() - 20, 28), 8, 8);
            p.drawRoundedRect(QRectF(panel.left() + 10, panel.top() + 164, panel.width() - 20, 28), 8, 8);
            if (m_step == 1) {
                p.setBrush(accent);
                p.drawEllipse(QPointF(panel.right() - 4, panel.top() + 6), 11, 11);
                press = span(.38, .45) * (1 - span(.45, .52));
            }
            // The cursor: a filled arrow with a ring while it presses.
            p.setBrush(QColor(255, 255, 255, int(90 * press)));
            p.drawEllipse(cursor, 18, 18);
            QPainterPath arrow;
            arrow.moveTo(cursor);
            arrow.lineTo(cursor + QPointF(0, 20));
            arrow.lineTo(cursor + QPointF(5, 15));
            arrow.lineTo(cursor + QPointF(9, 23));
            arrow.lineTo(cursor + QPointF(12, 21));
            arrow.lineTo(cursor + QPointF(8, 14));
            arrow.lineTo(cursor + QPointF(14, 14));
            arrow.closeSubpath();
            p.setPen(QPen(Qt::black, 1.2));
            p.setBrush(Qt::white);
            p.drawPath(arrow);
            // The step's line and its place among the three.
            QFont font = p.font();
            font.setPixelSize(18);
            font.setBold(true);
            p.setFont(font);
            p.setPen(Qt::white);
            p.drawText(QRectF(0, demo.bottom() + 18, width(), 30), Qt::AlignCenter, m_text);
            p.setPen(Qt::NoPen);
            for (int i = 0; i < 3; ++i) {
                p.setBrush(i == m_step ? QColor(255, 255, 255) : QColor(255, 255, 255, 90));
                p.drawEllipse(QPointF(width() / 2.0 + (i - 1) * 16, demo.bottom() + 62), 3.5, 3.5);
            }
            m_skip->move(int(demo.left()), int(demo.bottom() + 82));
            m_next->move(int(demo.right()) - m_next->width(), int(demo.bottom() + 82));
        }
    private:
        void updateText() {
            static const char *lines[] = {QT_TRANSLATE_NOOP("NewHome", "Drag the panel to the other side"),
                                          QT_TRANSLATE_NOOP("NewHome", "Buttons on a block change its size"),
                                          QT_TRANSLATE_NOOP("NewHome", "In the middle the servers go under the panel")};
            m_text = QCoreApplication::translate("NewHome", lines[m_step]);
            m_next->setText(QCoreApplication::translate("NewHome", m_step == 2 ? "Done" : "Next"));
        }
        void finish() { if (m_closed) m_closed(); m_closed = nullptr; close(); }
        std::function<void()> m_closed;
        NewTile *m_skip, *m_next;
        QElapsedTimer m_clock;
        QString m_text;
        int m_step = 0;
    };
}

QSize NewHome::windowMinimum() {
    // Child layouts cache their own minimums; invalidating only the root keeps the previous arrangement's sizes.
    for (auto *part : findChildren<QLayout *>()) part->invalidate();
    m_controls->layout()->activate();
    m_sideLayout->activate();
    m_columns->invalidate();
    layout()->invalidate();
    layout()->activate();
    const auto outer = layout()->contentsMargins();
    QSize size = m_columns->minimumSize() + QSize(outer.left() + outer.right(),
        outer.top() + outer.bottom() + (m_titleBar && !m_titleBar->isHidden() ? m_titleBar->height() : 0));
    size.setHeight(std::max(size.height(), 480));
    if (const auto *display = screen())
        size.setHeight(std::min(size.height(), display->availableGeometry().height() * 3 / 4));
    // The centering margins follow the window's width: counted in, the window could never narrow.
    const QMargins room = m_columns->contentsMargins();
    size.rwidth() -= room.left() + room.right();
    const int tabsWidth = m_tabsBar->minimumWidth();
    const bool centered = m_columns->direction() == QBoxLayout::TopToBottom;
    const int contentWidth = centered ? tabsWidth : m_side->minimumWidth() + m_columns->spacing() + tabsWidth;
    size.setWidth(std::max(size.width(), contentWidth + outer.left() + outer.right()));
    return size;
}

void NewHome::updateCentering() {
    const bool centered = Configs::dataManager->settingsRepo->new_layout_panel == QLatin1String("center");
    const int contentWidth = std::max({480, m_bodyLayout->minimumSize().width(), fullTabBarWidth()});
    const auto padding = layout()->contentsMargins();
    // The scroll area is still at its old width when the home widget gets a resize event.
    const int available = width() - padding.left() - padding.right();
    const int margin = centered ? std::max(0, (available - contentWidth) / 2) : 0;
    m_columns->setContentsMargins(margin, 0, margin, 0);
}

int NewHome::fullTabBarWidth() const {
    if (!m_sendLogs) return 0;
    const int spacing = m_tabsBar->layout()->spacing();
    int required = (m_tabs.size() - 1) * spacing;
    if (!m_sendLogs->isHidden()) required += m_sendLogs->width() + spacing;
    for (const auto *tab : m_tabs) required += tab->tabLabelWidth();
    return required;
}

void NewHome::updateTabPresentation() {
    if (!m_sendLogs) return;
    const bool compact = fullTabBarWidth() > m_tabsBar->width();
    auto *tabs = static_cast<QBoxLayout *>(m_tabsBar->layout());
    for (qsizetype i = 0; i < m_tabs.size(); ++i) {
        m_tabs[i]->setCompactTab(compact);
        tabs->setStretch(i, compact ? 1 : 0);
    }
    tabs->setStretch(m_tabs.size(), compact ? 0 : 1);
    if (auto *spacer = tabs->itemAt(m_tabs.size())->spacerItem())
        spacer->changeSize(0, 0, compact ? QSizePolicy::Fixed : QSizePolicy::Expanding, QSizePolicy::Minimum);
    tabs->invalidate();
}

void NewHome::updateSubscriptionScroll() {
    if (!m_list || m_updatingSubscriptionScroll) return;
    const QScopedValueRollback guard(m_updatingSubscriptionScroll, true);
    const bool joined = Configs::dataManager->settingsRepo->new_layout_join_subscription;
    const int width = m_profileColumn->width();
    auto *layout = m_subscriptionHeader->layout();
    m_profileColumn->setMinimumWidth(layout->minimumSize().width());
    const int height = std::max(0, layout->hasHeightForWidth() ? layout->heightForWidth(width) : layout->sizeHint().height());
    const int rows = m_list->model()->rowCount();
    const int rowHeight = rows ? m_list->sizeHintForRow(0) : 0;
    const int page = m_profileColumn->height();
    const int maximum = joined ? std::max(0, height + rows * rowHeight - page) : 0;
    m_list->setJoinedScrollBar(joined ? m_subscriptionScroll : nullptr);
    m_subscriptionScroll->setPageStep(page);
    m_subscriptionScroll->setSingleStep(m_list->verticalScrollBar()->singleStep());
    m_subscriptionScroll->setRange(0, maximum);
    m_subscriptionScroll->setVisible(maximum > 0);
    m_subscriptionHeader->resize(width, height);
    scrollSubscription();
}

void NewHome::scrollSubscription() {
    const QScopedValueRollback guard(m_updatingSubscriptionScroll, true);
    const int height = m_subscriptionHeader->height();
    const int value = m_subscriptionScroll->value();
    const int offset = std::min(height, value);
    const int visibleHeight = height - offset;
    if (m_subscriptionClip->minimumHeight() != visibleHeight || m_subscriptionClip->isHidden() != (visibleHeight == 0)) {
        m_subscriptionClip->setFixedHeight(visibleHeight);
        m_subscriptionClip->setVisible(visibleHeight > 0);
        m_profileColumn->layout()->activate();
    }
    m_subscriptionHeader->move(0, -offset);
    if (Configs::dataManager->settingsRepo->new_layout_join_subscription) m_list->verticalScrollBar()->setValue(value - offset);
}

void NewHome::applyArrangement() {
    const auto &settings = *Configs::dataManager->settingsRepo;
    // Left or right of the servers, or above them in one narrow column as on the phone.
    const bool centered = settings.new_layout_panel == QLatin1String("center");
    m_columns->setDirection(centered ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    // The panel is one whole block: 300 px beside the servers, the full column width over them.
    m_columns->removeWidget(m_side);
    m_columns->insertWidget(settings.new_layout_panel == QLatin1String("right") ? 1 : 0, m_side);
    // Centered, the panel takes the column's whole width like the servers under it, with no empty sides.
    if (centered) {
        m_side->setMinimumWidth(0);
        m_side->setMaximumWidth(QWIDGETSIZE_MAX);
    } else {
        m_side->setFixedWidth(300);
    }
    updateCentering();
    // Inside it the order never changes: header, card, the connect block, room, the status lines.
    m_sideLayout->removeWidget(m_header);
    m_sideLayout->removeWidget(m_chipBox);
    m_sideLayout->removeWidget(m_controls);
    for (int i = m_sideLayout->count() - 1; i >= 0; --i) if (m_sideLayout->itemAt(i)->spacerItem()) delete m_sideLayout->takeAt(i);
    int at = 0;
    m_sideLayout->insertWidget(at++, m_header);
    m_sideLayout->insertWidget(at++, m_chipBox);
    m_sideLayout->insertWidget(at++, m_controls);
    // Beside the servers the room takes the panel's height; centered, the panel keeps only what it needs.
    if (!centered) m_sideLayout->insertStretch(at++, 1);
    m_chipBox->layout()->setContentsMargins(0, 14, 0, 0);
    updateSubscriptionScroll();
    m_orb->setFixedHeight(settings.new_layout_size == QLatin1String("compact") ? 148 : 190);
    // The window's smallest size follows the arrangement.
    if (m_attached) mw->applyTopBarMetrics();
}

void NewHome::setArranging(bool on) {
    if (!on) { delete m_arrange; m_arrange = nullptr; applyArrangement(); return; }
    if (m_arrange) return;
    auto &settings = *Configs::dataManager->settingsRepo;
    m_subscriptionScroll->setValue(0);
    const auto set = [this](QString &field, const QString &value) {
        field = value;
        Configs::dataManager->settingsRepo->Save();
        applyArrangement();
    };
    auto *overlay = new ArrangeOverlay(this, m_contentScroll, [this] { setArranging(false); }, [this] { new ArrangeTutorial(this, {}); });
    // The panel is the one block: dropped in the left, middle or right third of the window, it stands there and
    // the servers and tabs take the rest. The connect button only changes size.
    // Called on every move of the drag: only a change of third saves and rearranges.
    overlay->addBlock(m_side, [=, &settings](QPoint at) {
        const QString place = at.x() < width() / 3 ? QStringLiteral("left") : at.x() > width() * 2 / 3 ? QStringLiteral("right") : QStringLiteral("center");
        if (settings.new_layout_panel != place) set(settings.new_layout_panel, place);
    });
    overlay->rebuild = [=, &settings] {
        overlay->clearButtons();
        const bool compact = settings.new_layout_size == QLatin1String("compact");
        overlay->addButton(m_orb, compact ? QStringLiteral("⤢") : QStringLiteral("⤡"), T("Change size"),
                           [=, &settings] { set(settings.new_layout_size, compact ? QStringLiteral("normal") : QStringLiteral("compact")); });
        overlay->addButton(m_subscriptionClip, settings.new_layout_join_subscription ? QStringLiteral("↥") : QStringLiteral("↕"),
                           T("Scroll subscription info with servers"), [this, &settings] {
            settings.new_layout_join_subscription = !settings.new_layout_join_subscription;
            settings.Save();
            updateSubscriptionScroll();
        });
    };
    overlay->rebuild();
    overlay->setGeometry(rect());
    overlay->show();
    overlay->raise();
    m_arrange = overlay;
    if (settings.new_layout_tutorial != QLatin1String("seen")) new ArrangeTutorial(this, [] {
        Configs::dataManager->settingsRepo->new_layout_tutorial = QStringLiteral("seen");
        Configs::dataManager->settingsRepo->Save();
    });
}

void NewHome::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateCentering();
    if (window()->windowFlags() & Qt::FramelessWindowHint) NewStyle::roundCorners(window(), false);
}

void NewHome::paintEvent(QPaintEvent *) {
    QPainter p(this);
#ifdef Q_OS_WIN
    NewStyle::paintBackdrop(p, QRect(-mapTo(window(), QPoint()), window()->size()));
#else
    NewStyle::paintBackdrop(p, rect());
#endif
}

Qt::Edges NewHome::edgesAt(const QPoint &pos) const {
    if (!(window()->windowFlags() & Qt::FramelessWindowHint) || window()->isMaximized() || window()->isFullScreen()) return {};
    const int grip = 6;
    Qt::Edges edges;
    if (pos.x() <= grip) edges |= Qt::LeftEdge;
    if (pos.x() >= width() - grip) edges |= Qt::RightEdge;
    if (pos.y() <= grip) edges |= Qt::TopEdge;
    if (pos.y() >= height() - grip) edges |= Qt::BottomEdge;
    return edges;
}

void NewHome::mouseMoveEvent(QMouseEvent *e) {
    const Qt::Edges edges = edgesAt(e->pos());
    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) setCursor(Qt::SizeFDiagCursor);
    else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) setCursor(Qt::SizeBDiagCursor);
    else if (edges & (Qt::LeftEdge | Qt::RightEdge)) setCursor(Qt::SizeHorCursor);
    else if (edges & (Qt::TopEdge | Qt::BottomEdge)) setCursor(Qt::SizeVerCursor);
    else unsetCursor();
    QWidget::mouseMoveEvent(e);
}

void NewHome::mousePressEvent(QMouseEvent *e) {
    // Ignored presses bubble here from containers; labels and controls are not empty space.
    const QWidget *hit = childAt(e->pos());
    if (e->button() == Qt::LeftButton && !m_arrange && m_list && m_list->isVisible() &&
        (!hit || hit->metaObject() == &QWidget::staticMetaObject)) m_list->selectionModel()->clear();
    const Qt::Edges edges = edgesAt(e->pos());
    if (e->button() == Qt::LeftButton && window()->windowHandle()) {
        if (edges) window()->windowHandle()->startSystemResize(edges);
        // The empty margins move the window like the title bar.
        else if ((window()->windowFlags() & Qt::FramelessWindowHint) && e->pos().y() < 60) window()->windowHandle()->startSystemMove();
    }
    QWidget::mousePressEvent(e);
}

void NewHome::leaveEvent(QEvent *e) {
    unsetCursor();
    QWidget::leaveEvent(e);
}

bool NewHome::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_tabsBar && event->type() == QEvent::Resize) updateTabPresentation();
    if ((watched == m_profileColumn && event->type() == QEvent::Resize) ||
        (watched == m_subscriptionHeader && event->type() == QEvent::LayoutRequest)) {
        // Recalculate after the new width and the wrapped labels have been laid out.
        QTimer::singleShot(0, this, [this] { updateSubscriptionScroll(); });
    }
    if (watched == m_subscriptionClip && event->type() == QEvent::Wheel &&
        Configs::dataManager->settingsRepo->new_layout_join_subscription) {
        event->ignore();
        QApplication::sendEvent(m_list->viewport(), event);
        return event->isAccepted();
    }
    const bool subscription = watched == m_chip || watched == m_groupHeading || watched == m_groupName || watched == m_groupDate;
    if (subscription && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QPoint point = mouse->globalPosition().toPoint();
        if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
            m_subscriptionPress = point;
            m_subscriptionDragging = false;
        } else if (event->type() == QEvent::MouseMove && mouse->buttons().testFlag(Qt::LeftButton)) {
            const auto delta = point - m_subscriptionPress;
            if (qAbs(delta.x()) > QApplication::startDragDistance() && qAbs(delta.x()) > qAbs(delta.y())) m_subscriptionDragging = true;
            if (m_subscriptionDragging) { m_chip->setDown(false); return true; }
        } else if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton) {
            const auto delta = point - m_subscriptionPress;
            if (m_subscriptionDragging) {
                m_subscriptionDragging = false;
                m_chip->setDown(false);
                if (qAbs(delta.x()) >= 40 && qAbs(delta.x()) > qAbs(delta.y())) {
                    const auto order = Configs::dataManager->groupsRepo->GetGroupsTabOrder();
                    const int next = order.indexOf(Configs::dataManager->settingsRepo->current_group) + (delta.x() < 0 ? 1 : -1);
                    if (next >= 0 && next < order.size()) mw->ui->tabWidget->setCurrentIndex(groupId2TabIndex(order[next]));
                }
                return true;
            }
            if (watched != m_chip) { showGroupPicker(); return true; }
        }
        if (watched != m_chip) return true;
    }
    // A label ignores the press, so its release would go to the parent: the press is the click.
    if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        if (watched == m_check) { checkCurrentConnection(); return true; }
        if (watched == m_announce) { m_announceOpen = !m_announceOpen; refreshGroupHeader(); return true; }
        // The routing line opens the routing menu under it: the profiles, the provider's routing when it decides.
        if (watched == m_routeLine) {
            guardReopen(mw->ui->menuRouting_Menu, m_routeLine);
            mw->ui->menuRouting_Menu->popup(m_routeLine->mapToGlobal(QPoint(8, m_routeLine->height())));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------- switching interfaces

void MainWindow::applyInterfaceStyle(bool animated) {
    const bool wantNew = NewStyle::active();
    const auto coverName = QStringLiteral("rawlineThemeTransition");
    delete findChild<QLabel *>(coverName, Qt::FindDirectChildrenOnly);
    QLabel *cover = nullptr;
    // A switch that changes the native frame must repaint directly after the window is recreated.
    if (animated && wantNew && newHome && newHome->attached() && isVisible() && !isMinimized()) {
        cover = new QLabel(this);
        cover->setObjectName(coverName);
        cover->setAttribute(Qt::WA_TransparentForMouseEvents);
        cover->setPixmap(grab());
        cover->setGeometry(rect());
        cover->show();
        cover->raise();
    }
    if (!rootStack) {
        rootStack = new QStackedWidget;
        QWidget *classic = takeCentralWidget();
        rootStack->addWidget(classic);
#ifdef Q_OS_WIN
        auto *frame = new QWidget;
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *caption = new NewTitleBar(frame);
        caption->setContentsMargins(14, 0, 6, 0);
        caption->setObjectName(QStringLiteral("rawlineMainCaption"));
        caption->follow(this);
        layout->addWidget(caption);
        layout->addWidget(rootStack, 1);
        setCentralWidget(frame);
#else
        setCentralWidget(rootStack);
#endif
    }
    setAutoFillBackground(!wantNew);
    rootStack->setAutoFillBackground(!wantNew);
#ifdef Q_OS_WIN
    auto framePalette = qApp->palette();
    if (wantNew) framePalette.setColor(QPalette::Window, NewStyle::background());
    centralWidget()->setPalette(framePalette);
    centralWidget()->setAutoFillBackground(true);
    centralWidget()->findChild<QWidget *>(QStringLiteral("rawlineMainCaption"), Qt::FindDirectChildrenOnly)->setVisible(wantNew);
#endif
    // The hidden page must not hold the window to its own minimum: the new one goes down to a phone's width.
    if (QWidget *classic = rootStack->widget(0)) {
        classic->setSizePolicy(wantNew ? QSizePolicy::Ignored : QSizePolicy::Preferred, wantNew ? QSizePolicy::Ignored : QSizePolicy::Preferred);
        classic->setAutoFillBackground(!wantNew);
    }
    if (wantNew) {
        if (!newHome) {
            newHome = new NewHome(this);
            rootStack->addWidget(newHome);
        }
        newHome->attach();
        rootStack->setCurrentWidget(newHome);
        newHome->update();
    } else {
        if (newHome) newHome->detach();
        rootStack->setCurrentIndex(0);
    }
    applyTopBarMetrics();
    syncWindowFrame();
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

void MainWindow::syncWindowFrame() {
#ifdef Q_OS_WIN
    const bool custom = NewStyle::active();
    if (const auto id = internalWinId(); id && (property("rawlineCaptionWindow").toULongLong() != quint64(id) ||
                                               property("rawlineCustomCaption").toBool() != custom)) {
        setProperty("rawlineCaptionWindow", quint64(id));
        setProperty("rawlineCustomCaption", custom);
        recalculateWindowsFrame(this);
    } else {
        NewStyle::themeFrame(this);
    }
#else
    const bool frameless = NewStyle::active();
    if (bool(windowFlags() & Qt::FramelessWindowHint) == frameless) return;
    // Changing the frame re-creates the window and would hide an open dialog with it: after the dialog closes.
    // Only a dialog window counts: a dialog class shown inside another one never finishes on its own.
    for (auto *dialog : findChildren<QDialog *>()) {
        if (!dialog->isWindow() || !dialog->isVisible()) continue;
        connect(dialog, &QDialog::finished, this, [this] { syncWindowFrame(); }, Qt::SingleShotConnection);
        return;
    }
    const bool shown = isVisible();
    const bool maximized = isMaximized();
    setWindowFlag(Qt::FramelessWindowHint, frameless);
    if (!frameless) clearMask();
    if (shown) maximized ? showMaximized() : show();
    if (frameless) NewStyle::roundCorners(this, false);
#endif
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    if (NewStyle::active() && handleWindowsFrame(this, static_cast<const MSG *>(message), result)) return true;
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::arrangeWindow() {
    if (newHome && newHome->attached()) newHome->arrange();
}
