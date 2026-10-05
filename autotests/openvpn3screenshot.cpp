/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

/**
 * Renders the openvpn3 editor pages to PNG files, for review.
 *
 * Not a test: a reviewer cannot run a Plasma session in a container, so this
 * loads the synthetic example.org profile into the real widget under the
 * offscreen platform and grabs each page.
 *
 * Usage: openvpn3screenshot <output directory>
 */

#include <QApplication>
#include <QDir>
#include <QTabWidget>

#include <NetworkManagerQt/VpnSetting>

#include "nm-openvpn3-service.h"
#include "openvpn3importer.h"
#include "openvpn3storage.h"
#include "openvpn3widget.h"

using namespace Qt::Literals::StringLiterals;

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc < 2) {
        qWarning("usage: openvpn3screenshot <output directory>");
        return 2;
    }
    const QDir outputDir(QString::fromLocal8Bit(argv[1]));
    const QString profilePath = QString::fromLatin1(OPENVPN3_TEST_DATA_DIR) + u"/office.ovpn"_s;

    const Openvpn3Import import = Openvpn3Importer::fromFile(profilePath);
    if (!import.isValid()) {
        qWarning("cannot import %s: %s", qPrintable(profilePath), qPrintable(import.errorMessage()));
        return 1;
    }

    NMStringMap data;
    NMStringMap secrets;
    Openvpn3Importer::apply(import, data, secrets);

    NetworkManager::VpnSetting source;
    source.setServiceType(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    source.setData(data);
    source.setSecrets(secrets);
    auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    setting->fromMap(source.toMap());
    setting->setSecrets(secrets);

    auto *widget = new OpenVpn3SettingWidget(setting);
    widget->resize(1050, 820);
    widget->show();

    auto *tabs = widget->findChild<QTabWidget *>(u"openvpn3_tabs"_s);
    const QStringList names{u"general"_s, u"directives"_s, u"source"_s};
    for (int i = 0; i < tabs->count() && i < names.size(); ++i) {
        tabs->setCurrentIndex(i);
        app.processEvents();
        const QString path = outputDir.filePath(u"openvpn3-editor-"_s + names.at(i) + u".png"_s);
        if (!widget->grab().save(path)) {
            qWarning("cannot write %s", qPrintable(path));
            return 1;
        }
        qInfo("wrote %s", qPrintable(path));
    }

    delete widget;
    return 0;
}
