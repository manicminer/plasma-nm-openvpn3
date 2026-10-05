/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "openvpn3directiveswidget.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <KLocalizedString>

namespace
{
enum Column {
    KindColumn,
    NameColumn,
    ArgumentsColumn,
    ColumnCount,
};

QString kindLabel(const Openvpn3Entry &entry)
{
    switch (entry.kind) {
    case Openvpn3Entry::Directive:
        return i18nc("@item an OpenVPN configuration directive", "Directive");
    case Openvpn3Entry::Block:
        return i18nc("@item an inline <tag>…</tag> block in an OpenVPN profile", "Block");
    }
    return QString();
}

/** The arguments of a directive as one editable, correctly quoted line. */
QString argumentsText(const Openvpn3Entry &entry)
{
    QStringList quoted;
    quoted.reserve(entry.arguments.size());
    for (const QString &argument : entry.arguments) {
        quoted.append(Openvpn3Profile::quoteArgument(argument));
    }
    return quoted.join(QLatin1Char(' '));
}
}

Openvpn3DirectivesWidget::Openvpn3DirectivesWidget(QWidget *parent)
    : QWidget(parent)
    , m_table(new QTableWidget(this))
    , m_body(new QPlainTextEdit(this))
    , m_removeButton(new QPushButton(i18nc("@action:button", "Remove"), this))
    , m_upButton(new QPushButton(i18nc("@action:button move an entry up", "Move Up"), this))
    , m_downButton(new QPushButton(i18nc("@action:button move an entry down", "Move Down"), this))
{
    auto layout = new QVBoxLayout(this);

    auto explanation = new QLabel(i18n("Every line of the profile, in order. Entries Plasma does not understand are kept exactly as they are."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    m_table->setObjectName(QStringLiteral("openvpn3_directives_table"));
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({
        i18nc("@title:column", "Kind"),
        i18nc("@title:column", "Name"),
        i18nc("@title:column", "Arguments"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(ArgumentsColumn, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_table, 1);

    auto buttons = new QHBoxLayout;
    auto addDirective = new QPushButton(i18nc("@action:button", "Add Directive"), this);
    addDirective->setObjectName(QStringLiteral("openvpn3_directives_add"));
    auto addBlock = new QPushButton(i18nc("@action:button", "Add Block"), this);
    addBlock->setObjectName(QStringLiteral("openvpn3_directives_add_block"));
    m_removeButton->setObjectName(QStringLiteral("openvpn3_directives_remove"));
    m_upButton->setObjectName(QStringLiteral("openvpn3_directives_up"));
    m_downButton->setObjectName(QStringLiteral("openvpn3_directives_down"));
    buttons->addWidget(addDirective);
    buttons->addWidget(addBlock);
    buttons->addStretch();
    buttons->addWidget(m_removeButton);
    buttons->addWidget(m_upButton);
    buttons->addWidget(m_downButton);
    layout->addLayout(buttons);

    auto bodyLabel = new QLabel(i18n("Contents of the selected block:"), this);
    layout->addWidget(bodyLabel);
    m_body->setObjectName(QStringLiteral("openvpn3_directives_body"));
    m_body->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_body->setEnabled(false);
    layout->addWidget(m_body, 1);

    connect(addDirective, &QPushButton::clicked, this, [this] {
        addEntry(Openvpn3Profile::directive(QStringLiteral("directive")));
    });
    connect(addBlock, &QPushButton::clicked, this, [this] {
        addEntry(Openvpn3Profile::block(QStringLiteral("tag"), QString()));
    });
    connect(m_removeButton, &QPushButton::clicked, this, &Openvpn3DirectivesWidget::removeSelected);
    connect(m_upButton, &QPushButton::clicked, this, [this] {
        moveSelected(-1);
    });
    connect(m_downButton, &QPushButton::clicked, this, [this] {
        moveSelected(1);
    });
    connect(m_table, &QTableWidget::cellChanged, this, &Openvpn3DirectivesWidget::onCellChanged);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &Openvpn3DirectivesWidget::onSelectionChanged);
    connect(m_body, &QPlainTextEdit::textChanged, this, &Openvpn3DirectivesWidget::onBodyChanged);

    onSelectionChanged();
}

Openvpn3DirectivesWidget::~Openvpn3DirectivesWidget() = default;

void Openvpn3DirectivesWidget::setProfile(const Openvpn3Profile &profile)
{
    m_profile = profile;
    rebuild();
}

Openvpn3Profile Openvpn3DirectivesWidget::profile() const
{
    return m_profile;
}

void Openvpn3DirectivesWidget::rebuild()
{
    const bool wasUpdating = m_updating;
    m_updating = true;
    const int selected = m_table->currentRow();

    m_table->setRowCount(m_profile.count());
    for (int row = 0; row < m_profile.count(); ++row) {
        fillRow(row, m_profile.at(row));
    }
    if (selected >= 0 && selected < m_table->rowCount()) {
        m_table->selectRow(selected);
    }

    m_updating = wasUpdating;
    onSelectionChanged();
}

void Openvpn3DirectivesWidget::fillRow(int row, const Openvpn3Entry &entry)
{
    auto take = [this, row](int column) {
        QTableWidgetItem *item = m_table->item(row, column);
        if (!item) {
            item = new QTableWidgetItem;
            m_table->setItem(row, column, item);
        }
        return item;
    };

    QTableWidgetItem *kind = take(KindColumn);
    kind->setText(kindLabel(entry));
    kind->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    // Every entry there is has a name, and it is the name that says what the
    // entry is, so every row's is editable.
    QTableWidgetItem *name = take(NameColumn);
    name->setText(entry.name);
    name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);

    QTableWidgetItem *arguments = take(ArgumentsColumn);
    switch (entry.kind) {
    case Openvpn3Entry::Directive:
        arguments->setText(argumentsText(entry));
        arguments->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
        break;
    case Openvpn3Entry::Block:
        arguments->setText(i18ncp("@item:intable the contents of an inline block", "%1 line", "%1 lines", entry.body.count(QLatin1Char('\n'))));
        arguments->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        break;
    }
}

void Openvpn3DirectivesWidget::onCellChanged(int row, int column)
{
    if (m_updating || row < 0 || row >= m_profile.count()) {
        return;
    }
    const Openvpn3Entry entry = m_profile.at(row);
    QTableWidgetItem *item = m_table->item(row, column);
    const QString text = item ? item->text() : QString();

    if (column == NameColumn) {
        const QString name = text.trimmed();
        if (name.isEmpty()) {
            // A nameless entry is a line with nothing on it, which the profile
            // does not keep: the row and its arguments would be gone the next
            // time the connection was loaded. So clearing a name is no edit at
            // all, and the cell goes back to saying what the entry is.
            if (item) {
                const bool wasUpdating = m_updating;
                m_updating = true;
                item->setText(entry.name);
                m_updating = wasUpdating;
            }
            return;
        }
        m_profile.replace(row, entry.isBlock() ? Openvpn3Profile::block(name, entry.body) : Openvpn3Profile::directive(name, entry.arguments));
    } else if (column == ArgumentsColumn && entry.isDirective()) {
        m_profile.setArguments(row, Openvpn3Profile::splitArguments(text));
    } else {
        return;
    }
    Q_EMIT changed();
}

void Openvpn3DirectivesWidget::onSelectionChanged()
{
    const int row = currentRow();
    const bool valid = row >= 0 && row < m_profile.count();
    const bool isBlock = valid && m_profile.at(row).isBlock();

    m_removeButton->setEnabled(valid);
    m_upButton->setEnabled(valid && row > 0);
    m_downButton->setEnabled(valid && row + 1 < m_profile.count());

    const bool wasUpdating = m_updating;
    m_updating = true;
    m_body->setEnabled(isBlock);
    m_body->setPlainText(isBlock ? m_profile.at(row).body : QString());
    m_updating = wasUpdating;
}

void Openvpn3DirectivesWidget::onBodyChanged()
{
    const int row = currentRow();
    if (m_updating || row < 0 || row >= m_profile.count() || !m_profile.at(row).isBlock()) {
        return;
    }
    m_profile.setBody(row, m_body->toPlainText());

    const bool wasUpdating = m_updating;
    m_updating = true;
    fillRow(row, m_profile.at(row));
    m_updating = wasUpdating;

    Q_EMIT changed();
}

void Openvpn3DirectivesWidget::addEntry(const Openvpn3Entry &entry)
{
    const int row = currentRow();
    const int at = row >= 0 ? row + 1 : m_profile.count();
    m_profile.insert(at, entry);
    rebuild();
    m_table->selectRow(at);
    Q_EMIT changed();
}

void Openvpn3DirectivesWidget::removeSelected()
{
    const int row = currentRow();
    if (row < 0 || row >= m_profile.count()) {
        return;
    }
    m_profile.removeAt(row);
    rebuild();
    Q_EMIT changed();
}

void Openvpn3DirectivesWidget::moveSelected(int delta)
{
    const int row = currentRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= m_profile.count()) {
        return;
    }
    m_profile.move(row, to);
    rebuild();
    m_table->selectRow(to);
    Q_EMIT changed();
}

int Openvpn3DirectivesWidget::currentRow() const
{
    return m_table->currentRow();
}
