/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#ifndef PLASMA_NM_OPENVPN3_DIRECTIVES_WIDGET_H
#define PLASMA_NM_OPENVPN3_DIRECTIVES_WIDGET_H

#include <QWidget>

#include "openvpn3profile.h"

class QPlainTextEdit;
class QPushButton;
class QTableWidget;

/**
 * The profile as an ordered, duplicate-preserving table of entries.
 *
 * Everything the named fields on the other page do not know about lives here:
 * unknown directives, repeated ones, and @c <tag> blocks whose body is edited
 * in the box underneath the table. Order is meaningful to OpenVPN, so rows can
 * be moved, and nothing is merged or sorted behind the user.
 *
 * Formatting is not among it. Openvpn3Profile drops the comments and the blank
 * lines when it reads a profile, so there is nothing to show a row for and no
 * point in a button that adds one: the next time the connection was loaded it
 * would be gone. Every row is therefore a directive or a block, and every row
 * has a name -- which is why one cannot be cleared.
 */
class Openvpn3DirectivesWidget : public QWidget
{
    Q_OBJECT

public:
    explicit Openvpn3DirectivesWidget(QWidget *parent = nullptr);
    ~Openvpn3DirectivesWidget() override;

    void setProfile(const Openvpn3Profile &profile);
    Openvpn3Profile profile() const;

Q_SIGNALS:
    void changed();

private:
    void rebuild();
    void fillRow(int row, const Openvpn3Entry &entry);
    void onCellChanged(int row, int column);
    void onSelectionChanged();
    void onBodyChanged();
    void addEntry(const Openvpn3Entry &entry);
    void removeSelected();
    void moveSelected(int delta);
    int currentRow() const;

    Openvpn3Profile m_profile;
    QTableWidget *const m_table;
    QPlainTextEdit *const m_body;
    QPushButton *const m_removeButton;
    QPushButton *const m_upButton;
    QPushButton *const m_downButton;
    bool m_updating = false;
};

#endif // PLASMA_NM_OPENVPN3_DIRECTIVES_WIDGET_H
