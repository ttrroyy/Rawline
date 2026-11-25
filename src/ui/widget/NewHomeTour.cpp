// --ui-tour <dir>: walks the new interface in each of its themes and saves what the screen shows, for CI.
// The whole screen is grabbed, so window corners, shadows and translucency come out as the user sees them.
#include "include/ui/widget/NewHome.hpp"

#include "include/ui/mainwindow.h"
#include "include/ui/profile/dialog_edit_profile.h"
#include "include/ui/utils/ProfilesTableModel.h"
#include "include/ui/setting/ThemeManager.hpp"
#include "include/ui/setting/dialog_basic_settings.h"
#include "include/ui/widget/StartStopButton.hpp"
#include "include/configs/sub/GroupUpdater.hpp"
#include "include/database/SettingsRepo.h"
#include "include/engine/EngineHost.h"

#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTextStream>
#include <QMenu>
#include <QMouseEvent>
#include <QFontMetrics>
#include "include/database/GroupsRepo.h"
#include <QPointer>
#include <QPushButton>
#include <QStyleFactory>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QScrollArea>
#include <QWheelEvent>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <qt_windows.h>
#endif

namespace {
    // Sample servers on documentation addresses: rows to look at, nothing to connect to.
    const char *kSampleLinks =
        "vless://11111111-1111-1111-1111-111111111111@203.0.113.10:443?security=reality&type=tcp&sni=example.com&pbk=YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXoxMjM0NTY&sid=01#%F0%9F%87%B8%F0%9F%87%AA%20se-tcp-reality\n"
        "vless://11111111-1111-1111-1111-111111111111@203.0.113.11:443?security=tls&type=xhttp&sni=example.com#%F0%9F%87%B7%F0%9F%87%BA%20ru-xhttp-tls\n"
        "hysteria2://secret@203.0.113.12:443?sni=example.com#%F0%9F%87%A9%F0%9F%87%AA%20de-hy2\n"
        "ss://Y2hhY2hhMjAtaWV0Zi1wb2x5MTMwNTpzZWNyZXQ@203.0.113.13:8388#nl-ss%20%F0%9F%8F%B3%EF%B8%8F\n";

    struct Tour {
        MainWindow *mw;
        QDir dir;
        QList<std::pair<QString, std::function<void()>>> steps;
        int at = 0;
        QString theme;

        void shoot(const QString &name) {
            const QString file = dir.filePath(QStringLiteral("%1-%2-%3.png").arg(theme).arg(at, 3, 10, QLatin1Char('0')).arg(name));
            if (QScreen *screen = QGuiApplication::primaryScreen()) screen->grabWindow(0).save(file);
        }
        // Everything the step opened goes away before the next one: popups, menus and dialog windows.
        void clear() {
            for (int pass = 0; pass < 3; ++pass) {
                if (QWidget *popup = QApplication::activePopupWidget()) { popup->close(); continue; }
                bool closed = false;
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (w == mw || !w->isVisible()) continue;
                    if (auto *dialog = qobject_cast<QDialog *>(w)) dialog->reject();
                    else w->close();
                    closed = true;
                }
                if (!closed) break;
            }
        }
    };

    void next(const std::shared_ptr<Tour> &tour);

    void run(const std::shared_ptr<Tour> &tour) {
        if (tour->at >= tour->steps.size()) {
            if (auto *host = Engine::host()) host->shutdown();
            QCoreApplication::exit(0);
            return;
        }
        const auto &[name, open] = tour->steps[tour->at];
        if (name == QLatin1String("theme-switch-regression")) {
            open();
            tour->shoot(name);
            tour->clear();
            next(tour);
            return;
        }
        // Armed first: a step that opens a modal dialog returns only once the dialog is closed.
        QTimer::singleShot(1400, qApp, [tour, name = name] {
            if (name == QLatin1String("edit-freeturn")) {
                DialogEditProfile *dialog = nullptr;
                for (auto *widget : QApplication::topLevelWidgets()) {
                    if (auto *editor = qobject_cast<DialogEditProfile *>(widget); editor && editor->isVisible()) {
                        dialog = editor;
                        break;
                    }
                }
                auto *box = dialog ? dialog->findChild<QDialogButtonBox *>() : nullptr;
                auto *ok = box ? box->button(QDialogButtonBox::Ok) : nullptr;
                if (!ok || !ok->isVisible() ||
                    !dialog->screen()->availableGeometry().contains(QRect(ok->mapToGlobal(QPoint()), ok->size())))
                    qFatal("The FreeTurn save button is outside the available screen");
            }
            if (!name.isEmpty()) tour->shoot(name);
            tour->clear();
            next(tour);
        });
        open();
    }

    void next(const std::shared_ptr<Tour> &tour) {
        ++tour->at;
        QTimer::singleShot(500, qApp, [tour] { run(tour); });
    }
}

void NewHome::runTour(MainWindow *mw, const QString &dir) {
    auto tour = std::make_shared<Tour>();
    tour->mw = mw;
    QDir().mkpath(dir);
    tour->dir = QDir(dir);
    auto &s = tour->steps;
    const auto action = [](QAction *a) { return [a] { a->trigger(); }; };
    const auto home = [mw] { return mw->newHome; };

    s << std::pair{QString(), std::function<void()>([tour] {
        tour->clear();
        auto name = QStringLiteral("Server \U0001F3F3\uFE0F Example \U0001F1E9\U0001F1EA");
        const auto white = NewStyle::takeProfileFlag(name);
        if (white != QStringLiteral("\U0001F3F3\uFE0F") || name != QStringLiteral("Server Example \U0001F1E9\U0001F1EA") ||
            NewStyle::flag(white, 28, 1).toImage().pixelColor(13, 10) != QColor(Qt::white))
            qFatal("The white flag did not move from the name into the badge");
        name = QStringLiteral("Pride \U0001F3F3\uFE0F\u200D\U0001F308Server");
        if (NewStyle::takeProfileFlag(name) != QStringLiteral("\U0001F3F3\uFE0F\u200D\U0001F308") || name != QStringLiteral("Pride Server"))
            qFatal("A joined flag swallowed part of the profile name");
        Subscription::updater()->ImportText(QString::fromUtf8(kSampleLinks));
        // Local sample data only: display the subscription routing row without fetching a subscription.
        if (const auto group = Configs::dataManager->groupsRepo->CurrentGroup()) group->url = QStringLiteral("https://subscription.example.invalid");
    })};
    for (const QString theme : {QString::fromLatin1(NewStyle::OledTheme), QString::fromLatin1(NewStyle::WhiteTheme),
                                QString::fromLatin1(NewStyle::PastelTheme), QString::fromLatin1(NewStyle::PastelLightTheme),
                                QString::fromLatin1(NewStyle::GreyTheme)}) {
        s << std::pair{QString(), std::function<void()>([tour, mw, theme] {
            tour->theme = theme;
            Configs::dataManager->settingsRepo->theme = theme;
            Configs::dataManager->settingsRepo->new_layout_panel = QStringLiteral("left");
            themeManager()->ApplyTheme(theme);
            mw->showNormal();
            const auto area = mw->screen()->availableGeometry().adjusted(12, 12, -12, -12);
            mw->resize(QSize(1000, 700).boundedTo(area.size() - (mw->frameSize() - mw->size())));
            mw->move(area.topLeft());
            mw->raise();
            mw->activateWindow();
        })};
        s << std::pair{QStringLiteral("home"), std::function<void()>([home] {
            if (auto *h = home(); h && h->m_list->model() && h->m_list->model()->rowCount() > 1) h->m_list->setCurrentIndex(h->m_list->model()->index(1, 0));
        })};
#ifdef Q_OS_WIN
        s << std::pair{QString(), std::function<void()>([mw] {
            const auto *caption = mw->findChild<QWidget *>(QStringLiteral("rawlineMainCaption"));
            if (!caption || !caption->isVisible()) qFatal("The Rawline title bar is not visible");
            const auto buttons = caption->findChildren<QAbstractButton *>(QString(), Qt::FindDirectChildrenOnly);
            if (buttons.size() != 3) qFatal("The window caption is missing its controls");
            const auto hwnd = reinterpret_cast<HWND>(mw->winId());
            buttons[1]->click();
            if (!IsZoomed(hwnd)) qFatal("The caption maximize command did not maximize");
            buttons[1]->click();
            if (IsZoomed(hwnd)) qFatal("The caption restore command did not restore");
            buttons[0]->click();
            if (!IsIconic(hwnd)) qFatal("The caption minimize command did not minimize");
            SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            if (IsIconic(hwnd)) qFatal("The shell restore command did not restore");
        })};
#endif
        s << std::pair{QStringLiteral("switching-orb"), std::function<void()>([home, mw] {
            auto *button = mw->ui->toolButton_startstop;
            button->setState(StartStopButton::State::Disconnecting);
            if (home()->m_orb->isEnabled()) qFatal("Connection button did not react immediately to stopping");
            button->setState(StartStopButton::State::Connecting);
            if (!home()->m_orb->isEnabled()) qFatal("Connection button did not react immediately to switching");
        })};
        s << std::pair{QString(), std::function<void()>([mw] { mw->refresh_startstop_button(); })};
        s << std::pair{QStringLiteral("empty-space-selection"), std::function<void()>([home, mw] {
            auto *h = home();
            const auto check = [](bool ok, const char *message) { if (!ok) qFatal("Selection regression: %s", message); };
            auto *selection = h->m_list->selectionModel();
            const auto row = h->m_list->model()->index(1, 0);
            check(row.isValid(), "sample profile missing");
            const int running = mw->runningSnapshot() ? mw->runningSnapshot()->id : -1;
            const auto choose = [&] { selection->select(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); };
            const auto click = [](QWidget *widget, QPoint point) {
                const QPointF global = widget->mapToGlobal(point);
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(widget, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(widget, &release);
            };
            choose();
            click(h->m_more, h->m_more->rect().center());
            if (auto *popup = QApplication::activePopupWidget()) popup->close();
            check(selection->hasSelection(), "clicking a control cleared the selected profile");
            const QPoint blank(h->width() / 2, h->height() - 12);
            check(!h->childAt(blank), "sample background point is not empty");
            click(h, blank);
            check(!selection->hasSelection(), "empty home background kept selection");
            choose();
            const QPoint gap(1, h->m_list->visualRect(row).center().y());
            click(h->m_list->viewport(), gap);
            check(!selection->hasSelection(), "empty tile margin kept selection");
            choose();
            const QPoint below(h->m_list->viewport()->width() / 2, h->m_list->viewport()->height() - 1);
            check(!h->m_list->indexAt(below).isValid(), "sample list has no empty area");
            click(h->m_list->viewport(), below);
            check(!selection->hasSelection(), "empty list background kept selection");
            check((mw->runningSnapshot() ? mw->runningSnapshot()->id : -1) == running, "clearing view selection changed the running profile");
            check(h->m_routeBox->isVisible(), "subscription routing row missing");
            check(h->m_routeBox->findChildren<QAbstractButton *>().size() == 1, "routing icon missing");
        })};
        s << std::pair{QStringLiteral("groups"), std::function<void()>([home] { home()->showGroupPicker(); })};
        s << std::pair{QStringLiteral("fixed-subscription-picker"), std::function<void()>([home] {
            auto *h = home();
            Configs::dataManager->settingsRepo->new_layout_chip = QStringLiteral("hidden");
            h->applyArrangement();
            QApplication::processEvents();
            if (!h->m_chipBox->isVisible() || !h->m_add->isVisible() || !h->m_chipBox->isAncestorOf(h->m_add))
                qFatal("The saved desktop layout hid the subscription picker");
            if (h->m_contentScroll->verticalScrollBar()->isVisible())
                qFatal("The outer scroll bar is visible");
        })};
        s << std::pair{QStringLiteral("joined-subscription-scroll"), std::function<void()>([home] {
            auto *h = home();
            auto &settings = *Configs::dataManager->settingsRepo;
            settings.new_layout_join_subscription = true;
            h->m_profileColumn->setFixedHeight(175);
            QApplication::processEvents();
            h->updateSubscriptionScroll();
            const int fullHeight = h->m_subscriptionClip->height();
            auto *viewport = h->m_list->viewport();
            const QPoint p = viewport->rect().center();
            QWheelEvent wheel(p, viewport->mapToGlobal(p), QPoint(0, -16), {}, Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate, false);
            QApplication::sendEvent(viewport, &wheel);
            if (h->m_subscriptionClip->height() != fullHeight - 16 || h->m_list->verticalScrollBar()->value() != 0)
                qFatal("Joined scroll did not move the subscription first");
            h->m_subscriptionScroll->setValue(h->m_subscriptionScroll->maximum());
            if (h->m_subscriptionClip->isVisible() || h->m_list->verticalScrollBar()->value() <= 0)
                qFatal("Joined scroll did not continue through the profiles");
            settings.new_layout_join_subscription = false;
            h->m_profileColumn->setMinimumHeight(0);
            h->m_profileColumn->setMaximumHeight(QWIDGETSIZE_MAX);
            h->updateSubscriptionScroll();
            h->m_list->scrollToTop();
            if (!h->m_subscriptionClip->isVisible()) qFatal("Detached subscription did not return");
        })};
        s << std::pair{QStringLiteral("fixed-mode-switches"), std::function<void()>([home] {
            auto *h = home();
            h->applyArrangement();
            if (!h->m_tun->isVisible() || !h->m_proxy->isVisible() ||
                h->m_switches->indexOf(h->m_tun) != 0 || h->m_switches->indexOf(h->m_proxy) != 1)
                qFatal("Both mode switches must stay visible in their fixed order");
        })};
        s << std::pair{QStringLiteral("more-menu"), std::function<void()>([home] {
            home()->showMoreMenu();
            QApplication::processEvents();
            const auto *popup = QApplication::activePopupWidget();
            if (!popup || popup->mask().boundingRect() != popup->rect() || popup->mask().contains(QPoint(0, 0)))
                qFatal("The menu mask is clipped or has an uncovered square corner");
        })};
        s << std::pair{QStringLiteral("settings-menu"), std::function<void()>([mw] {
            mw->ui->menu_preferences->popup(mw->mapToGlobal(QPoint(380, 90)));
            QApplication::processEvents();
            const auto *popup = QApplication::activePopupWidget();
            if (!popup || popup->mask().boundingRect() != popup->rect() || popup->mask().contains(QPoint(0, 0)))
                qFatal("The settings menu mask does not match its visible size");
        })};
        s << std::pair{QStringLiteral("add-menu"), std::function<void()>([home] { home()->showAddMenu(); })};
        s << std::pair{QStringLiteral("group-menu"), std::function<void()>([home] { home()->showGroupMenu(); })};
        s << std::pair{QStringLiteral("server-menu"), std::function<void()>([mw] { mw->ui->menu_server->popup(QPoint(400, 300)); })};
        s << std::pair{QStringLiteral("tray-menu"), std::function<void()>([mw] { mw->trayMenu->popup(QPoint(600, 300)); })};
        for (int tab = 1; tab <= 4; ++tab) {
            s << std::pair{QStringLiteral("tab%1").arg(tab), std::function<void()>([home, tab] { home()->selectTab(tab, false); })};
        }
        s << std::pair{QStringLiteral("arrange"), std::function<void()>([home] { home()->selectTab(0, false); home()->setArranging(true); })};
        s << std::pair{QStringLiteral("arrange-plain"), std::function<void()>([home] {
            for (auto *w : home()->findChildren<QWidget *>(QStringLiteral("arrangeTutorial"))) w->close();
            Configs::dataManager->settingsRepo->new_layout_tutorial = QStringLiteral("seen");
        })};
        s << std::pair{QStringLiteral("narrow-center"), std::function<void()>([home, mw, dir] {
            home()->setArranging(false);
            for (const QString panel : {QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("center")}) {
                Configs::dataManager->settingsRepo->new_layout_panel = panel;
                home()->applyArrangement();
                mw->resize(1000, 700);
                QApplication::processEvents();
                if (home()->m_tabsBar->width() >= home()->fullTabBarWidth() &&
                    std::any_of(home()->m_tabs.cbegin(), home()->m_tabs.cend(), [](const auto *tab) { return tab->compactTab(); }))
                    qFatal("The full tab titles did not return on widening");
                const int frameHeight = mw->frameSize().height() - mw->height();
                mw->resize(380, std::min(760, mw->screen()->availableGeometry().height() - 24 - frameHeight));
                QApplication::processEvents();
                mw->applyTopBarMetrics();
                QApplication::processEvents();
                auto *h = home();
                QFile trace(QDir(dir).filePath("layout-geometry.txt"));
                if (trace.open(QIODevice::WriteOnly | QIODevice::Append)) {
                    QTextStream out(&trace);
                    out << panel << " window=" << mw->width() << 'x' << mw->height()
                        << " homeHeight=" << h->height() << " minimumHeight=" << h->minimumHeight()
                        << " sideY=" << h->m_side->y() << " sideHeight=" << h->m_side->height()
                        << " bodyY=" << h->m_bodyLayout->geometry().top()
                        << " tabs=" << h->m_tabsBar->width() << '/' << h->fullTabBarWidth();
                    for (const auto *tab : h->m_tabs) out << ' ' << tab->x() << ':' << tab->width() << ':' << tab->compactTab();
                    out << '\n';
                }
                const auto bounds = [h](QWidget *widget) { return QRect(widget->mapTo(h, QPoint()), widget->size()); };
                const QRect orb = bounds(h->m_orb), tun = bounds(h->m_tun), proxy = bounds(h->m_proxy);
                auto body = h->m_bodyLayout->geometry();
                body.moveTopLeft(h->m_side->parentWidget()->mapTo(h, body.topLeft()));
                if (bounds(h->m_side).intersects(body))
                    qFatal("Side panel overlaps the body in %s: window=%dx%d side=%d bodyTop=%d minimumHeight=%d",
                           qPrintable(panel), mw->width(), mw->height(), h->m_side->height(),
                           h->m_bodyLayout->geometry().top(), h->windowMinimum().height());
                if (mw->height() > mw->screen()->availableGeometry().height())
                    qFatal("The main window exceeds the available screen height in %s", qPrintable(panel));
                if (orb.intersects(tun) || (h->m_proxy->isVisible() && (orb.intersects(proxy) || tun.intersects(proxy))) ||
                    bounds(h->m_controls).intersects(bounds(h->m_running)) || bounds(h->m_controls).intersects(bounds(h->m_inbound)))
                    qFatal("Connect controls overlap in %s", qPrintable(panel));
                const QFontMetrics metrics(NewStyle::font(14, QFont::Medium));
                const bool needsIcons = h->fullTabBarWidth() > h->m_tabsBar->width();
                for (const auto *tab : h->m_tabs) {
                    if (tab->compactTab() != needsIcons || tab->toolTip() != tab->text() ||
                        (!needsIcons && tab->width() < metrics.horizontalAdvance(tab->text()) + 20))
                        qFatal("Tab labels or icons do not fit in %s", qPrintable(panel));
                }
                if (needsIcons && h->m_sendLogs->isHidden()) {
                    const auto *first = h->m_tabs.first();
                    const auto *last = h->m_tabs.last();
                    if (first->geometry().left() != 0 || last->geometry().right() != h->m_tabsBar->width() - 1 ||
                        std::any_of(h->m_tabs.cbegin(), h->m_tabs.cend(),
                                    [first](const auto *tab) { return std::abs(tab->width() - first->width()) > 1; }))
                        qFatal("Compact tabs do not fill the row evenly in %s", qPrintable(panel));
                }
                const auto viewportBounds = bounds(h->m_contentScroll->viewport());
                const auto checkWidth = [&](QWidget *widget) {
                    const auto r = bounds(widget);
                    if (r.left() < viewportBounds.left() || r.right() > viewportBounds.right()) {
                        const auto *button = qobject_cast<QAbstractButton *>(widget);
                        const auto label = button ? button->text() : widget->objectName();
                        qFatal("Control '%s' is clipped in %s: x=%d..%d viewport=%d..%d", qPrintable(label),
                               qPrintable(panel), r.left(), r.right(), viewportBounds.left(), viewportBounds.right());
                    }
                };
                for (auto *widget : QList<QWidget *>{h->m_chip, h->m_add, h->m_tun, h->m_proxy, h->m_groupMore}) checkWidth(widget);
                for (auto *tab : h->m_tabs) checkWidth(tab);
                h->selectTab(1, false);
                QApplication::processEvents();
                if (h->m_sendLogs->isVisible()) checkWidth(h->m_sendLogs);
                h->selectTab(0, false);
                QApplication::processEvents();
                h->setArranging(true);
                QApplication::processEvents();
                const auto buttons = h->m_arrange->findChildren<QAbstractButton *>(QStringLiteral("arrangeBlockButton"));
                if (buttons.size() != 2) qFatal("Layout editor is missing its block controls");
                const QList<QWidget *> anchors{h->m_orb, h->m_subscriptionClip};
                auto *outerScroll = h->m_contentScroll->verticalScrollBar();
                bool reached[2] = {};
                for (const int offset : {0, outerScroll->maximum() / 2, outerScroll->maximum()}) {
                    outerScroll->setValue(offset);
                    QApplication::processEvents();
                    for (int i = 0; i < 2; ++i) {
                        if (!buttons[i]->isVisible()) continue;
                        reached[i] = true;
                        const auto allowed = bounds(anchors[i]) & bounds(h->m_contentScroll->viewport()) & h->rect();
                        if (!allowed.contains(bounds(buttons[i]))) qFatal("A layout control escaped its block");
                    }
                    if (buttons[0]->isVisible() && buttons[1]->isVisible() && buttons[0]->geometry().intersects(buttons[1]->geometry()))
                        qFatal("Layout controls overlap");
                }
                if (!reached[0] || !reached[1]) qFatal("A layout control cannot be reached after scrolling");
                outerScroll->setValue(0);
                h->setArranging(false);
            }
        })};
        s << std::pair{QString(), std::function<void()>([home, mw] {
            Configs::dataManager->settingsRepo->new_layout_panel = QStringLiteral("left");
            home()->applyArrangement();
            const auto area = mw->screen()->availableGeometry().adjusted(12, 12, -12, -12);
            mw->resize(QSize(1000, 700).boundedTo(area.size() - (mw->frameSize() - mw->size())));
        })};
        for (const auto &[name, a] : {std::pair{"add-input", mw->ui->menu_add_from_input}, std::pair{"manage-groups", mw->ui->menu_manage_groups},
                 std::pair{"new-group", mw->ui->actionAdd_New_Group}, std::pair{"edit-group", mw->ui->actionEdit_Group},
                 std::pair{"routing", mw->ui->menu_routing_settings}, std::pair{"vpn", mw->ui->menu_vpn_settings},
                 std::pair{"vk-relay", mw->ui->menu_vk_relay_settings}, std::pair{"presets", mw->ui->menu_preset_settings},
                 std::pair{"integration", mw->ui->menu_integration_settings}, std::pair{"otp", mw->ui->menu_otp_manager},
                 std::pair{"scanner", mw->ui->menu_scanner}, std::pair{"traffic", mw->ui->actionTraffic_Stats},
                 std::pair{"about", mw->ui->actionAbout}, std::pair{"find-settings", mw->ui->menu_find_settings}}) {
            s << std::pair{QString::fromLatin1(name), std::function<void()>(action(a))};
        }
        s << std::pair{QStringLiteral("profile-edge-fades"), std::function<void()>([home] {
            auto *list = home()->m_list;
            list->setFixedHeight(145);
            QApplication::processEvents();
            auto *bar = list->verticalScrollBar();
            if (bar->maximum() <= bar->minimum()) qFatal("Profile fade test has no scrollable rows");
            bar->setValue((bar->minimum() + bar->maximum()) / 2);
        })};
        s << std::pair{QStringLiteral("edit-profile"), std::function<void()>([home, mw] {
            auto *list = home()->m_list;
            list->setMinimumHeight(0);
            list->setMaximumHeight(QWIDGETSIZE_MAX);
            list->scrollToTop();
            QApplication::processEvents();
            const auto index = list->model()->index(0, ProfilesTableModel::ColName);
            if (!index.isValid()) qFatal("Profile double-click test has no profile");
            const QPointF pos = list->visualRect(index).center();
            const QPointF global = list->viewport()->mapToGlobal(pos.toPoint());
            const auto send = [&](QEvent::Type type, Qt::MouseButtons buttons) {
                QMouseEvent event(type, pos, global, Qt::LeftButton, buttons, Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &event);
            };
            const auto originalActivation = list->activateRequested;
            int activations = 0;
            list->activateRequested = [&](const QModelIndex &) { ++activations; };
            send(QEvent::MouseButtonPress, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, Qt::NoButton);
            send(QEvent::MouseButtonDblClick, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, Qt::NoButton);
            QEventLoop settle;
            QTimer::singleShot(QApplication::doubleClickInterval() + 50, &settle, &QEventLoop::quit);
            settle.exec();
            list->activateRequested = originalActivation;
            if (activations != 0) qFatal("Double-click started the profile");
            const auto dialogs = mw->findChildren<DialogEditProfile *>();
            const auto editor = std::find_if(dialogs.cbegin(), dialogs.cend(), [](const auto *dialog) { return dialog->isVisible(); });
            if (editor == dialogs.cend() ||
                std::count_if(dialogs.cbegin(), dialogs.cend(), [](const auto *dialog) { return dialog->isVisible(); }) != 1)
                qFatal("Double-click did not open exactly one profile editor");
            const auto frame = (*editor)->frameGeometry();
            const auto owner = mw->frameGeometry();
            const auto screen = mw->screen()->availableGeometry();
            if (!screen.contains(frame) || !owner.contains(frame.center()) || qAbs(frame.center().x() - owner.center().x()) > 24)
                qFatal("Editor %dx%d at (%d,%d), owner %dx%d at (%d,%d), screen %dx%d at (%d,%d)",
                       frame.width(), frame.height(), frame.x(), frame.y(), owner.width(), owner.height(), owner.x(), owner.y(),
                       screen.width(), screen.height(), screen.x(), screen.y());
        })};
        s << std::pair{QStringLiteral("edit-freeturn"), std::function<void()>([mw] {
            auto *editor = new DialogEditProfile("freeturn", Configs::dataManager->settingsRepo->current_group, mw);
            editor->setAttribute(Qt::WA_DeleteOnClose);
        })};
        // The settings dialog page by page: it holds most of the program's fields.
        s << std::pair{QStringLiteral("settings"), std::function<void()>(action(mw->ui->menu_basic_settings))};
        for (int page = 1; page < 10; ++page) {
            s << std::pair{QStringLiteral("settings-page%1").arg(page), std::function<void()>([tour, page] {
                QTimer::singleShot(0, qApp, [tour, page] {
                    tour->mw->ui->menu_basic_settings->trigger();
                    QTimer::singleShot(600, qApp, [page] {
                        for (QWidget *w : QApplication::topLevelWidgets()) {
                            if (!w->isVisible() || !qobject_cast<QDialog *>(w)) continue;
                            if (auto *tabs = w->findChild<QTabWidget *>(); tabs && page < tabs->count()) tabs->setCurrentIndex(page);
                        }
                    });
                });
            })};
        }
        s << std::pair{QStringLiteral("theme-switch-regression"), std::function<void()>([theme, mw, tour] {
            const auto check = [](bool ok, const char *message) { if (!ok) qFatal("Theme regression: %s", message); };
            check(!NewStyle::styleSheet().contains(QRegularExpression("%[0-9]+")), "unresolved palette placeholder");
            QDialog dialog(mw);
            const auto flags = dialog.windowFlags();
            const QMargins margins(7, 9, 11, 13);
            dialog.setContentsMargins(margins);
            QPointer<QPushButton> child = new QPushButton(QStringLiteral("Regression"), &dialog);
            dialog.show();
            QApplication::processEvents();
#ifdef Q_OS_WIN
            const auto mainId = mw->winId();
            const auto dialogId = dialog.winId();
            const auto hasCaption = [](WId id) {
                const auto style = GetWindowLongPtrW(reinterpret_cast<HWND>(id), GWL_STYLE);
                return (style & WS_CAPTION) == WS_CAPTION;
            };
            const auto hasRoundedRegion = [](WId id) {
                const HRGN region = CreateRectRgn(0, 0, 0, 0);
                if (!region) return false;
                const bool rounded = GetWindowRgn(reinterpret_cast<HWND>(id), region) == COMPLEXREGION;
                DeleteObject(region);
                return rounded;
            };
            const auto checkClientFrame = [&](WId id, bool custom) {
                const auto hwnd = reinterpret_cast<HWND>(id);
                RECT window{};
                POINT client{};
                check(GetWindowRect(hwnd, &window) && ClientToScreen(hwnd, &client) &&
                      ((client.x == window.left && client.y == window.top) == custom),
                      "the Windows frame did not follow the selected theme");
            };
            const auto checkMainCaption = [&](bool custom) {
                const auto *caption = mw->findChild<QWidget *>(QStringLiteral("rawlineMainCaption"));
                check(caption && caption->isVisible() == custom && caption->findChildren<QAbstractButton *>().size() == 3,
                      "the wrong caption is visible");
                check(hasCaption(mainId) != custom, "the native main-window caption has the wrong state");
                checkClientFrame(mainId, custom);
            };
            const auto checkDialogCaption = [&](bool custom) {
                const auto *caption = dialog.findChild<QWidget *>(QStringLiteral("newDialogBar"));
                check(bool(caption && caption->isVisible()) == custom, "the dialog has the wrong title bar");
                check(hasCaption(dialogId) != custom, "the native dialog caption has the wrong state");
                checkClientFrame(dialogId, custom);
            };
            checkMainCaption(true);
            checkDialogCaption(true);
            check(hasRoundedRegion(mainId) && hasRoundedRegion(dialogId), "new Windows frame is still rectangular");
            const auto previousTitle = mw->windowTitle();
            mw->setWindowTitle(QStringLiteral("Rawline caption regression"));
            dialog.setWindowTitle(QStringLiteral("Dialog caption regression"));
            for (const auto id : {mainId, dialogId}) {
                const auto hwnd = reinterpret_cast<HWND>(id);
                SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_CAPTION);
            }
            QApplication::processEvents();
            checkMainCaption(true);
            checkDialogCaption(true);
            mw->setWindowTitle(previousTitle);
#else
            check(dialog.testAttribute(Qt::WA_TranslucentBackground), "new dialog must have a translucent surface");
#endif
            const QStringList classicThemes = QStringList{"System", "QDarkStyle", "FlatGray", "LightBlue", "SoftPink", "BlackSoft"}
                + QStyleFactory::keys();
            for (const auto &classic : classicThemes) {
                themeManager()->ApplyTheme(classic);
                QApplication::processEvents();
                check(child && dialog.windowFlags() == flags, "classic dialog flags or children changed");
#ifdef Q_OS_WIN
                check(mw->winId() == mainId && dialog.winId() == dialogId && hasCaption(mainId) && hasCaption(dialogId),
                      "switching with settings open changed the Windows frame");
                checkMainCaption(false);
                checkDialogCaption(false);
                check(!hasRoundedRegion(mainId) && !hasRoundedRegion(dialogId), "Rawline rounding survived the switch to classic");
#endif
                check(dialog.contentsMargins() == margins, "title bar space leaked into classic layout");
                check(!dialog.testAttribute(Qt::WA_TranslucentBackground) && !dialog.testAttribute(Qt::WA_NoSystemBackground), "classic dialog still transparent");
                themeManager()->ApplyTheme(theme);
                QApplication::processEvents();
#ifdef Q_OS_WIN
                check(mw->winId() == mainId && dialog.winId() == dialogId,
                      "switching back changed the Windows frame");
                check(hasRoundedRegion(mainId) && hasRoundedRegion(dialogId), "Rawline rounding did not return");
                checkMainCaption(true);
                checkDialogCaption(true);
#else
                check(dialog.property("newFrame").toBool(), "existing dialog did not recover the new frame");
#endif
            }
            dialog.close();
#ifdef Q_OS_WIN
            {
                DialogBasicSettings settings(mw);
                settings.show();
                auto *picker = settings.findChild<QComboBox *>(QStringLiteral("theme"));
                check(picker && picker->findText(QStringLiteral("System")) >= 0, "settings theme selector missing");
                picker->setCurrentIndex(picker->findText(QStringLiteral("System")));
                QApplication::processEvents();
                check(settings.isVisible() && hasCaption(settings.winId()) && hasCaption(mainId) && mw->winId() == mainId,
                      "System lost its native frame while settings remained open");
                using DwmGetAttribute = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
                const HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
                const auto getDwmAttribute = dwm
                    ? reinterpret_cast<DwmGetAttribute>(GetProcAddress(dwm, "DwmGetWindowAttribute")) : nullptr;
                BOOL mainComposed = FALSE, dialogComposed = FALSE;
                if (getDwmAttribute && SUCCEEDED(getDwmAttribute(reinterpret_cast<HWND>(mainId),
                    1 /* DWMWA_NCRENDERING_ENABLED */, &mainComposed, sizeof mainComposed)) && mainComposed) {
                    const auto hwnd = reinterpret_cast<HWND>(settings.winId());
                    const auto result = getDwmAttribute(hwnd, 1 /* DWMWA_NCRENDERING_ENABLED */,
                                                       &dialogComposed, sizeof dialogComposed);
                    if (FAILED(result) || !dialogComposed)
                        qWarning() << "System settings native frame:" << Qt::hex << result
                                   << GetWindowLongPtrW(hwnd, GWL_STYLE) << GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
                                   << hasRoundedRegion(settings.winId());
                    check(SUCCEEDED(result) && dialogComposed,
                          "System settings fell back to classic GDI while the main window used DWM");
                }
                check(mw->centralWidget()->autoFillBackground() && mw->mask().isEmpty(), "classic background is not opaque");
                check(mw->centralWidget()->palette().color(QPalette::Window) == qApp->palette().color(QPalette::Window),
                      "Rawline background survived the switch to System");
                checkMainCaption(false);
                tour->shoot(QStringLiteral("system-with-settings-open"));
            }
            themeManager()->ApplyTheme(theme);
#endif
            const auto captionButtons = Qt::WindowTitleHint | Qt::WindowMinimizeButtonHint |
                                        Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint;
            for (const auto &classic : classicThemes) {
                themeManager()->ApplyTheme(classic);
                QEventLoop settle;
                QTimer::singleShot(300, &settle, &QEventLoop::quit);
                settle.exec();
                const auto classicFlags = mw->windowFlags();
                check(mw->isVisible() && !(classicFlags & Qt::FramelessWindowHint), "main window did not regain its system frame");
                check((classicFlags & captionButtons) == captionButtons, "system title bar buttons are missing");
                check(mw->mask().isEmpty(), "new theme corner mask survived the switch");
                tour->shoot(QStringLiteral("theme-to-") + classic);
                themeManager()->ApplyTheme(theme);
#ifdef Q_OS_WIN
                check(mw->winId() == mainId, "switching back recreated the main window");
                checkMainCaption(true);
#else
                check(mw->windowFlags() & Qt::FramelessWindowHint, "main window did not recover the Rawline frame");
#endif
            }
        })};
    }
    QTimer::singleShot(4000, qApp, [tour] { run(tour); });
}
