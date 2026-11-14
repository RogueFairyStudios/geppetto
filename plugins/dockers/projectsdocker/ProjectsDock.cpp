/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsDock.h"

#include <functional>

#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisPart.h>
#include <KoFileDialog.h>

#include <KisVersioningDriver.h>

#include "ProjectsController.h"
#include "ProjectsDocumentHost.h"
#include "ProjectsModel.h"

using namespace KisVersioning;

ProjectsDock::ProjectsDock()
    : QDockWidget(i18n("Projects"))
{
    QWidget *page = new QWidget(this);
    setWidget(page);

    m_controller = ProjectsController::instance();
    if (!m_controller) {
        auto *layout = new QVBoxLayout(page);
        auto *label = new QLabel(i18n("Projects are not available in this installation."), page);
        label->setWordWrap(true);
        layout->addWidget(label);
        layout->addStretch();
        return;
    }

    buildInterface(page);

    ProjectsModel *model = m_controller->model();
    connect(m_controller, &ProjectsController::changed, this, &ProjectsDock::update);
    connect(m_controller, &ProjectsController::progress, this, &ProjectsDock::onSyncProgress);
    connect(m_controller, &ProjectsController::runFinished, this, [this](bool everythingWentWell) {
        // The notes went into the versions of this window's run.
        if (m_startedRun && everythingWentWell) {
            m_notesEdit->clear();
        }
        m_startedRun = false;
    });
    connect(model, &QAbstractItemModel::modelAboutToBeReset, this, &ProjectsDock::rememberExpanded);
    connect(model, &QAbstractItemModel::modelReset, this, &ProjectsDock::restoreExpanded);
    connect(model, &ProjectsModel::refreshed, this, &ProjectsDock::update);
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        m_controller->setPanelVisible(this, visible);
    });

    restoreExpanded();
    update();
}

ProjectsDock::~ProjectsDock()
{
    if (m_controller) {
        m_controller->setPanelVisible(this, false);
    }
}

void ProjectsDock::buildInterface(QWidget *page)
{
    const Capabilities capabilities = m_controller->driver()->capabilities();
    auto *layout = new QVBoxLayout(page);

    m_userLabel = new QLabel(page);
    m_userLabel->setWordWrap(true);
    layout->addWidget(m_userLabel);
    auto *accountRow = new QHBoxLayout();
    m_addressEdit = new QLineEdit(page);
    m_addressEdit->setPlaceholderText(i18n("Server address"));
    m_signInButton = new QPushButton(i18n("Sign in"), page);
    accountRow->addWidget(m_addressEdit, 1);
    accountRow->addWidget(m_signInButton);
    layout->addLayout(accountRow);
    connect(m_signInButton, &QPushButton::clicked, this, &ProjectsDock::signIn);
    connect(m_addressEdit, &QLineEdit::returnPressed, this, &ProjectsDock::signIn);

    m_addFolderButton = new QPushButton(i18n("Add folder…"), page);
    m_addFolderButton->setVisible(!capabilities.testFlag(RemoteProjects));
    layout->addWidget(m_addFolderButton);
    connect(m_addFolderButton, &QPushButton::clicked, this, &ProjectsDock::addFolder);

    m_view = new QTreeView(page);
    m_view->setModel(m_controller->model());
    m_view->setHeaderHidden(true);
    m_view->setUniformRowHeights(true);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_view->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_view, 1);
    connect(m_view, &QTreeView::doubleClicked, this, &ProjectsDock::openItem);
    connect(m_view, &QTreeView::customContextMenuRequested, this, &ProjectsDock::showContextMenu);

    m_messageLabel = new QLabel(page);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setVisible(false);
    layout->addWidget(m_messageLabel);

    m_summaryLabel = new QLabel(page);
    m_pendingLabel = new QLabel(i18n("pending upload"), page);
    m_pendingLabel->setVisible(false);
    auto *summaryRow = new QHBoxLayout();
    summaryRow->addWidget(m_summaryLabel, 1);
    summaryRow->addWidget(m_pendingLabel);
    layout->addLayout(summaryRow);

    m_notesEdit = new QLineEdit(page);
    m_notesEdit->setPlaceholderText(i18n("Notes (optional)"));
    m_notesEdit->setVisible(capabilities.testFlag(Notes));
    layout->addWidget(m_notesEdit);

    m_saveWarningLabel = new QLabel(page);
    m_saveWarningLabel->setWordWrap(true);
    m_saveWarningLabel->setVisible(false);
    layout->addWidget(m_saveWarningLabel);

    m_progressBar = new QProgressBar(page);
    m_progressBar->setVisible(false);
    layout->addWidget(m_progressBar);

    m_syncButton = new QPushButton(i18n("Synchronize"), page);
    layout->addWidget(m_syncButton);
    connect(m_syncButton, &QPushButton::clicked, this, &ProjectsDock::synchronise);
}

void ProjectsDock::update()
{
    using Account = ProjectsController::Account;
    const Account account = m_controller->account();
    const bool hasSignIn = account != Account::NotNeeded;

    // The account: who is signed in, or what signing in asks of the artist.
    QString accountText;
    if (m_controller->isSigningIn()) {
        accountText = m_controller->signInProgress();
    } else if (account == Account::SignedIn) {
        accountText = i18n("%1 · signed in", m_controller->user());
    }
    m_userLabel->setText(accountText);
    m_userLabel->setVisible(!accountText.isEmpty());
    const bool canSignIn = hasSignIn && (account == Account::SignedOut || account == Account::Unknown);
    m_addressEdit->setVisible(canSignIn && m_controller->serverAddress().isEmpty());
    m_signInButton->setVisible(canSignIn);
    m_signInButton->setEnabled(!m_controller->isSigningIn());

    const QStringList messages = m_controller->messages();
    m_messageLabel->setText(messages.join(QLatin1Char('\n')));
    m_messageLabel->setVisible(!messages.isEmpty());

    m_summaryLabel->setText(m_controller->model()->summaryText());
    m_pendingLabel->setVisible(m_controller->pendingUpload());

    const int toSave = m_controller->documentsToSave();
    m_saveWarningLabel->setText(i18np("1 open document will be saved", "%1 open documents will be saved", toSave));
    m_saveWarningLabel->setVisible(toSave > 0);

    const bool synchronising = m_controller->isSynchronising();
    if (synchronising && !m_progressBar->isVisible()) {
        m_progressBar->setRange(0, 0);
    }
    m_progressBar->setVisible(synchronising);
    m_syncButton->setEnabled(m_controller->canSynchronise());
    m_addFolderButton->setEnabled(m_controller->canChooseFolders());
    // The boxes follow whether choices are taken now.
    m_view->viewport()->update();
}

void ProjectsDock::signIn()
{
    if (m_controller->serverAddress().isEmpty() && m_addressEdit->text().trimmed().isEmpty()) {
        m_addressEdit->setFocus();
        return;
    }
    m_controller->signIn(m_addressEdit->text());
}

void ProjectsDock::addFolder()
{
    KoFileDialog dialog(this, KoFileDialog::OpenDirectory, QStringLiteral("ProjectsAddFolder"));
    dialog.setCaption(i18n("Choose a project folder"));
    const QString folder = dialog.filename();
    if (!folder.isEmpty()) {
        m_controller->addFolder(folder);
    }
}

void ProjectsDock::synchronise()
{
    if (!m_controller->canSynchronise()) {
        return;
    }
    const bool notes = m_controller->driver()->capabilities().testFlag(Notes);
    m_startedRun = true;
    m_progressBar->setRange(0, 0);
    m_controller->synchronise(notes ? m_notesEdit->text().trimmed() : QString());
}

void ProjectsDock::onSyncProgress(int project, int projectCount, qint64 done, qint64 total, const QString &label)
{
    const qint64 steps = 100;
    const int fraction = total > 0 ? int(qBound(qint64(0), done * steps / total, steps)) : 0;
    m_progressBar->setRange(0, projectCount * int(steps));
    m_progressBar->setValue(project * int(steps) + fraction);
    m_progressBar->setFormat(label.isEmpty() ? QStringLiteral("%p%") : label);
}

void ProjectsDock::openItem(const QModelIndex &index)
{
    const QString path = index.data(ProjectsModel::PathRole).toString();
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        return;
    }
    // A file already open is brought forward rather than opened a second time.
    if (!ProjectsDocumentHost::showDocument(path)) {
        KisPart::instance()->openExistingFile(path);
    }
}

void ProjectsDock::showContextMenu(const QPoint &position)
{
    const QModelIndex index = m_view->indexAt(position);
    if (!index.isValid() || !index.data(ProjectsModel::ConflictCopyRole).toBool() || !m_controller->canChooseFolders()) {
        return;
    }
    const QPersistentModelIndex copy(index.sibling(index.row(), 0));
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction *resolved = menu->addAction(i18n("Conflict resolved"));
    connect(resolved, &QAction::triggered, this, [this, copy]() {
        // The list may have been read again while the menu was open.
        if (copy.isValid()) {
            m_controller->markConflictResolved(copy);
        }
    });
    menu->popup(m_view->viewport()->mapToGlobal(position));
}

QString ProjectsDock::keyOf(const QModelIndex &index) const
{
    QStringList names;
    for (QModelIndex i = index; i.isValid(); i = i.parent()) {
        names.prepend(i.data().toString());
    }
    return names.join(QLatin1Char('/'));
}

void ProjectsDock::rememberExpanded()
{
    m_expanded.clear();
    ProjectsModel *model = m_controller->model();
    std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            const QModelIndex index = model->index(row, 0, parent);
            if (m_view->isExpanded(index)) {
                m_expanded.insert(keyOf(index));
                walk(index);
            }
        }
    };
    walk(QModelIndex());
}

void ProjectsDock::restoreExpanded()
{
    // The list is read again after every change: what was open stays open, and a project seen
    // for the first time opens.
    ProjectsModel *model = m_controller->model();
    std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            const QModelIndex index = model->index(row, 0, parent);
            const QString key = keyOf(index);
            bool expand = m_expanded.contains(key);
            if (!parent.isValid() && !m_knownProjects.contains(key)) {
                m_knownProjects.insert(key);
                expand = true;
            }
            if (expand) {
                m_view->setExpanded(index, true);
                walk(index);
            }
        }
    };
    walk(QModelIndex());
}
