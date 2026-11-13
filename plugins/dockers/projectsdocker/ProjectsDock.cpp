/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsDock.h"

#include <functional>

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <KoFileDialog.h>

#include <KisVersioningDriver.h>
#include <KisVersioningDriverRegistry.h>
#include <KisVersioningJob.h>

#include "ProjectsDocumentHost.h"
#include "ProjectsModel.h"
#include "ProjectsPresence.h"
#include "ProjectsSyncRunner.h"

using namespace KisVersioning;

ProjectsDock::ProjectsDock()
    : QDockWidget(i18n("Projects"))
{
    QWidget *page = new QWidget(this);
    setWidget(page);

    // The driver chosen in the settings, or the folder on this computer when that one is missing.
    KisVersioningDriverRegistry *registry = KisVersioningDriverRegistry::instance();
    KisVersioningDriverFactory *factory = registry->value(m_settings.driverId());
    if (!factory) {
        factory = registry->value(QStringLiteral("localfolder"));
    }
    if (!factory) {
        auto *layout = new QVBoxLayout(page);
        auto *label = new QLabel(i18n("Projects are not available in this installation."), page);
        label->setWordWrap(true);
        layout->addWidget(label);
        layout->addStretch();
        return;
    }

    m_driver = factory->create(this);
    m_host = new ProjectsDocumentHost(this);
    m_model = new ProjectsModel(m_driver, &m_settings, this);
    m_runner = new ProjectsSyncRunner(m_driver, m_host, this);
    m_presence = new ProjectsPresence(m_driver, &m_settings, this);
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(500);
    m_signedIn = m_driver->capabilities().testFlag(SignIn) && !m_driver->currentUser().isEmpty();

    buildInterface(page);

    connect(m_refreshTimer, &QTimer::timeout, m_model, &ProjectsModel::refresh);
    connect(m_model, &ProjectsModel::refreshed, this, &ProjectsDock::onRefreshed);
    connect(m_model, &ProjectsModel::operationFailed, this, [this](const QString &text) { showMessages({text}); });
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, &ProjectsDock::rememberExpanded);
    connect(m_model, &QAbstractItemModel::modelReset, this, &ProjectsDock::restoreExpanded);
    connect(m_runner, &ProjectsSyncRunner::progress, this, &ProjectsDock::onSyncProgress);
    connect(m_runner, &ProjectsSyncRunner::projectFinished, this, &ProjectsDock::onProjectSynchronised);
    connect(m_runner, &ProjectsSyncRunner::finished, this, &ProjectsDock::onSyncFinished);

    // The warning about open documents follows their changes; a save changes the state of a file.
    KisPart *part = KisPart::instance();
    connect(part, &KisPart::sigDocumentAdded, this, [this](KisDocument *document) {
        watchDocument(document);
        updateSaveWarning();
    });
    connect(part, &KisPart::sigDocumentRemoved, this, &ProjectsDock::updateSaveWarning);
    connect(part, &KisPart::sigDocumentRemoved, this, [this](const QString &path) {
        m_presence->documentClosed(path, m_unsavedDocuments.remove(ProjectsDocumentHost::pathKey(path)));
    });
    // Opening a file that someone else is editing: say so. Nobody is prevented from editing.
    connect(part, &KisPart::sigViewAdded, this, [this](KisView *view) {
        if (view && view->document()) {
            m_presence->documentActivated(view->document()->path());
        }
    });
    connect(m_presence, &ProjectsPresence::someoneElseIsEditing, this, [](const QString &, const QString &user) {
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
        if (window && window->viewManager()) {
            window->viewManager()->showFloatingMessage(i18n("%1 is editing this file", user), QIcon());
        }
    });
    connect(part, &KisPart::sigDocumentSaved, this, &ProjectsDock::onDocumentSaved);
    const QList<QPointer<KisDocument>> documents = part->documents();
    for (const QPointer<KisDocument> &document : documents) {
        if (document) {
            watchDocument(document);
        }
    }

    updateAccount();
    updateSaveWarning();
    m_model->refresh();
}

ProjectsDock::~ProjectsDock()
{
    // What holds the settings, the driver or the host goes first, while all three still exist.
    delete m_presence;
    delete m_runner;
    delete m_model;
    delete m_host;
    delete m_driver;
}

void ProjectsDock::buildInterface(QWidget *page)
{
    const Capabilities capabilities = m_driver->capabilities();
    auto *layout = new QVBoxLayout(page);

    m_userLabel = new QLabel(page);
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
    m_view->setModel(m_model);
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

void ProjectsDock::updateAccount()
{
    // A driver without sign-in has no address and no button.
    const bool hasSignIn = m_driver->capabilities().testFlag(SignIn);
    m_userLabel->setVisible(hasSignIn && m_signedIn);
    if (hasSignIn && m_signedIn) {
        m_userLabel->setText(i18n("%1 · signed in", m_driver->currentUser()));
    }
    m_addressEdit->setVisible(hasSignIn && !m_signedIn && m_settings.serverAddress().isEmpty());
    m_signInButton->setVisible(hasSignIn && !m_signedIn);
}

void ProjectsDock::signIn()
{
    QString address = m_settings.serverAddress();
    if (address.isEmpty()) {
        address = m_addressEdit->text().trimmed();
    }
    if (address.isEmpty()) {
        m_addressEdit->setFocus();
        return;
    }
    m_signInButton->setEnabled(false);
    KisVersioningJob *job = m_driver->signIn(address);
    // The job deletes itself after finished(): its answer is read in the slot.
    connect(job, &KisVersioningJob::finished, this, [this, address](KisVersioningJob *job) {
        m_signInButton->setEnabled(true);
        if (job->error() != KisVersioningJob::NoError) {
            showMessages({job->errorText()});
            return;
        }
        m_settings.setServerAddress(address);
        m_signedIn = true;
        showMessages({});
        updateAccount();
        m_model->refresh();
    });
}

void ProjectsDock::addFolder()
{
    KoFileDialog dialog(this, KoFileDialog::OpenDirectory, QStringLiteral("ProjectsAddFolder"));
    dialog.setCaption(i18n("Choose a project folder"));
    const QString folder = dialog.filename();
    if (!folder.isEmpty()) {
        m_model->addFolder(folder);
    }
}

void ProjectsDock::synchronise()
{
    if (m_runner->isRunning()) {
        return;
    }
    // A project whose folder is missing is already shown with its error; the others go ahead.
    QStringList projects;
    const QStringList downloaded = m_model->downloadedProjectPaths();
    for (const QString &path : downloaded) {
        if (QFileInfo(path).isDir()) {
            projects << path;
        }
    }
    const QString notes = (m_driver->capabilities() & Notes) ? m_notesEdit->text().trimmed() : QString();

    m_syncMessages.clear();
    showMessages({});
    m_syncButton->setEnabled(false);
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);
    m_runner->start(projects, notes);
}

void ProjectsDock::onSyncProgress(int project, int projectCount, qint64 done, qint64 total, const QString &label)
{
    const qint64 steps = 100;
    const int fraction = total > 0 ? int(qBound(qint64(0), done * steps / total, steps)) : 0;
    m_progressBar->setRange(0, projectCount * steps);
    m_progressBar->setValue(project * steps + fraction);
    m_progressBar->setFormat(label.isEmpty() ? QStringLiteral("%p%") : label);
}

void ProjectsDock::onProjectSynchronised(int project)
{
    const ProjectsSyncRunner::Result result = m_runner->results().at(project);
    const KisVersioningSyncSession::Report &report = result.report;
    if (report.error == KisVersioningJob::NoError) {
        m_presence->projectSynchronised(result.projectPath);
    }
    for (const QString &path : report.keptBoth) {
        m_syncMessages << i18n("Both versions of %1 were kept", QFileInfo(path).fileName());
    }
    // Received or kept both while the document had unsaved changes: it was not reloaded.
    for (const QString &path : result.staleDocuments) {
        m_syncMessages << i18n("%1 was replaced by the other version while you kept working. "
                               "Save your work under another name.",
                               QFileInfo(path).fileName());
    }
    if (report.error == KisVersioningJob::NotSignedIn && (m_driver->capabilities() & SignIn)) {
        m_signedIn = false;
        updateAccount();
        if (!m_syncMessages.contains(i18n("Sign in again to send your work"))) {
            m_syncMessages << i18n("Sign in again to send your work");
        }
    } else if (report.error != KisVersioningJob::NoError) {
        m_syncMessages << report.errorText;
    }
}

void ProjectsDock::onSyncFinished()
{
    bool allSent = true;
    const QVector<ProjectsSyncRunner::Result> results = m_runner->results();
    for (const ProjectsSyncRunner::Result &result : results) {
        allSent = allSent && result.report.error == KisVersioningJob::NoError;
    }
    if (allSent) {
        m_notesEdit->clear();
    }
    showMessages(m_syncMessages);
    m_progressBar->setVisible(false);
    m_syncButton->setEnabled(true);
    m_model->refresh();
}

void ProjectsDock::onRefreshed()
{
    m_summaryLabel->setText(m_model->summaryText());
    m_pendingLabel->setVisible(m_model->pendingUploadCount() > 0);
    if (m_model->signInNeeded() && (m_driver->capabilities() & SignIn) && m_signedIn) {
        // The session expired: back to the signed-out state.
        m_signedIn = false;
        showMessages({i18n("Sign in again to send your work")});
    }
    updateAccount();
    updateSaveWarning();
}

void ProjectsDock::openItem(const QModelIndex &index)
{
    const QString path = index.data(ProjectsModel::PathRole).toString();
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        return;
    }
    // A file already open is brought forward rather than opened a second time.
    if (!m_host->showDocument(path)) {
        KisPart::instance()->openExistingFile(path);
    }
}

void ProjectsDock::showContextMenu(const QPoint &position)
{
    const QModelIndex index = m_view->indexAt(position);
    if (!index.isValid() || !index.data(ProjectsModel::ConflictCopyRole).toBool()) {
        return;
    }
    const QPersistentModelIndex copy(index.sibling(index.row(), 0));
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction *resolved = menu->addAction(i18n("Conflict resolved"));
    connect(resolved, &QAction::triggered, this, [this, copy]() {
        // The list may have been read again while the menu was open.
        if (copy.isValid()) {
            m_model->markConflictResolved(copy);
        }
    });
    menu->popup(m_view->viewport()->mapToGlobal(position));
}

void ProjectsDock::updateSaveWarning()
{
    int count = 0;
    const QStringList projects = m_model->downloadedProjectPaths();
    for (const QString &project : projects) {
        count += m_host->modifiedOpenDocuments(project).size();
    }
    m_saveWarningLabel->setText(i18np("1 open document will be saved", "%1 open documents will be saved", count));
    m_saveWarningLabel->setVisible(count > 0);
}

void ProjectsDock::watchDocument(KisDocument *document)
{
    connect(document, &KisDocument::modified, this, &ProjectsDock::updateSaveWarning, Qt::UniqueConnection);
    connect(document, &KisDocument::modified, this, [this, document](bool modified) {
        const QString key = ProjectsDocumentHost::pathKey(document->path());
        if (modified) {
            m_unsavedDocuments.insert(key);
            m_presence->documentModified(document->path());
        } else {
            m_unsavedDocuments.remove(key);
        }
    });
}

void ProjectsDock::onDocumentSaved(const QString &path)
{
    updateSaveWarning();
    const QStringList projects = m_model->downloadedProjectPaths();
    for (const QString &project : projects) {
        if (ProjectsDocumentHost::isInside(path, project)) {
            m_refreshTimer->start();
            return;
        }
    }
}

void ProjectsDock::showMessages(const QStringList &messages)
{
    QStringList shown;
    for (const QString &message : messages) {
        if (!message.isEmpty()) {
            shown << message;
        }
    }
    m_messageLabel->setText(shown.join(QLatin1Char('\n')));
    m_messageLabel->setVisible(!shown.isEmpty());
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
    std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        for (int row = 0; row < m_model->rowCount(parent); ++row) {
            const QModelIndex index = m_model->index(row, 0, parent);
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
    std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        for (int row = 0; row < m_model->rowCount(parent); ++row) {
            const QModelIndex index = m_model->index(row, 0, parent);
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
