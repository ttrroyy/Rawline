#pragma once

// The Mikan interface: a dashboard over the same actions, menus and models as the classic window.
// It adds a look and loses no function. Log, connections, the traffic graph and runtime stats
// sit in a sidebar opened from a small icon.

#include <QAbstractButton>
#include <QElapsedTimer>
#include <QListView>
#include <QPalette>
#include <QStyledItemDelegate>
#include <QWidget>
#include <functional>

class MainWindow;
class QBoxLayout;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTimer;
class QVariantAnimation;
class QVBoxLayout;

namespace Mikan {
    // Theme keys kept in settings; anything else is the classic interface.
    inline constexpr auto DarkTheme = "MikanDark";
    inline constexpr auto LightTheme = "MikanLight";
    // Previous keys. Read once at startup and rewritten to the names above.
    inline constexpr auto LegacyDarkTheme = "NewOLED";
    inline constexpr auto LegacyLightTheme = "NewWhite";
    bool isTheme(const QString &theme);
    void setTheme(const QString &theme);
    bool active();
    bool light();

    QColor background();
    QColor text();
    QColor muted();
    QColor solid();
    QColor outline();
    QColor overlay(int alpha);

    QPalette palette();
    QString styleSheet();
    // Geologica, the mobile app's typeface; registered once.
    QString fontFamily();
    QFont font(qreal px, int weight = QFont::Normal);

    void paintBackdrop(QPainter &p, const QRect &r);
    // A translucent body, lighter at the top, with one thin rim; tinted for the chosen row or action.
    void paintGlass(QPainter &p, const QRectF &r, qreal radius, const QColor &tint = QColor());
    void paintIcon(QPainter &p, const QString &name, const QRectF &box, const QColor &color);
    QIcon icon(const QString &name, const QColor &color);
    // The flat square flag of a profile name's leading flag, or a null pixmap.
    QPixmap flag(const QString &leading, int size, qreal dpr);
    QString leadingFlag(const QString &name);

    // Fusion with the new style's own check boxes, radio buttons and menu ticks.
    QStyle *createStyle();
    // Windows 11's round corners for a window without a frame of its own.
    void roundCorners(QWidget *window, bool compact);
    // A widget arriving: fades in while it rises into place, as the mobile screens do.
    void rise(QWidget *widget, int distance, int duration);
}

// A row, tile or tab of the new style: glass that lights under the pointer and presses like the mobile ones.
class MikanTile : public QAbstractButton {
public:
    enum Kind { Icon, Chip, Tab, Text };
    explicit MikanTile(Kind kind, QWidget *parent = nullptr);
    void setIconName(const QString &name) { m_icon = name; update(); }
    // A second, muted line under the text, as the mobile subscription rows have.
    void setSubtitle(const QString &subtitle) { m_subtitle = subtitle; updateGeometry(); update(); }
    void setSelected(bool selected) { m_selected = selected; update(); }
    // An opaque plate, so a row over a translucent popup stays readable.
    void setFilled(bool filled) { m_filled = filled; update(); }
    // A chevron: right while collapsed, down while expanded.
    void setExpandable(bool on) { m_expandable = on; update(); }
    void setExpanded(bool on) { m_expanded = on; update(); }
    void setSpinning(bool spinning);
    void setTextSize(qreal px) { m_textSize = px; updateGeometry(); update(); }
    void setFlat(bool flat) { m_flat = flat; update(); }
    // A menu row: the label starts at the left, and a submenu draws a chevron.
    void setAlignLeft(bool on) { m_alignLeft = on; update(); }
    void setChevron(bool on) { m_chevron = on; update(); }
    // A shortcut on the right, and a tick for a checked action.
    void setTrailing(const QString &text) { m_trailing = text; update(); }
    void setMark(bool on) { m_mark = on; update(); }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    void animatePress(qreal to, int duration);
    Kind m_kind;
    QString m_icon;
    QString m_subtitle;
    bool m_selected = false;
    bool m_hover = false;
    bool m_flat = false;
    bool m_filled = false;
    bool m_expandable = false;
    bool m_expanded = false;
    bool m_alignLeft = false;
    bool m_chevron = false;
    bool m_mark = false;
    QString m_trailing;
    qreal m_textSize = 14;
    qreal m_press = 0;
    qreal m_angle = 0;
    QVariantAnimation *m_pressAnim = nullptr;
    QTimer *m_spin = nullptr;
};

// The TUN and system proxy switches of the mobile home screen.
class MikanSwitch : public QAbstractButton {
public:
    explicit MikanSwitch(const QString &label, QWidget *parent = nullptr);
    void setOn(bool on);
    void setIconName(const QString &name) { m_icon = name; update(); }
    void setDescription(const QString &description);
    // Drawn without its own card, so a parent card can hold several switches.
    void setBare(bool bare) { m_bare = bare; update(); }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    bool m_on = false;
    bool m_hover = false;
    bool m_bare = false;
    QString m_icon;
    QString m_description;
    qreal m_knob = 0;
    QVariantAnimation *m_anim = nullptr;
};

// The new style's connect button: a disc whose edge is a slow wave, rings leaving it while connected,
// a stroke running round it while connecting.
class MikanOrb : public QAbstractButton {
public:
    explicit MikanOrb(QWidget *parent = nullptr);
    void setState(bool connected, bool connecting, const QString &status, const QString &elapsed);
    QSize sizeHint() const override { return {92, 92}; }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool hitButton(const QPoint &pos) const override;

private:
    void animatePress(qreal to, int duration);
    bool m_connected = false;
    bool m_connecting = false;
    QString m_status;
    QString m_elapsed;
    qreal m_wave = .3, m_spin = 0, m_lit = 0, m_angle = 0, m_phase = 0, m_pressScale = 1;
    qint64 m_lastFrame = 0;
    QElapsedTimer m_clock;
    QTimer *m_frame = nullptr;
    QVariantAnimation *m_pressAnim = nullptr;
};

// Close, maximise and minimise in the window's own style.
class MikanTitleBar : public QWidget {
public:
    explicit MikanTitleBar(QWidget *parent = nullptr);
    // Shows the window's icon and title, as its own frame would.
    void follow(QWidget *window);

protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QWidget *m_window = nullptr;
};

class MikanProfileDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    int runningId = -1;
    int hoverRow = -1;
    qreal spinAngle = 0;
    static QRect chevronRect(const QRect &row);
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

class MikanProfileList : public QListView {
public:
    explicit MikanProfileList(QWidget *parent = nullptr);
    MikanProfileDelegate *rowDelegate = nullptr;
    std::function<void(const QModelIndex &)> editRequested;
    std::function<void(int from, int to)> rowsSwapped;
    std::function<int(int)> toSourceRow;
    QString emptyText;

protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void dropEvent(QDropEvent *) override;

private:
    QTimer *m_spinner = nullptr;
};

class MikanHome : public QWidget {
public:
    explicit MikanHome(MainWindow *window);
    // Moves the notice and the log, connections, traffic and runtime pages in, or back to the classic window.
    void attach();
    void detach();
    bool attached() const { return m_attached; }
    void groupChanged();
    void refresh();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Qt::Edges edgesAt(const QPoint &pos) const;
    void buildHero(QVBoxLayout *root);
    void buildControls(QVBoxLayout *root);
    void buildDashboard(QWidget *page);
    void buildServerList(QVBoxLayout *serversLayout);
    void buildBody(QVBoxLayout *body);
    void updateRates(bool live);
    void placeDrawer();
    void showDetails(bool open);
    void selectTab(int index, bool animated = true);
    void showGroupPicker();
    void showAppMenu(QWidget *anchor);
    void showSubscriptionMenu();
    void showAddMenu();
    void showGroupMenu();
    void checkCurrentConnection();
    void refreshGroupHeader();
    void updateNotice();

    MainWindow *mw;
    bool m_attached = false;
    struct Slot { QBoxLayout *layout = nullptr; int index = -1; int stretch = 0; };
    Slot m_dataSlot, m_statsSlot;

    MikanTitleBar *m_titleBar = nullptr;
    MikanOrb *m_orb = nullptr;
    MikanTile *m_chip = nullptr;
    MikanTile *m_more = nullptr;
    MikanTile *m_add = nullptr;
    MikanTile *m_refresh = nullptr;
    MikanTile *m_groupMore = nullptr;
    MikanTile *m_support = nullptr;
    MikanSwitch *m_tun = nullptr;
    MikanSwitch *m_proxy = nullptr;
    QLabel *m_check = nullptr;
    QLabel *m_down = nullptr;
    QLabel *m_up = nullptr;
    QLabel *m_running = nullptr;
    QLabel *m_inbound = nullptr;
    QLabel *m_groupName = nullptr;
    QLabel *m_groupDate = nullptr;
    QLabel *m_quota = nullptr;
    QWidget *m_traffic = nullptr;
    QLabel *m_expiry = nullptr;
    QLabel *m_announce = nullptr;
    QLineEdit *m_search = nullptr;
    QWidget *m_notice = nullptr;
    QVBoxLayout *m_noticeLayout = nullptr;
    QVBoxLayout *m_statsLayout = nullptr;
    QWidget *m_empty = nullptr;
    QWidget *m_drawer = nullptr;
    QList<MikanTile *> m_tabs;
    // stats_widget index for each sidebar tab, in the same order as m_tabs.
    QList<int> m_tabStats;
    MikanProfileList *m_list = nullptr;
    QTimer *m_tick = nullptr;
    QVariantAnimation *m_noticeAnim = nullptr;

    QElapsedTimer m_connectedFor;
    bool m_wasRunning = false;
    qint64 m_checkSince = 0;
    qint64 m_checkShownAt = 0;
    QString m_checkResult;
    bool m_announceOpen = false;
    int m_renderedGroup = -1;
    QString m_headerKey;
    bool m_syncingTabs = false;
    bool m_detailsOpen = false;
    double m_drawerProgress = 0;
    QVariantAnimation *m_drawerAnim = nullptr;
    double m_rateDown = 0, m_rateUp = 0;

    QStackedWidget *m_lower = nullptr;
    MikanTile *m_details = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_via = nullptr;
    QLabel *m_flag = nullptr;
    QWidget *m_dot = nullptr;
    QWidget *m_latencyRow = nullptr;
    QLabel *m_server = nullptr;
    QLabel *m_serverFlag = nullptr;
    QLabel *m_latency = nullptr;
    QLabel *m_protocol = nullptr;
    QLabel *m_transport = nullptr;
    QLabel *m_uptime = nullptr;
};
