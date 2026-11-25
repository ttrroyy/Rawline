#pragma once

#include <QList>
#include <QString>

class QWidget;

namespace Configs {
    class Profile;
}

// Rich text for a profile's stored service check, colored with the theme's tokens.
namespace ServiceCheckHtml {
    // Each category's name colored by its services' status, then the exit IP's kind; empty without a service check.
    [[nodiscard]] QString summary(const Configs::Profile &profile);

    // Every service's status, for a tooltip; empty without a service check.
    [[nodiscard]] QString details(const Configs::Profile &profile);
    void showReport(QWidget *parent, const QList<int> &ids, qint64 startedAt);
}
