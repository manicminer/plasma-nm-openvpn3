/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#ifndef PLASMA_NM_OPENVPN3_WIDGET_H
#define PLASMA_NM_OPENVPN3_WIDGET_H

#include <NetworkManagerQt/VpnSetting>
#include <QSet>

#include "openvpn3importer.h"
#include "openvpn3profile.h"
#include "openvpn3storage.h"
#include "settingwidget.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;

class Openvpn3DirectivesWidget;
class PasswordField;

/**
 * The editor page for an org.freedesktop.NetworkManager.openvpn3 connection.
 *
 * The profile itself is the document being edited, and it is edited in three
 * views of the same thing: named fields for what is worth naming, a table of
 * every entry in order, and the profile text. Whichever view is in front owns
 * the document; switching views hands it over. Nothing the editor does not
 * understand is dropped on the way through -- see Openvpn3Profile.
 *
 * Secrets follow Openvpn3Storage: the whole profile can be a NetworkManager
 * secret kept by the wallet, or NetworkManager's own for unattended use. When
 * the profile is one and was not handed to the editor, the page refuses to be
 * valid rather than letting a save replace it with nothing.
 */
class OpenVpn3SettingWidget : public SettingWidget
{
    Q_OBJECT

public:
    explicit OpenVpn3SettingWidget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent = nullptr);
    ~OpenVpn3SettingWidget() override;

    void loadConfig(const NetworkManager::Setting::Ptr &setting) override;
    void loadSecrets(const NetworkManager::Setting::Ptr &setting) override;
    QVariantMap setting() const override;
    bool isValid() const override;

    /**
     * Replaces the VPN configuration with the one in @p fileName.
     *
     * All or nothing: if the file cannot be imported nothing changes at all.
     * The connection's identity, addresses, routes and permissions are not
     * this widget's to touch, so a reimport never reaches them.
     *
     * @return false and sets @p errorMessage when the import failed.
     */
    bool importProfile(const QString &fileName, QString *errorMessage = nullptr);

    /** True when the profile was changed since it was last loaded or imported. */
    bool hasProfileEdits() const
    {
        return m_profileEdited;
    }

    /**
     * True when anything at all was changed since the last load or import.
     *
     * Not only the profile: a password typed in, a different storage choice or
     * a changed username are edits an import would throw away just the same,
     * so they are what the confirmation before an import goes by.
     */
    bool hasUnsavedEdits() const
    {
        return m_profileEdited || m_fieldsEdited;
    }

    /**
     * True when importing would replace credentials this connection has.
     *
     * Worth asking about even when nothing was edited: the new profile is for
     * a server that may well not be the old one, and quietly reusing the old
     * password against it is the one thing that must not happen.
     */
    bool importWouldReplaceStoredCredentials() const;

    /** The profile as it would be stored. Empty when none is available. */
    QString profileText() const;

    /**
     * Embeds @p fileName into the profile as a @p directive block.
     *
     * The file replaces any reference of that name, so nothing points outside
     * the profile afterwards. A file that is not text cannot be a PEM block
     * and is refused rather than mangled into one.
     *
     * @return false and sets @p errorMessage when the file was not embedded.
     */
    bool embedFile(const QString &directive, const QString &fileName, QString *errorMessage = nullptr);

    /** Removes every @p directive entry, reference or embedded block alike. */
    void clearMaterial(const QString &directive);

    /**
     * Why this connection cannot be saved as it stands, for the user to read.
     *
     * Empty when it can. Everything isValid() refuses is in here, which is
     * what keeps a disabled Save button from being a mystery.
     */
    QString blockingProblem() const;

    /**
     * True when the connection has a profile this editor could not read.
     *
     * Saving then has to be blocked: NetworkManager replaces a connection's
     * secrets wholesale, so writing back what we have would delete the
     * profile we never saw. Importing one is the way out.
     */
    bool storedProfileIsUnreadable() const;

private:
    /** The profile storage the connection already uses, as combo box data. */
    int storedStorageChoice() const;
    /** The storage policy the combo box and the password fields ask for. */
    Openvpn3Policy chosenPolicy() const;
    /** Secret flags of @p field, as its chosen option means them. */
    static NetworkManager::Setting::SecretFlags flagsOf(const PasswordField *field);

    void buildUi();
    QWidget *buildGeneralPage();

    void setProfile(const Openvpn3Profile &profile);
    Openvpn3Profile currentProfile() const;
    /** The profile the source view holds; the one it was given back
     * unchanged, when nothing in it was actually edited. */
    Openvpn3Profile profileFromSource() const;
    void takeProfileFromCurrentView();
    void loadViewsFromProfile();
    void loadGeneralFromProfile();
    void loadRemotesFromProfile();

    void onTabChanged(int index);
    void onProfileEdited();
    void onFieldEdited();
    void chooseProfileFile();

    void applySimpleDirective(const QString &name, const QString &value);
    void applyPresence(const QString &name, bool present);
    void applyRemotes();
    void addRemoteRow(const QString &host, const QString &port, const QString &transport, const QStringList &extras, quint64 id);

    void refreshStatus();
    void refreshMaterialRows();
    void refreshStorageChoices();
    //! What the page says about a profile it has, or has not, got.
    static QString refreshedStatusText(Openvpn3Storage::Availability availability, const NMStringMap &data);

    struct MaterialRow;
    void addMaterialRow(class QFormLayout *form, const QString &label, const QString &directive, bool base64, const QString &filter);
    void loadMaterial(const MaterialRow &row);

    void fillPasswordField(PasswordField *field, const QString &key, NetworkManager::Setting::SecretFlags fallback) const;
    void storePasswordField(const PasswordField *field, const QString &key, NMStringMap &data, NMStringMap &secrets, const QString &text) const;

    /** What the profile currently in front looks like once the backend has
     * made it self-contained; cached, because the answer costs an import. */
    struct Normalized {
        QString of; //!< the profile text it was worked out from
        Openvpn3Import result;
    };
    const Openvpn3Import &normalized(const QString &text) const;
    /** @p data and @p secrets as a whole vpn setting, with the properties
     * this page does not show carried across from the one it was given. */
    QVariantMap vpnSettingOf(const NMStringMap &data, const NMStringMap &secrets) const;
    //! Fills the username and password fields in from a normalised profile.
    void takeCredentialsFromProfile();

    class Private;
    Private *const d;

    NetworkManager::VpnSetting::Ptr m_setting;
    //! Everything the connection holds, so keys this editor does not model survive.
    NMStringMap m_data;
    NMStringMap m_secrets;
    Openvpn3Profile m_profile;
    Openvpn3Storage::Availability m_availability = Openvpn3Storage::Availability::Absent;
    //! A successful reimport supersedes pending secrets from the loaded connection.
    bool m_connectionSecretsSuperseded = false;
    bool m_profileEdited = false;
    bool m_fieldsEdited = false;
    QSet<QString> m_editedSecretValues;
    QSet<QString> m_editedSecretFlags;
    bool m_updating = false;
    int m_previousTab = 0;
    //! The source view's text as this widget last put it there; see currentProfile().
    QString m_sourceBaseline;
    mutable Normalized m_normalized;
};

#endif // PLASMA_NM_OPENVPN3_WIDGET_H
