/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "openvpn3widget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStringDecoder>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <KLocalizedString>

#include "nm-openvpn3-service.h"
#include "openvpn3directiveswidget.h"
#include "openvpn3importer.h"
#include "passwordfield.h"
#include "plasma_nm_openvpn3.h"

using Flags = NetworkManager::Setting::SecretFlags;

namespace
{
enum RemoteColumn {
    HostColumn,
    PortColumn,
    TransportColumn,
    RemoteColumnCount,
};

/** OpenVPN's own default, used when a transport is given without a port. */
const auto kDefaultPort = QStringLiteral("1194");

/** Arguments of a remote past host/port/transport, kept on the row's item. */
constexpr int ExtraArgumentsRole = Qt::UserRole + 1;
/** Openvpn3Entry::id() of the entry this row is, or 0 for a row with no entry
 * yet. Rows are matched to entries by this and never by position. */
constexpr int EntryIdRole = Qt::UserRole + 2;

// A credential source replaces the old pair even when its password is empty.
// Normalizing unrelated certificate files must leave the existing pair alone.
bool hasCredentialSource(const Openvpn3Profile &profile)
{
    for (const auto &entry : profile.optionsNamed(QStringLiteral("auth-user-pass"))) {
        if (entry.isBlock() || !entry.arguments.isEmpty()) {
            return true;
        }
    }
    return false;
}

// Profile storage choices. The first two are the secret flags the profile is
// stored with; the third is the old public data item, kept so that opening an
// existing connection and saving it does not move its profile somewhere else
// without being asked.
constexpr int kWalletStorage = NetworkManager::Setting::AgentOwned;
constexpr int kSystemStorage = NetworkManager::Setting::None;
constexpr int kLegacyStorage = -1;

QString cellText(const QTableWidget *table, int row, int column)
{
    const QTableWidgetItem *item = table->item(row, column);
    return item ? item->text().trimmed() : QString();
}

QStringList remoteExtras(const QTableWidget *table, int row)
{
    const QTableWidgetItem *item = table->item(row, 0);
    return item ? item->data(ExtraArgumentsRole).toStringList() : QStringList();
}

quint64 remoteEntryId(const QTableWidget *table, int row)
{
    const QTableWidgetItem *item = table->item(row, 0);
    return item ? item->data(EntryIdRole).value<quint64>() : 0;
}

void setCellText(QTableWidget *table, int row, int column, const QString &text)
{
    QTableWidgetItem *item = table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, column, item);
    }
    item->setText(text);
}

/**
 * @p contents as text, or nothing when it is not text at all.
 *
 * A PEM block is text by definition, and QString::fromUtf8() would turn a DER
 * certificate or a PKCS#12 bundle into replacement characters without saying
 * so -- an embedded block that no longer is the file it came from.
 */
std::optional<QString> asText(const QByteArray &contents)
{
    auto decoder = QStringDecoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder(contents);
    if (decoder.hasError() || contents.contains('\0')) {
        return std::nullopt;
    }
    return text;
}

/** The file wrapped in the form its directive takes: PEM and friends as the
 * text they are, a PKCS#12 bundle as base64. */
QString inlineBody(const QByteArray &contents, bool base64)
{
    if (!base64) {
        QString text = asText(contents).value_or(QString());
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n'))) {
            text += QLatin1Char('\n');
        }
        return text;
    }
    const QByteArray encoded = contents.toBase64();
    QString body;
    for (qsizetype i = 0; i < encoded.size(); i += 64) {
        body += QString::fromLatin1(encoded.mid(i, 64)) + QLatin1Char('\n');
    }
    return body;
}

/** @p text with the line terminator the document it belongs to uses. */
QString withCrlf(const QString &text)
{
    QString converted = text;
    converted.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    converted.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
    return converted;
}
}

/** One certificate, key or bundle the profile can carry inline. */
struct OpenVpn3SettingWidget::MaterialRow {
    QString directive;
    bool base64 = false;
    QString filter;
    QLabel *status = nullptr;
    QPushButton *load = nullptr;
    QPushButton *clear = nullptr;
};

class OpenVpn3SettingWidget::Private
{
public:
    QTabWidget *tabs = nullptr;
    QWidget *generalPage = nullptr;
    QWidget *directivesPage = nullptr;
    QWidget *sourcePage = nullptr;

    QLabel *status = nullptr;
    QPushButton *importButton = nullptr;

    QCheckBox *clientMode = nullptr;
    QTableWidget *remotes = nullptr;
    QPushButton *removeRemote = nullptr;
    QPushButton *remoteUp = nullptr;
    QPushButton *remoteDown = nullptr;
    QLineEdit *port = nullptr;
    QComboBox *proto = nullptr;
    QComboBox *device = nullptr;
    QCheckBox *authUserPass = nullptr;
    QLineEdit *username = nullptr;
    PasswordField *password = nullptr;
    PasswordField *certPass = nullptr;
    QComboBox *storage = nullptr;

    QList<MaterialRow> materials;

    Openvpn3DirectivesWidget *directives = nullptr;
    QPlainTextEdit *source = nullptr;
};

OpenVpn3SettingWidget::OpenVpn3SettingWidget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent)
    : SettingWidget(setting, parent)
    , d(new Private)
    , m_setting(setting)
{
    buildUi();
    watchChangedSetting();
    KAcceleratorManager::manage(this);

    // Deliberately not guarded on isNull(): a setting carrying data is worth
    // loading whether or not NetworkManagerQt considers it initialised, and
    // loadConfig() copes with an empty one by showing an empty profile.
    if (setting) {
        loadConfig(setting);
    } else {
        loadViewsFromProfile();
        refreshStatus();
    }
}

OpenVpn3SettingWidget::~OpenVpn3SettingWidget()
{
    delete d;
}

// -- construction ------------------------------------------------------------

void OpenVpn3SettingWidget::buildUi()
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto header = new QHBoxLayout;
    d->status = new QLabel(this);
    d->status->setObjectName(QStringLiteral("openvpn3_status"));
    d->status->setWordWrap(true);
    d->status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    d->importButton = new QPushButton(i18nc("@action:button", "Import Profile…"), this);
    d->importButton->setObjectName(QStringLiteral("openvpn3_import"));
    header->addWidget(d->status, 1);
    header->addWidget(d->importButton);
    layout->addLayout(header);
    connect(d->importButton, &QPushButton::clicked, this, &OpenVpn3SettingWidget::chooseProfileFile);

    d->tabs = new QTabWidget(this);
    d->tabs->setObjectName(QStringLiteral("openvpn3_tabs"));
    layout->addWidget(d->tabs, 1);

    d->generalPage = buildGeneralPage();
    d->tabs->addTab(d->generalPage, i18nc("@title:tab", "General"));

    d->directivesPage = new QWidget(this);
    auto directivesLayout = new QVBoxLayout(d->directivesPage);
    d->directives = new Openvpn3DirectivesWidget(d->directivesPage);
    d->directives->setObjectName(QStringLiteral("openvpn3_directives"));
    directivesLayout->addWidget(d->directives);
    d->tabs->addTab(d->directivesPage, i18nc("@title:tab", "Directives"));
    connect(d->directives, &Openvpn3DirectivesWidget::changed, this, &OpenVpn3SettingWidget::onProfileEdited);

    d->sourcePage = new QWidget(this);
    auto sourceLayout = new QVBoxLayout(d->sourcePage);
    auto sourceHint = new QLabel(i18n("The profile exactly as it is stored. Editing it here is the last word: the other pages are rebuilt from it."),
                                 d->sourcePage);
    sourceHint->setWordWrap(true);
    sourceLayout->addWidget(sourceHint);
    d->source = new QPlainTextEdit(d->sourcePage);
    d->source->setObjectName(QStringLiteral("openvpn3_source"));
    d->source->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    d->source->setLineWrapMode(QPlainTextEdit::NoWrap);
    sourceLayout->addWidget(d->source, 1);
    d->tabs->addTab(d->sourcePage, i18nc("@title:tab the profile as plain text", "Profile Source"));
    connect(d->source, &QPlainTextEdit::textChanged, this, &OpenVpn3SettingWidget::onProfileEdited);

    connect(d->tabs, &QTabWidget::currentChanged, this, &OpenVpn3SettingWidget::onTabChanged);
}

QWidget *OpenVpn3SettingWidget::buildGeneralPage()
{
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto page = new QWidget(scroll);
    auto form = new QFormLayout(page);
    scroll->setWidget(page);

    d->clientMode = new QCheckBox(i18n("Pull the configuration from the server (client mode)"), page);
    d->clientMode->setObjectName(QStringLiteral("openvpn3_client"));
    form->addRow(QString(), d->clientMode);
    connect(d->clientMode, &QCheckBox::toggled, this, [this](bool on) {
        applyPresence(QStringLiteral("client"), on);
    });

    d->remotes = new QTableWidget(page);
    d->remotes->setObjectName(QStringLiteral("openvpn3_remotes"));
    d->remotes->setColumnCount(RemoteColumnCount);
    d->remotes->setHorizontalHeaderLabels({
        i18nc("@title:column a VPN server host name", "Server"),
        i18nc("@title:column", "Port"),
        i18nc("@title:column udp or tcp", "Transport"),
    });
    d->remotes->horizontalHeader()->setSectionResizeMode(HostColumn, QHeaderView::Stretch);
    d->remotes->verticalHeader()->setVisible(false);
    d->remotes->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->remotes->setSelectionMode(QAbstractItemView::SingleSelection);
    d->remotes->setMinimumHeight(110);
    form->addRow(i18n("Servers:"), d->remotes);
    connect(d->remotes, &QTableWidget::cellChanged, this, [this](int, int) {
        if (!m_updating) {
            applyRemotes();
        }
    });

    auto remoteButtons = new QHBoxLayout;
    auto addRemote = new QPushButton(i18nc("@action:button", "Add"), page);
    addRemote->setObjectName(QStringLiteral("openvpn3_remote_add"));
    d->removeRemote = new QPushButton(i18nc("@action:button", "Remove"), page);
    d->removeRemote->setObjectName(QStringLiteral("openvpn3_remote_remove"));
    d->remoteUp = new QPushButton(i18nc("@action:button", "Move Up"), page);
    d->remoteUp->setObjectName(QStringLiteral("openvpn3_remote_up"));
    d->remoteDown = new QPushButton(i18nc("@action:button", "Move Down"), page);
    d->remoteDown->setObjectName(QStringLiteral("openvpn3_remote_down"));
    remoteButtons->addWidget(addRemote);
    remoteButtons->addWidget(d->removeRemote);
    remoteButtons->addWidget(d->remoteUp);
    remoteButtons->addWidget(d->remoteDown);
    remoteButtons->addStretch();
    form->addRow(QString(), remoteButtons);

    connect(addRemote, &QPushButton::clicked, this, [this] {
        addRemoteRow(QString(), QString(), QString(), {}, 0);
        d->remotes->selectRow(d->remotes->rowCount() - 1);
        applyRemotes();
    });
    connect(d->removeRemote, &QPushButton::clicked, this, [this] {
        const int row = d->remotes->currentRow();
        if (row >= 0) {
            d->remotes->removeRow(row);
            applyRemotes();
        }
    });
    auto moveRemote = [this](int delta) {
        const int row = d->remotes->currentRow();
        const int to = row + delta;
        if (row < 0 || to < 0 || to >= d->remotes->rowCount()) {
            return;
        }
        const bool wasUpdating = m_updating;
        m_updating = true;
        // Whole items, not just their text: the trailing arguments the table
        // does not show are stored on the item and have to travel with it.
        for (int column = 0; column < RemoteColumnCount; ++column) {
            QTableWidgetItem *here = d->remotes->takeItem(row, column);
            QTableWidgetItem *there = d->remotes->takeItem(to, column);
            d->remotes->setItem(row, column, there);
            d->remotes->setItem(to, column, here);
        }
        m_updating = wasUpdating;
        d->remotes->selectRow(to);
        applyRemotes();
    };
    connect(d->remoteUp, &QPushButton::clicked, this, [moveRemote] {
        moveRemote(-1);
    });
    connect(d->remoteDown, &QPushButton::clicked, this, [moveRemote] {
        moveRemote(1);
    });

    d->port = new QLineEdit(page);
    d->port->setObjectName(QStringLiteral("openvpn3_port"));
    d->port->setPlaceholderText(i18nc("@info:placeholder", "OpenVPN's default"));
    d->port->setToolTip(i18n("The <interface>port</interface> directive: the port used by servers that do not name one themselves."));
    form->addRow(i18n("Default port:"), d->port);
    connect(d->port, &QLineEdit::editingFinished, this, [this] {
        applySimpleDirective(QStringLiteral("port"), d->port->text().trimmed());
    });

    d->proto = new QComboBox(page);
    d->proto->setObjectName(QStringLiteral("openvpn3_proto"));
    d->proto->setEditable(true);
    d->proto->addItems({QString(), QStringLiteral("udp"), QStringLiteral("tcp"), QStringLiteral("tcp-client"), QStringLiteral("udp4"), QStringLiteral("udp6"),
                        QStringLiteral("tcp4-client"), QStringLiteral("tcp6-client")});
    d->proto->setToolTip(i18n("The <interface>proto</interface> directive. Leave empty for OpenVPN's default."));
    form->addRow(i18n("Default transport:"), d->proto);
    connect(d->proto, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (!m_updating) {
            applySimpleDirective(QStringLiteral("proto"), text.trimmed());
        }
    });

    d->device = new QComboBox(page);
    d->device->setObjectName(QStringLiteral("openvpn3_device"));
    d->device->setEditable(true);
    d->device->addItems({QString(), QStringLiteral("tun"), QStringLiteral("tap")});
    d->device->setToolTip(i18n("The <interface>dev</interface> directive. Leave empty for OpenVPN's default."));
    form->addRow(i18n("Device type:"), d->device);
    connect(d->device, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (!m_updating) {
            applySimpleDirective(QStringLiteral("dev"), text.trimmed());
        }
    });

    d->authUserPass = new QCheckBox(i18n("The server asks for a username and password"), page);
    d->authUserPass->setObjectName(QStringLiteral("openvpn3_authuserpass"));
    d->authUserPass->setToolTip(i18n("The <interface>auth-user-pass</interface> directive."));
    form->addRow(QString(), d->authUserPass);
    connect(d->authUserPass, &QCheckBox::toggled, this, [this](bool on) {
        applyPresence(QStringLiteral("auth-user-pass"), on);
        d->username->setEnabled(on);
        d->password->setEnabled(on);
    });

    d->username = new QLineEdit(page);
    d->username->setObjectName(QStringLiteral("openvpn3_username"));
    form->addRow(i18n("Username:"), d->username);

    d->password = new PasswordField(page);
    d->password->setObjectName(QStringLiteral("openvpn3_password"));
    d->password->setPasswordModeEnabled(true);
    d->password->setPasswordOptionsEnabled(true);
    d->password->setPasswordOption(PasswordField::StoreForUser);
    d->password->setPasswordNotRequiredEnabled(true);
    form->addRow(i18n("Password:"), d->password);

    const auto certificates = i18n("Certificates (*.pem *.crt *.cer *.key *.p12 *.pfx);;All files (*)");
    addMaterialRow(form, i18n("Certificate authority:"), QStringLiteral("ca"), false, certificates);
    addMaterialRow(form, i18n("User certificate:"), QStringLiteral("cert"), false, certificates);
    addMaterialRow(form, i18n("Private key:"), QStringLiteral("key"), false, certificates);
    addMaterialRow(form, i18n("PKCS#12 bundle:"), QStringLiteral("pkcs12"), true, certificates);
    addMaterialRow(form, i18n("TLS auth key:"), QStringLiteral("tls-auth"), false, certificates);
    addMaterialRow(form, i18n("TLS crypt key:"), QStringLiteral("tls-crypt"), false, certificates);

    d->certPass = new PasswordField(page);
    d->certPass->setObjectName(QStringLiteral("openvpn3_certpass"));
    d->certPass->setPasswordModeEnabled(true);
    d->certPass->setPasswordOptionsEnabled(true);
    d->certPass->setPasswordOption(PasswordField::StoreForUser);
    d->certPass->setPasswordNotRequiredEnabled(true);
    form->addRow(i18n("Private key passphrase:"), d->certPass);

    d->storage = new QComboBox(page);
    d->storage->setObjectName(QStringLiteral("openvpn3_storage"));
    d->storage->addItem(i18n("Store for this user (password wallet)"), kWalletStorage);
    d->storage->addItem(i18n("Store for all users (needed for unattended connections)"), kSystemStorage);
    // The third choice, the old public data item, is added only for a
    // connection that already uses it; see refreshStorageChoices().
    d->storage->setToolTip(
        i18n("A profile contains the private keys and certificates the connection uses. Stored as a secret, the wallet keeps it for you, or "
             "NetworkManager keeps it for all users so the connection can come up with nobody logged in."));
    form->addRow(i18n("Profile storage:"), d->storage);
    connect(d->storage, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_updating) {
            onFieldEdited();
        }
    });

    for (PasswordField *field : {d->password, d->certPass}) {
        const QString key = field == d->password ? QLatin1String(NM_OPENVPN3_KEY_PASSWORD) : QLatin1String(NM_OPENVPN3_KEY_CERT_PASS);
        connect(field, &PasswordField::textChanged, this, [this, key] {
            if (!m_updating) {
                m_editedSecretValues.insert(key);
                onFieldEdited();
            }
        });
        connect(field, &PasswordField::passwordOptionChanged, this, [this, key] {
            if (!m_updating) {
                m_editedSecretFlags.insert(key);
                onFieldEdited();
            }
        });
    }
    connect(d->username, &QLineEdit::textChanged, this, &OpenVpn3SettingWidget::onFieldEdited);

    return scroll;
}

void OpenVpn3SettingWidget::refreshStorageChoices()
{
    const bool wasUpdating = m_updating;
    m_updating = true;
    const int index = d->storage->findData(kLegacyStorage);
    // Keeping key material in the connection's own settings, where every
    // program allowed to read the connection can read it, is not something to
    // offer for a profile that is not already there: the user asked for a
    // secret, and offering to undo that in the same list is not a choice, it
    // is a trap. An existing legacy connection keeps the option so that
    // opening and saving it does not silently relocate its profile.
    if (storedStorageChoice() == kLegacyStorage && index < 0) {
        d->storage->addItem(i18n("Store with the connection settings (older, not protected)"), kLegacyStorage);
    } else if (storedStorageChoice() != kLegacyStorage && index >= 0) {
        d->storage->removeItem(index);
    }
    m_updating = wasUpdating;
}

void OpenVpn3SettingWidget::addMaterialRow(QFormLayout *form, const QString &label, const QString &directive, bool base64, const QString &filter)
{
    MaterialRow row;
    row.directive = directive;
    row.base64 = base64;
    row.filter = filter;

    auto container = new QWidget(form->parentWidget());
    auto layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    row.status = new QLabel(container);
    row.status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row.load = new QPushButton(i18nc("@action:button embed a certificate or key file in the profile", "Embed File…"), container);
    row.clear = new QPushButton(i18nc("@action:button", "Clear"), container);
    layout->addWidget(row.status, 1);
    layout->addWidget(row.load);
    layout->addWidget(row.clear);
    form->addRow(label, container);

    d->materials.append(row);
    const MaterialRow &stored = d->materials.last();
    connect(row.load, &QPushButton::clicked, this, [this, stored] {
        loadMaterial(stored);
    });
    connect(row.clear, &QPushButton::clicked, this, [this, directive] {
        clearMaterial(directive);
    });
}

// -- the document ------------------------------------------------------------

void OpenVpn3SettingWidget::setProfile(const Openvpn3Profile &profile)
{
    m_profile = profile;
    loadViewsFromProfile();
}

Openvpn3Profile OpenVpn3SettingWidget::currentProfile() const
{
    // Whichever view is in front owns the document.
    if (d->tabs->currentWidget() == d->directivesPage) {
        return d->directives->profile();
    }
    if (d->tabs->currentWidget() == d->sourcePage) {
        return profileFromSource();
    }
    return m_profile;
}

Openvpn3Profile OpenVpn3SettingWidget::profileFromSource() const
{
    const QString text = d->source->toPlainText();
    if (text == m_sourceBaseline) {
        // Looking at the page is not editing it. A QPlainTextEdit holds text
        // with no carriage returns in it, so parsing back what it shows would
        // rewrite every line of a profile written on Windows without anybody
        // having touched it.
        return m_profile;
    }
    // Edited for real; the document keeps the line terminator it had.
    return Openvpn3Profile::fromText(m_profile.usesCrlf() ? withCrlf(text) : text);
}

QString OpenVpn3SettingWidget::profileText() const
{
    return currentProfile().toText();
}

void OpenVpn3SettingWidget::takeProfileFromCurrentView()
{
    m_profile = currentProfile();
}

void OpenVpn3SettingWidget::loadViewsFromProfile()
{
    const bool wasUpdating = m_updating;
    m_updating = true;

    loadGeneralFromProfile();
    d->directives->setProfile(m_profile);
    const QString text = m_profile.toText();
    if (d->source->toPlainText() != text) {
        d->source->setPlainText(text);
    }
    // What the widget holds, which is not always what was put in it: the
    // editor drops carriage returns. Remembering it is how an edit is told
    // apart from a visit; see currentProfile().
    m_sourceBaseline = d->source->toPlainText();

    m_updating = wasUpdating;
}

void OpenVpn3SettingWidget::loadGeneralFromProfile()
{
    const bool wasUpdating = m_updating;
    m_updating = true;

    d->clientMode->setChecked(m_profile.contains(QStringLiteral("client")));
    d->port->setText(m_profile.value(QStringLiteral("port")));
    d->proto->setCurrentText(m_profile.value(QStringLiteral("proto")));
    d->device->setCurrentText(m_profile.value(QStringLiteral("dev")));

    // What openvpn3 will do, which is not only what the top level says: a
    // profile may ask for a username and password inside a <connection>
    // block, and then it needs them just as much.
    const bool userPass = m_profile.containsOption(QStringLiteral("auth-user-pass"));
    const bool elsewhere = userPass && !m_profile.contains(QStringLiteral("auth-user-pass"));
    d->authUserPass->setChecked(userPass);
    // The checkbox adds and removes a top-level directive, which is not where
    // this one is. Saying so beats a checkbox that does nothing when clicked.
    d->authUserPass->setEnabled(!elsewhere);
    d->authUserPass->setToolTip(elsewhere
                                    ? i18n("This profile asks for a username and password inside a <interface>connection</interface> block. "
                                           "Edit it on the Directives or Profile Source page.")
                                    : i18n("The <interface>auth-user-pass</interface> directive."));
    d->username->setEnabled(userPass);
    d->password->setEnabled(userPass);

    loadRemotesFromProfile();
    refreshMaterialRows();

    m_updating = wasUpdating;
}

void OpenVpn3SettingWidget::loadRemotesFromProfile()
{
    const QList<int> indexes = m_profile.indexesOf(QStringLiteral("remote"));
    d->remotes->setRowCount(0);
    for (int index : indexes) {
        const Openvpn3Entry &entry = m_profile.at(index);
        addRemoteRow(entry.arguments.value(0), entry.arguments.value(1), entry.arguments.value(2), entry.arguments.mid(3), entry.id());
    }
}

void OpenVpn3SettingWidget::addRemoteRow(const QString &host, const QString &port, const QString &transport, const QStringList &extras, quint64 id)
{
    const bool wasUpdating = m_updating;
    m_updating = true;
    const int row = d->remotes->rowCount();
    d->remotes->insertRow(row);
    setCellText(d->remotes, row, HostColumn, host);
    setCellText(d->remotes, row, PortColumn, port);
    setCellText(d->remotes, row, TransportColumn, transport);
    d->remotes->item(row, HostColumn)->setData(ExtraArgumentsRole, extras);
    d->remotes->item(row, HostColumn)->setData(EntryIdRole, QVariant::fromValue(id));
    m_updating = wasUpdating;
}

void OpenVpn3SettingWidget::applyRemotes()
{
    const int rows = d->remotes->rowCount();

    // A row is one entry of the document, named by its id. Positions are no
    // use for that: removing the first row would rewrite every remaining
    // remote in place and delete the last entry, which moves remotes past the
    // directives between them and swaps their trailing arguments over.
    QList<quint64> wanted;
    wanted.reserve(rows);
    for (int row = 0; row < rows; ++row) {
        wanted.append(remoteEntryId(d->remotes, row));
    }

    // Entries whose row is gone.
    const QList<int> existing = m_profile.indexesOf(QStringLiteral("remote"));
    for (auto it = existing.crbegin(); it != existing.crend(); ++it) {
        if (!wanted.contains(m_profile.at(*it).id())) {
            m_profile.removeAt(*it);
        }
    }

    for (int row = 0; row < rows; ++row) {
        const QString host = cellText(d->remotes, row, HostColumn);
        const QString transport = cellText(d->remotes, row, TransportColumn);
        QString port = cellText(d->remotes, row, PortColumn);
        // "remote host proto" is not a thing: a transport needs a port in
        // front of it. Fill in the profile's own default rather than drop
        // what the user typed, and show what was stored.
        if (port.isEmpty() && !transport.isEmpty()) {
            port = m_profile.value(QStringLiteral("port"));
            if (port.isEmpty()) {
                port = kDefaultPort;
            }
            const bool wasUpdating = m_updating;
            m_updating = true;
            setCellText(d->remotes, row, PortColumn, port);
            m_updating = wasUpdating;
        }

        QStringList arguments{host};
        if (!port.isEmpty()) {
            arguments.append(port);
        }
        if (!transport.isEmpty()) {
            arguments.append(transport);
        }
        // Anything past the three fields this table models rides on the row,
        // not on its position: deleting or moving a row must not hand one
        // remote's trailing arguments to another.
        arguments += remoteExtras(d->remotes, row);

        int index = wanted.at(row) ? m_profile.indexOfId(wanted.at(row)) : -1;
        if (index < 0) {
            // A row the user added: a new entry, at the end, where a new
            // directive goes when nothing says otherwise.
            index = m_profile.append(Openvpn3Profile::directive(QStringLiteral("remote"), arguments));
            wanted[row] = m_profile.at(index).id();
            const bool wasUpdating = m_updating;
            m_updating = true;
            d->remotes->item(row, HostColumn)->setData(EntryIdRole, QVariant::fromValue(wanted.at(row)));
            m_updating = wasUpdating;
        } else {
            m_profile.setArguments(index, arguments);
        }
    }

    // Rows the user moved: the remotes change places with each other, in the
    // places they already occupy. Nothing else in the document moves, and no
    // remote crosses a directive that was between them.
    const QList<int> places = m_profile.indexesOf(QStringLiteral("remote"));
    if (places.size() == rows) {
        QList<Openvpn3Entry> inRowOrder;
        inRowOrder.reserve(rows);
        for (int row = 0; row < rows; ++row) {
            inRowOrder.append(m_profile.at(m_profile.indexOfId(wanted.at(row))));
        }
        for (int i = 0; i < rows; ++i) {
            m_profile.replace(places.at(i), inRowOrder.at(i));
        }
    }

    onProfileEdited();
}

void OpenVpn3SettingWidget::applySimpleDirective(const QString &name, const QString &value)
{
    if (m_updating) {
        return;
    }
    const int index = m_profile.indexOf(name);
    if (index >= 0) {
        if (value.isEmpty()) {
            m_profile.removeAt(index);
        } else {
            QStringList arguments = m_profile.at(index).arguments;
            if (arguments.isEmpty()) {
                arguments.append(value);
            } else {
                arguments[0] = value;
            }
            m_profile.setArguments(index, arguments);
        }
    } else if (!value.isEmpty()) {
        m_profile.setDirective(name, {value});
    }
    onProfileEdited();
}

void OpenVpn3SettingWidget::applyPresence(const QString &name, bool present)
{
    if (m_updating) {
        return;
    }
    m_profile.setPresent(name, present);
    onProfileEdited();
}

// -- certificates and keys ---------------------------------------------------

void OpenVpn3SettingWidget::refreshMaterialRows()
{
    // Whichever view owns the document right now, not the last one the
    // General page happened to see.
    const Openvpn3Profile profile = currentProfile();
    for (const MaterialRow &row : std::as_const(d->materials)) {
        const int index = profile.indexOf(row.directive);
        QString text;
        if (index < 0) {
            text = i18nc("@info:status no certificate or key is set", "Not set");
        } else if (profile.at(index).isBlock()) {
            const int lines = profile.at(index).body.count(QLatin1Char('\n'));
            text = i18ncp("@info:status", "Embedded in the profile (%1 line)", "Embedded in the profile (%1 lines)", lines);
        } else {
            const QString reference = profile.at(index).value();
            text = reference.isEmpty() ? i18nc("@info:status", "Set, with no file name")
                                       : i18nc("@info:status a file the profile refers to but does not contain", "File: %1 (not embedded)", reference);
        }
        row.status->setText(text);
        row.clear->setEnabled(index >= 0);
    }
}

void OpenVpn3SettingWidget::loadMaterial(const MaterialRow &row)
{
    const QString fileName = QFileDialog::getOpenFileName(this, i18nc("@title:window", "Choose a Certificate or Key"), QString(), row.filter);
    if (fileName.isEmpty()) {
        return;
    }
    QString errorMessage;
    if (!embedFile(row.directive, fileName, &errorMessage)) {
        QMessageBox::warning(this, i18nc("@title:window", "Cannot Embed File"), errorMessage);
    }
}

bool OpenVpn3SettingWidget::embedFile(const QString &directive, const QString &fileName, QString *errorMessage)
{
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    bool base64 = false;
    bool known = false;
    for (const MaterialRow &row : std::as_const(d->materials)) {
        if (row.directive == directive) {
            base64 = row.base64;
            known = true;
            break;
        }
    }
    if (!known) {
        return fail(i18n("<interface>%1</interface> is not a certificate or key this page embeds.", directive));
    }

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(file.errorString());
    }
    const QByteArray contents = file.readAll();
    if (!base64 && !asText(contents)) {
        // A DER certificate or a PKCS#12 bundle read as UTF-8 comes out as
        // replacement characters: a block that is no longer the file it came
        // from, and that openvpn3 cannot use. Refuse it instead.
        return fail(i18n("%1 is not text, so it cannot be embedded as a <interface>%2</interface> block. "
                         "A PKCS#12 bundle belongs in the PKCS#12 field; a certificate in DER form has to be "
                         "converted to PEM first.",
                         QFileInfo(fileName).fileName(),
                         directive));
    }

    takeProfileFromCurrentView();
    // A file reference and an inline block are the same directive to OpenVPN;
    // embedding replaces the reference so nothing points outside any more.
    const QList<int> existing = m_profile.indexesOf(directive);
    // The direction of a tls-auth key is the reference's second argument, and
    // a block has nowhere to put an argument. OpenVPN carries it in a
    // key-direction directive instead, which is what the backend's own
    // importer writes when it inlines one; dropping it here would keep the key
    // and lose the direction, and the connection would stop authenticating
    // against a server that expects one.
    QString keyDirection;
    for (auto it = existing.crbegin(); it != existing.crend(); ++it) {
        if (!m_profile.at(*it).isBlock()) {
            if (directive == QLatin1String("tls-auth") && m_profile.at(*it).arguments.size() > 1) {
                keyDirection = m_profile.at(*it).arguments.at(1);
            }
            m_profile.removeAt(*it);
        }
    }
    m_profile.setBlock(directive, inlineBody(contents, base64));
    if (!keyDirection.isEmpty()) {
        m_profile.setDirective(QStringLiteral("key-direction"), {keyDirection});
    }
    loadViewsFromProfile();
    onProfileEdited();
    return true;
}

void OpenVpn3SettingWidget::clearMaterial(const QString &directive)
{
    takeProfileFromCurrentView();
    m_profile.removeAll(directive);
    loadViewsFromProfile();
    onProfileEdited();
}

// -- loading and saving ------------------------------------------------------

int OpenVpn3SettingWidget::storedStorageChoice() const
{
    if (Openvpn3Storage::isSecretMode(m_data)) {
        // An explicit choice the connection already made, including system
        // storage for an unattended connection.
        return static_cast<int>(Openvpn3Storage::profileFlags(m_data).value_or(NetworkManager::Setting::AgentOwned));
    }
    if (m_data.contains(QLatin1String(NM_OPENVPN3_KEY_PROFILE))) {
        // An existing connection from before this editor. Opening it and
        // saving it again should not quietly move its profile elsewhere;
        // moving it is the user's choice to make, in this very combo box.
        return kLegacyStorage;
    }
    return kWalletStorage;
}

void OpenVpn3SettingWidget::loadConfig(const NetworkManager::Setting::Ptr &setting)
{
    auto vpn = setting.staticCast<NetworkManager::VpnSetting>();
    if (!vpn) {
        return;
    }
    m_connectionSecretsSuperseded = false;
    m_setting = vpn;
    m_data = vpn->data();
    m_secrets = vpn->secrets();
    m_profileEdited = false;
    m_fieldsEdited = false;
    m_editedSecretValues.clear();
    m_editedSecretFlags.clear();

    m_availability = Openvpn3Storage::availability(m_data, m_secrets);
    setProfile(Openvpn3Profile::fromText(Openvpn3Storage::readProfile(m_data, m_secrets)));

    const bool wasUpdating = m_updating;
    m_updating = true;
    d->username->setText(m_data.value(QLatin1String(NM_OPENVPN3_KEY_USERNAME)));
    refreshStorageChoices();
    d->storage->setCurrentIndex(d->storage->findData(storedStorageChoice()));
    fillPasswordField(d->password, QLatin1String(NM_OPENVPN3_KEY_PASSWORD), NetworkManager::Setting::AgentOwned);
    fillPasswordField(d->certPass, QLatin1String(NM_OPENVPN3_KEY_CERT_PASS), NetworkManager::Setting::AgentOwned);
    m_updating = wasUpdating;

    refreshStatus();
}

void OpenVpn3SettingWidget::loadSecrets(const NetworkManager::Setting::Ptr &setting)
{
    if (m_connectionSecretsSuperseded) {
        return;
    }

    auto vpn = setting.staticCast<NetworkManager::VpnSetting>();
    if (!vpn) {
        return;
    }

    // NetworkManager hands back only the secrets it has; merge rather than
    // replace, so a reply without the profile cannot blank it.
    const NMStringMap incoming = vpn->secrets();
    for (auto it = incoming.cbegin(); it != incoming.cend(); ++it) {
        // Unlock cancellation can arrive as a successful, empty reply.
        // Never let an empty profile replace a previously loaded secret.
        if (it.key() == QLatin1String(NM_OPENVPN3_KEY_PROFILE) && (it.value().isEmpty() || m_profileEdited)) {
            continue;
        }
        if (!m_editedSecretValues.contains(it.key())) {
            m_secrets.insert(it.key(), it.value());
        }
    }

    const Openvpn3Storage::Availability availability = Openvpn3Storage::availability(m_data, m_secrets);
    if (availability == Openvpn3Storage::Availability::Available && !m_profileEdited) {
        m_availability = availability;
        setProfile(Openvpn3Profile::fromText(Openvpn3Storage::readProfile(m_data, m_secrets)));
    } else if (availability == Openvpn3Storage::Availability::Available) {
        m_availability = availability;
    }

    const bool wasUpdating = m_updating;
    m_updating = true;
    for (PasswordField *field : {d->password, d->certPass}) {
        const QString key = field == d->password ? QLatin1String(NM_OPENVPN3_KEY_PASSWORD) : QLatin1String(NM_OPENVPN3_KEY_CERT_PASS);
        const QString text = field->text();
        const auto option = field->passwordOption();
        fillPasswordField(field, key, NetworkManager::Setting::AgentOwned);
        if (m_editedSecretValues.contains(key)) {
            field->setText(text);
        }
        if (m_editedSecretFlags.contains(key)) {
            field->setPasswordOption(option);
        }
    }
    refreshStorageChoices();
    m_updating = wasUpdating;

    refreshStatus();
    Q_EMIT validChanged(isValid());
}

void OpenVpn3SettingWidget::fillPasswordField(PasswordField *field, const QString &key, Flags fallback) const
{
    const QString stored = m_data.value(key + QLatin1String("-flags"));
    bool ok = false;
    const int value = stored.toInt(&ok);
    const Flags flags = (stored.isEmpty() || !ok) ? fallback : static_cast<Flags>(value);

    if (flags.testFlag(NetworkManager::Setting::NotRequired)) {
        field->setPasswordOption(PasswordField::NotRequired);
    } else if (flags.testFlag(NetworkManager::Setting::NotSaved)) {
        field->setPasswordOption(PasswordField::AlwaysAsk);
    } else if (flags.testFlag(NetworkManager::Setting::AgentOwned)) {
        field->setPasswordOption(PasswordField::StoreForUser);
    } else {
        field->setPasswordOption(PasswordField::StoreForAllUsers);
    }
    field->setText(m_secrets.value(key));
}

Flags OpenVpn3SettingWidget::flagsOf(const PasswordField *field)
{
    switch (field->passwordOption()) {
    case PasswordField::StoreForAllUsers:
        return NetworkManager::Setting::None;
    case PasswordField::StoreForUser:
        return NetworkManager::Setting::AgentOwned;
    case PasswordField::AlwaysAsk:
        return NetworkManager::Setting::NotSaved;
    case PasswordField::NotRequired:
        return NetworkManager::Setting::NotRequired;
    }
    return NetworkManager::Setting::AgentOwned;
}

void OpenVpn3SettingWidget::storePasswordField(const PasswordField *field, const QString &key, NMStringMap &data, NMStringMap &secrets, const QString &text)
    const
{
    const Flags flags = flagsOf(field);
    data.insert(key + QLatin1String("-flags"), QString::number(static_cast<int>(flags)));

    const bool storable = !flags.testFlag(NetworkManager::Setting::NotSaved) && !flags.testFlag(NetworkManager::Setting::NotRequired);
    if (!text.isEmpty() && storable) {
        secrets.insert(key, text);
    } else {
        secrets.remove(key);
    }
}

Openvpn3Policy OpenVpn3SettingWidget::chosenPolicy() const
{
    Openvpn3Policy policy;
    const int choice = d->storage->currentData().toInt();
    // An import brings new key material, and that is never put in the old
    // public data item; a legacy connection being reimported moves to the
    // wallet, which is also what the combo box will then show.
    policy.profileFlags = choice == kLegacyStorage ? NetworkManager::Setting::AgentOwned : static_cast<Flags>(choice);
    policy.passwordFlags = flagsOf(d->password);
    policy.certPassFlags = flagsOf(d->certPass);
    return policy;
}

const Openvpn3Import &OpenVpn3SettingWidget::normalized(const QString &text) const
{
    if (m_normalized.of != text) {
        m_normalized.of = text;
        m_normalized.result = Openvpn3Importer::normalize(text);
    }
    return m_normalized.result;
}

QVariantMap OpenVpn3SettingWidget::setting() const
{
    NMStringMap data = m_data;
    NMStringMap secrets = m_secrets;
    secrets.remove(QLatin1String(NM_OPENVPN3_KEY_CHALLENGE));
    // A connection that cannot be saved correctly is not half-saved: what
    // comes back is what came in, down to the keys this page never shows.
    // isValid() is what stops it being written at all, and the page says why,
    // but a caller that asks anyway gets the connection it already had rather
    // than a gap where its profile used to be.
    const bool blocked = !blockingProblem().isEmpty();

    Openvpn3Profile profile = currentProfile();
    QString username = d->username->text().trimmed();
    QString password = d->password->text();
    bool needsCertPass = profile.mayNeedPrivateKeyPassphrase();

    if (!blocked && profile.needsNormalization()) {
        // The profile points at a file, or carries credentials in the
        // document. openvpn3 gets a profile and nothing else, so the backend
        // makes it self-contained -- the same code an import goes through,
        // rather than a second opinion that would drift from it.
        const Openvpn3Import &result = normalized(profile.toText());
        const bool replacesCredentials = hasCredentialSource(profile);
        profile = Openvpn3Profile::fromText(result.profile());
        // Credentials the user wrote into the document are the newest thing
        // said about them; the fields are brought up to date to match as soon
        // as this page is in a position to do it.
        if (replacesCredentials) {
            username = result.username();
            password = result.password();
        }
        needsCertPass = result.needsCertPass();
    }

    if (blocked) {
        // Nothing to write: the maps are the connection's own, untouched.
        return vpnSettingOf(data, secrets);
    }

    if (!storedProfileIsUnreadable()) {
        const int choice = d->storage->currentData().toInt();
        if (choice == kLegacyStorage) {
            Openvpn3Storage::writeLegacyProfile(data, secrets, profile.toText());
        } else {
            Openvpn3Storage::writeSecretProfile(data, secrets, profile.toText(), static_cast<Flags>(choice));
        }
    }
    // Otherwise the stored profile stays exactly as it is: this editor never
    // saw it, so it has nothing to put in its place. isValid() keeps that
    // state from being saved at all.

    if (profile.containsOption(QStringLiteral("auth-user-pass"))) {
        if (username.isEmpty()) {
            data.remove(QLatin1String(NM_OPENVPN3_KEY_USERNAME));
        } else {
            data.insert(QLatin1String(NM_OPENVPN3_KEY_USERNAME), username);
        }
        storePasswordField(d->password, QLatin1String(NM_OPENVPN3_KEY_PASSWORD), data, secrets, password);
    } else {
        data.remove(QLatin1String(NM_OPENVPN3_KEY_USERNAME));
        data.remove(QLatin1String(NM_OPENVPN3_KEY_PASSWORD "-flags"));
        secrets.remove(QLatin1String(NM_OPENVPN3_KEY_PASSWORD));
    }

    // A one-time code is a one-time code whatever else is going on: never
    // stored, and an old one never kept to be replayed against the server.
    // Flags alone would not do it -- the secret would still be in the map
    // this returns, and the whole map is what gets written.
    secrets.remove(QLatin1String(NM_OPENVPN3_KEY_CHALLENGE));
    if (profile.containsOption(QStringLiteral("auth-user-pass"))) {
        data.insert(QLatin1String(NM_OPENVPN3_KEY_CHALLENGE "-flags"), QString::number(NetworkManager::Setting::NotSaved));
    } else {
        data.remove(QLatin1String(NM_OPENVPN3_KEY_CHALLENGE "-flags"));
    }

    if (needsCertPass) {
        storePasswordField(d->certPass, QLatin1String(NM_OPENVPN3_KEY_CERT_PASS), data, secrets, d->certPass->text());
    } else {
        // Saying a passphrase is agent-owned on a connection that has no key
        // to unlock would have the secrets prompt ask for one on every
        // connect, and nothing would ever use the answer.
        data.remove(QLatin1String(NM_OPENVPN3_KEY_CERT_PASS "-flags"));
        secrets.remove(QLatin1String(NM_OPENVPN3_KEY_CERT_PASS));
    }

    return vpnSettingOf(data, secrets);
}

QVariantMap OpenVpn3SettingWidget::vpnSettingOf(const NMStringMap &data, const NMStringMap &secrets) const
{
    NetworkManager::VpnSetting result;
    result.setServiceType(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    // The whole vpn setting is replaced by what this returns, so the
    // properties this page does not show have to be carried across --
    // otherwise saving would reset, say, a vpn.timeout set with nmcli.
    if (m_setting) {
        result.setPersistent(m_setting->persistent());
        result.setTimeout(m_setting->timeout());
        result.setUsername(m_setting->username());
    }
    result.setData(data);
    result.setSecrets(secrets);
    return result.toMap();
}

bool OpenVpn3SettingWidget::storedProfileIsUnreadable() const
{
    switch (m_availability) {
    case Openvpn3Storage::Availability::Locked:
    case Openvpn3Storage::Availability::Corrupt:
    case Openvpn3Storage::Availability::Unsupported:
    case Openvpn3Storage::Availability::UnusableFlags:
        return true;
    case Openvpn3Storage::Availability::Available:
    case Openvpn3Storage::Availability::Absent:
        return false;
    }
    return false;
}

QString OpenVpn3SettingWidget::blockingProblem() const
{
    if (storedProfileIsUnreadable()) {
        return refreshedStatusText(m_availability);
    }

    const int choice = d->storage->currentData().toInt();
    const bool wallet = Openvpn3Storage::secretServiceIsAvailable();
    if (!wallet && choice == kWalletStorage) {
        // The agent stores agent-owned secrets in a secret service and keeps
        // nothing when there is none. Writing the profile now would lose it.
        // Moving it to NetworkManager instead is a real change of who can
        // read the private keys, so it is offered, never done quietly.
        return i18n(
            "There is no password wallet available, so a profile stored for this user would not be stored at all. "
            "Unlock or set up the wallet, or choose to store the profile for all users -- which lets any program "
            "NetworkManager trusts read it.");
    }
    if (!wallet) {
        for (const PasswordField *field : {d->password, d->certPass}) {
            if (field->isEnabled() && flagsOf(field) == NetworkManager::Setting::AgentOwned && !field->text().isEmpty()) {
                return i18n(
                    "There is no password wallet available, so a password stored for this user would not be stored at all. "
                    "Unlock or set up the wallet, or choose to store it for all users or to be asked every time.");
            }
        }
    }

    // The one thing openvpn3 cannot do without. Everything else has a
    // default, depends on the authentication method, or is the server's to
    // push, so demanding it here would only get in the way.
    const Openvpn3Profile profile = currentProfile();
    const QStringList hosts = profile.remoteHosts();
    if (hosts.isEmpty()) {
        return i18n("The profile names no server to connect to.");
    }
    for (const QString &host : hosts) {
        if (host.trimmed().isEmpty()) {
            return i18n("A server row has no host name in it.");
        }
    }

    if (profile.needsNormalization()) {
        // Everything it points at has to be readable from here, now, because
        // what gets stored is one self-contained profile.
        const Openvpn3Import &result = normalized(profile.toText());
        if (!result.isValid()) {
            return i18n("The profile cannot be made self-contained: %1", result.errorMessage());
        }
        if (!wallet && !result.password().isEmpty() && flagsOf(d->password) == NetworkManager::Setting::AgentOwned) {
            return i18n("No password wallet is available to store the password in this profile. Choose password storage explicitly, "
                        "or set up the wallet before saving.");
        }
        if (result.needsUserPass() && result.username().isEmpty()
            && (hasCredentialSource(profile) || d->username->text().trimmed().isEmpty())) {
            return i18n("This profile asks for a username and password, and no username is set.");
        }
        return QString();
    }
    if (profile.containsOption(QStringLiteral("auth-user-pass")) && d->username->text().trimmed().isEmpty()) {
        // openvpn3 refuses to start a connection whose profile wants a
        // username and that has none, so saving one is saving a connection
        // that cannot come up.
        return i18n("This profile asks for a username and password, and no username is set.");
    }
    return QString();
}

bool OpenVpn3SettingWidget::isValid() const
{
    return blockingProblem().isEmpty();
}

// -- status, tabs, edits -----------------------------------------------------

QString OpenVpn3SettingWidget::refreshedStatusText(Openvpn3Storage::Availability availability)
{
    switch (availability) {
    case Openvpn3Storage::Availability::Available:
        return i18n("The profile is stored with this connection.");
    case Openvpn3Storage::Availability::Absent:
        return i18n("No profile yet. Import an OpenVPN profile, or write one on the Profile Source page.");
    case Openvpn3Storage::Availability::Locked:
        return i18n(
            "The profile is kept with this connection's secrets and was not available here. "
            "Unlock the wallet and reopen this page, or import a profile to replace it. "
            "Saving is blocked so the stored profile is not lost.");
    case Openvpn3Storage::Availability::Corrupt:
        return i18n("The stored profile cannot be read. Import a profile to replace it; saving is blocked until then.");
    case Openvpn3Storage::Availability::Unsupported:
        return i18n(
            "This connection stores its profile in a way this version of Plasma does not know. "
            "Update plasma-nm, or import a profile to replace it; saving is blocked until then.");
    case Openvpn3Storage::Availability::UnusableFlags:
        return i18n(
            "This connection says its profile is never stored, which cannot be true of a profile. "
            "Import a profile to repair it; saving is blocked until then.");
    }
    return QString();
}

void OpenVpn3SettingWidget::refreshStatus()
{
    // Why Save is unavailable, when it is: a disabled button with no reason
    // next to it is the same as no button. Otherwise, where the profile is.
    const QString problem = blockingProblem();
    d->status->setText(problem.isEmpty() ? refreshedStatusText(m_availability) : problem);

    const bool editable = !storedProfileIsUnreadable();
    d->tabs->setEnabled(editable);
}

void OpenVpn3SettingWidget::onTabChanged(int index)
{
    if (m_updating) {
        m_previousTab = index;
        return;
    }
    // Hand the document over from the view that had it to the new one. Which
    // view that is decides what currentProfile() reads, so it is taken while
    // the old page is still the current one.
    QWidget *previous = d->tabs->widget(m_previousTab);
    if (previous == d->directivesPage) {
        m_profile = d->directives->profile();
    } else if (previous == d->sourcePage) {
        m_profile = profileFromSource();
    }
    m_previousTab = index;
    takeCredentialsFromProfile();
    loadViewsFromProfile();
    refreshStatus();
}

void OpenVpn3SettingWidget::takeCredentialsFromProfile()
{
    if (!m_profile.needsNormalization()) {
        return;
    }
    // onTabChanged has already selected the incoming view. Only m_profile
    // holds the outgoing document at this point.
    const Openvpn3Import &result = normalized(m_profile.toText());
    if (!result.isValid()) {
        return; // refreshStatus() says why; nothing is thrown away meanwhile
    }
    // Credentials written into the document belong with the connection, and
    // showing them in the fields they will be saved from is the only way the
    // user can see that they are no longer in the profile.
    const bool wasUpdating = m_updating;
    m_updating = true;
    if (hasCredentialSource(m_profile)) {
        d->username->setText(result.username());
        d->password->setText(result.password());
        // Counted as an edit, even when it is empty. The password came out of
        // the user's own profile text and is about to be removed from it, so
        // the field is the only thing holding it; a secrets reply that was
        // already on its way would otherwise put the stored password back, or
        // clear the field when there was none. Setting the text here does not
        // mark it by itself, because m_updating silences the field's own
        // signal -- which it must, or this would count as a user edit of the
        // profile too.
        m_editedSecretValues.insert(QLatin1String(NM_OPENVPN3_KEY_PASSWORD));
    }
    // Consume successful normalization when handing the source back to the
    // other views. Later field edits must not be overwritten by credentials
    // still embedded in an older copy of the source.
    m_profile = Openvpn3Profile::fromText(result.profile());
    m_updating = wasUpdating;
}

void OpenVpn3SettingWidget::onProfileEdited()
{
    if (m_updating) {
        return;
    }
    m_profileEdited = true;
    refreshMaterialRows();
    refreshStatus();
    Q_EMIT validChanged(isValid());
    // Not slotWidgetChanged(), which only reports whether the page is valid:
    // the connection editor enables its Save button on settingChanged(), and
    // nothing else in this page emits it for an edit to the profile itself.
    Q_EMIT settingChanged();
}

void OpenVpn3SettingWidget::onFieldEdited()
{
    if (m_updating) {
        return;
    }
    // A username, a password or a different place to keep them is an unsaved
    // edit too, and an import would replace it just as surely as the profile.
    m_fieldsEdited = true;
    refreshStatus();
    Q_EMIT validChanged(isValid());
    Q_EMIT settingChanged();
}

// -- import ------------------------------------------------------------------

bool OpenVpn3SettingWidget::importWouldReplaceStoredCredentials() const
{
    return !m_secrets.value(QLatin1String(NM_OPENVPN3_KEY_PASSWORD)).isEmpty()
        || !m_secrets.value(QLatin1String(NM_OPENVPN3_KEY_CERT_PASS)).isEmpty() || !d->password->text().isEmpty() || !d->certPass->text().isEmpty();
}

void OpenVpn3SettingWidget::chooseProfileFile()
{
    QString warning;
    if (hasUnsavedEdits()) {
        warning = i18n("Importing a profile replaces everything on these pages, including the changes you have not saved yet.");
    } else if (importWouldReplaceStoredCredentials()) {
        // Not an unsaved edit, but worth stopping for all the same: the new
        // profile may well be for a different server, and the password stored
        // for the old one must not be sent to it.
        warning = i18n("Importing a profile replaces this connection's stored password with whatever the file carries, which may be nothing.");
    }
    if (!warning.isEmpty()) {
        const auto answer = QMessageBox::question(this,
                                                  i18nc("@title:window", "Discard Changes?"),
                                                  warning + QLatin1Char('\n') + i18n("Import anyway?"),
                                                  QMessageBox::Yes | QMessageBox::Cancel,
                                                  QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    const QString fileName =
        QFileDialog::getOpenFileName(this,
                                     i18nc("@title:window", "Import OpenVPN Profile"),
                                     QString(),
                                     i18n("OpenVPN profiles (*.ovpn *.conf);;All files (*)"));
    if (fileName.isEmpty()) {
        return;
    }

    QString errorMessage;
    if (!importProfile(fileName, &errorMessage)) {
        QMessageBox::warning(this, i18nc("@title:window", "Import Failed"), errorMessage);
    }
}

bool OpenVpn3SettingWidget::importProfile(const QString &fileName, QString *errorMessage)
{
    const Openvpn3Import import = Openvpn3Importer::fromFile(fileName);
    if (!import.isValid()) {
        // Nothing has been touched: a failed import is not a half-import.
        if (errorMessage) {
            *errorMessage = import.errorMessage();
        }
        return false;
    }

    NMStringMap data = m_data;
    NMStringMap secrets = m_secrets;
    // Where things are kept is what this page currently shows, not what the
    // connection was last saved with: an unsaved choice of system storage is
    // still the user's choice, and a reimport is not the place to undo it.
    if (!Openvpn3Importer::apply(import, data, secrets, chosenPolicy())) {
        if (errorMessage) {
            *errorMessage = import.errorMessage();
        }
        return false;
    }

    m_connectionSecretsSuperseded = true;
    m_data = data;
    m_secrets = secrets;
    m_availability = Openvpn3Storage::availability(m_data, m_secrets);
    setProfile(Openvpn3Profile::fromText(import.profile()));

    const bool wasUpdating = m_updating;
    m_updating = true;
    d->username->setText(import.username());
    // The new profile is a secret now, whatever the connection used before,
    // so the old public data item is no longer among the choices.
    refreshStorageChoices();
    d->storage->setCurrentIndex(d->storage->findData(storedStorageChoice()));
    fillPasswordField(d->password, QLatin1String(NM_OPENVPN3_KEY_PASSWORD), NetworkManager::Setting::AgentOwned);
    fillPasswordField(d->certPass, QLatin1String(NM_OPENVPN3_KEY_CERT_PASS), NetworkManager::Setting::AgentOwned);
    m_updating = wasUpdating;

    m_profileEdited = false;
    m_fieldsEdited = false;
    refreshStatus();
    Q_EMIT validChanged(isValid());
    Q_EMIT settingChanged();
    return true;
}

#include "moc_openvpn3widget.cpp"
