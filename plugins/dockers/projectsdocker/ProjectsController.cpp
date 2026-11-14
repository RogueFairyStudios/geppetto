/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsController.h"

#include <memory>

#include <QCoreApplication>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>

#include <klocalizedstring.h>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>

#include <KisVersioningDriver.h>
#include <KisVersioningDriverRegistry.h>
#include <KisVersioningJob.h>
#include <KisVersioningSettings.h>

#include "ProjectsDocumentHost.h"
#include "ProjectsModel.h"
#include "ProjectsPresence.h"
#include "ProjectsSyncRunner.h"

using namespace KisVersioning;

namespace {

/// The application's controller with what it works with. They go in this order: the controller
/// first, while the documents, the driver and the settings still exist.
struct Owned : public QObject {
    ~Owned() override
    {
        delete controller;
        delete documents;
        delete driver;
    }

    std::unique_ptr<KisVersioningSettings> settings;
    KisVersioningDriver *driver {nullptr};
    ProjectsDocumentHost *documents {nullptr};
    QPointer<ProjectsController> controller;
};

} // namespace

ProjectsController *ProjectsController::instance()
{
    static QPointer<ProjectsController> s_instance;
    static bool s_made = false;
    if (s_made) {
        return s_instance;
    }
    s_made = true;

    auto settings = std::make_unique<KisVersioningSettings>();
    // The driver chosen in the settings, or the folder on this computer when that one is missing.
    KisVersioningDriverRegistry *registry = KisVersioningDriverRegistry::instance();
    KisVersioningDriverFactory *factory = registry->value(settings->driverId());
    if (!factory) {
        factory = registry->value(QStringLiteral("localfolder"));
    }
    if (!factory) {
        return nullptr;
    }

    // Everything lives as long as the application.
    auto *owned = new Owned;
    owned->setParent(QCoreApplication::instance());
    owned->settings = std::move(settings);
    owned->driver = factory->create(nullptr);
    owned->documents = new ProjectsDocumentHost();
    owned->controller = new ProjectsController(owned->driver, owned->documents, owned->settings.get());
    s_instance = owned->controller;
    s_instance->connectToApplication();
    s_instance->start();
    return s_instance;
}

ProjectsController::ProjectsController(KisVersioningDriver *driver, ProjectsDocuments *documents,
                                       KisVersioningSettings *settings, QObject *parent)
    : QObject(parent)
    , m_driver(driver)
    , m_documents(documents)
    , m_settings(settings)
    , m_model(new ProjectsModel(driver, settings, this))
    , m_runner(new ProjectsSyncRunner(driver, documents, this))
    , m_presence(new ProjectsPresence(driver, settings, this))
    , m_saveRefresh(new QTimer(this))
    , m_slowRefresh(new QTimer(this))
{
    m_saveRefresh->setSingleShot(true);
    m_saveRefresh->setInterval(500);
    m_slowRefresh->setInterval(60000);
    connect(m_saveRefresh, &QTimer::timeout, this, &ProjectsController::requestRefresh);
    connect(m_slowRefresh, &QTimer::timeout, this, &ProjectsController::requestRefresh);

    connect(m_model, &ProjectsModel::refreshed, this, &ProjectsController::onRefreshed);
    connect(m_model, &ProjectsModel::operationFailed, this, [this](const QString &text) { showMessages({text}); });
    connect(m_model, &ProjectsModel::busyChanged, this, &ProjectsController::changed);
    connect(m_runner, &ProjectsSyncRunner::progress, this, &ProjectsController::progress);
    connect(m_runner, &ProjectsSyncRunner::projectFinished, this, &ProjectsController::onProjectSynchronised);
    connect(m_runner, &ProjectsSyncRunner::finished, this, &ProjectsController::onRunFinished);
    connect(m_presence, &ProjectsPresence::someoneElseIsEditing, this, &ProjectsController::someoneElseIsEditing);

    if (!m_driver->capabilities().testFlag(SignIn)) {
        m_account = Account::NotNeeded;
    } else {
        m_account = m_driver->currentUser().isEmpty() ? Account::SignedOut : Account::SignedIn;
    }
    updateChoices();
}

ProjectsController::~ProjectsController()
{
    // What holds the settings, the driver or the documents goes first, while all three still exist.
    delete m_presence;
    delete m_runner;
    delete m_model;
}

void ProjectsController::start()
{
    m_model->refresh();
}

KisVersioningDriver *ProjectsController::driver() const
{
    return m_driver;
}

ProjectsModel *ProjectsController::model() const
{
    return m_model;
}

QString ProjectsController::serverAddress() const
{
    return m_settings->serverAddress();
}

ProjectsController::Account ProjectsController::account() const
{
    return m_account;
}

QString ProjectsController::user() const
{
    return m_account == Account::SignedIn ? m_driver->currentUser() : QString();
}

bool ProjectsController::isSigningIn() const
{
    return m_signingIn;
}

QString ProjectsController::signInProgress() const
{
    return m_signInProgress;
}

bool ProjectsController::isSynchronising() const
{
    return m_runRequested || m_runner->isRunning();
}

bool ProjectsController::canSynchronise() const
{
    // A folder choice under way works on a project too: the run waits for it to end.
    return !isSynchronising() && m_account != Account::SignedOut && m_account != Account::Checking
        && !m_model->isBusy();
}

bool ProjectsController::canChooseFolders() const
{
    return !isSynchronising() && m_account != Account::SignedOut && m_account != Account::Checking;
}

bool ProjectsController::pendingUpload() const
{
    return m_model->pendingUploadCount() > 0 || !m_pendingProjects.isEmpty();
}

QStringList ProjectsController::messages() const
{
    return m_messages;
}

int ProjectsController::documentsToSave() const
{
    int count = 0;
    const QStringList projects = m_model->downloadedProjectPaths();
    for (const QString &project : projects) {
        count += m_documents->modifiedOpenDocuments(project).size();
    }
    return count;
}

void ProjectsController::showMessages(const QStringList &messages)
{
    m_messages.clear();
    for (const QString &message : messages) {
        if (!message.isEmpty() && !m_messages.contains(message)) {
            m_messages << message;
        }
    }
    Q_EMIT changed();
}

void ProjectsController::synchronise(const QString &notes)
{
    if (!canSynchronise()) {
        return;
    }
    m_requestedNotes = notes;
    m_runRequested = true;
    m_runMessages.clear();
    m_messages.clear();
    // From now on nothing else reaches the driver until the run ends.
    updateChoices();
    m_presence->setHeld(true);
    Q_EMIT changed();
    if (!m_model->isRefreshing()) {
        startRun();
    }
    // Otherwise the run starts once the list being read has arrived.
}

void ProjectsController::startRun()
{
    m_runRequested = false;
    // A project whose folder is missing is already shown with its error; the others go ahead.
    QStringList projects;
    const QStringList downloaded = m_model->downloadedProjectPaths();
    for (const QString &path : downloaded) {
        if (QFileInfo(path).isDir()) {
            projects << path;
        }
    }
    m_runner->start(projects, m_requestedNotes);
}

void ProjectsController::requestRefresh()
{
    if (isSynchronising()) {
        // The list is read once the run ends.
        m_refreshAfterRun = true;
        return;
    }
    m_model->refresh();
}

void ProjectsController::onRefreshed()
{
    if (m_model->signInNeeded() && m_driver->capabilities().testFlag(SignIn) && m_account != Account::SignedOut) {
        // The session expired: back to the signed-out state.
        setAccount(Account::SignedOut);
        m_messages = QStringList{i18n("Sign in again to send your work")};
    }
    if (m_runRequested) {
        startRun();
    }
    Q_EMIT changed();
}

void ProjectsController::onProjectSynchronised(int project)
{
    const ProjectsSyncRunner::Result result = m_runner->results().at(project);
    const KisVersioningSyncSession::Report &report = result.report;
    const QString key = ProjectsDocumentHost::pathKey(result.projectPath);

    // Work kept here stays shown as waiting until a later run of the project sends it. A run that
    // failed proves nothing either way: what was shown stays.
    if (report.pendingUpload) {
        m_pendingProjects.insert(key);
    } else if (report.error == KisVersioningJob::NoError) {
        m_pendingProjects.remove(key);
    }

    // The editing notices go only when everything of the project reached the server.
    if (report.error == KisVersioningJob::NoError && !report.pendingUpload) {
        m_presence->projectSynchronised(result.projectPath);
    }

    for (const QString &path : report.keptBoth) {
        m_runMessages << i18n("Both versions of %1 were kept", QFileInfo(path).fileName());
    }
    // Received or kept both while the document had unsaved changes: it was not reloaded.
    for (const QString &path : result.staleDocuments) {
        m_runMessages << i18n("%1 was replaced by the other version while you kept working. "
                              "Save your work under another name.",
                              QFileInfo(path).fileName());
    }
    if (report.error == KisVersioningJob::NotSignedIn && m_driver->capabilities().testFlag(SignIn)) {
        setAccount(Account::SignedOut);
        m_runMessages << i18n("Sign in again to send your work");
    } else if (report.error != KisVersioningJob::NoError) {
        m_runMessages << report.errorText;
    } else if (report.pendingUpload) {
        m_runMessages << i18n("Your work is kept on this computer and will be sent the next time you synchronize.");
    }
}

void ProjectsController::onRunFinished()
{
    bool everythingWentWell = true;
    const QVector<ProjectsSyncRunner::Result> results = m_runner->results();
    for (const ProjectsSyncRunner::Result &result : results) {
        everythingWentWell = everythingWentWell && result.report.error == KisVersioningJob::NoError;
    }
    updateChoices();
    m_presence->setHeld(false);
    // The list is read again after every run, which also answers any refresh asked for meanwhile.
    m_refreshAfterRun = false;
    m_model->refresh();
    showMessages(m_runMessages);
    Q_EMIT runFinished(everythingWentWell);
}

void ProjectsController::signIn(const QString &serverAddress)
{
    QString address = m_settings->serverAddress();
    if (address.isEmpty()) {
        address = serverAddress.trimmed();
    }
    if (address.isEmpty() || m_signingIn) {
        return;
    }
    m_signingIn = true;
    m_signInProgress.clear();
    Q_EMIT changed();
    KisVersioningJob *job = m_driver->signIn(address);
    connect(job, &KisVersioningJob::progress, this, [this](qint64, qint64, const QString &label) {
        m_signInProgress = label;
        Q_EMIT changed();
    });
    // The job deletes itself after finished(): its answer is read in the slot.
    connect(job, &KisVersioningJob::finished, this, [this, address](KisVersioningJob *finished) {
        m_signingIn = false;
        m_signInProgress.clear();
        if (finished->error() != KisVersioningJob::NoError) {
            showMessages({finished->errorText()});
            return;
        }
        m_settings->setServerAddress(address);
        setAccount(Account::SignedIn);
        showMessages({});
        requestRefresh();
    });
}

void ProjectsController::addFolder(const QString &path)
{
    if (canChooseFolders()) {
        m_model->addFolder(path);
    }
}

void ProjectsController::markConflictResolved(const QModelIndex &index)
{
    if (canChooseFolders()) {
        m_model->markConflictResolved(index);
    }
}

void ProjectsController::setPanelVisible(QObject *panel, bool visible)
{
    if (visible) {
        m_visiblePanels.insert(panel);
    } else {
        m_visiblePanels.remove(panel);
    }
    if (m_visiblePanels.isEmpty()) {
        m_slowRefresh->stop();
    } else if (!m_slowRefresh->isActive()) {
        m_slowRefresh->start();
    }
}

void ProjectsController::setSlowRefreshInterval(int milliseconds)
{
    m_slowRefresh->setInterval(milliseconds);
}

void ProjectsController::setSaveRefreshDelay(int milliseconds)
{
    m_saveRefresh->setInterval(milliseconds);
}

void ProjectsController::setAccount(Account account)
{
    m_account = account;
    updateChoices();
    Q_EMIT changed();
}

void ProjectsController::updateChoices()
{
    m_model->setChoicesAllowed(canChooseFolders());
}

void ProjectsController::documentModified(const QString &absolutePath)
{
    m_unsavedDocuments.insert(ProjectsDocumentHost::pathKey(absolutePath));
    m_presence->documentModified(absolutePath);
    Q_EMIT changed();
}

void ProjectsController::documentClosed(const QString &absolutePath)
{
    m_presence->documentClosed(absolutePath, m_unsavedDocuments.remove(ProjectsDocumentHost::pathKey(absolutePath)));
    Q_EMIT changed();
}

void ProjectsController::documentActivated(const QString &absolutePath)
{
    m_presence->documentActivated(absolutePath);
}

void ProjectsController::documentSaved(const QString &absolutePath)
{
    m_unsavedDocuments.remove(ProjectsDocumentHost::pathKey(absolutePath));
    Q_EMIT changed();
    const QStringList projects = m_model->downloadedProjectPaths();
    for (const QString &project : projects) {
        if (ProjectsDocumentHost::isInside(absolutePath, project)) {
            m_saveRefresh->start();
            return;
        }
    }
}

void ProjectsController::documentsChanged()
{
    Q_EMIT changed();
}

void ProjectsController::connectToApplication()
{
    KisPart *part = KisPart::instance();
    auto watch = [this](KisDocument *document) {
        connect(document, &KisDocument::modified, this, [this, document](bool modified) {
            if (modified) {
                documentModified(document->path());
            } else {
                m_unsavedDocuments.remove(ProjectsDocumentHost::pathKey(document->path()));
                documentsChanged();
            }
        }, Qt::UniqueConnection);
    };
    connect(part, &KisPart::sigDocumentAdded, this, [this, watch](KisDocument *document) {
        watch(document);
        documentsChanged();
    });
    connect(part, &KisPart::sigDocumentRemoved, this, &ProjectsController::documentClosed);
    // Opening a file that someone else is editing: say so. Nobody is prevented from editing.
    connect(part, &KisPart::sigViewAdded, this, [this](KisView *view) {
        if (view && view->document()) {
            documentActivated(view->document()->path());
        }
    });
    connect(part, &KisPart::sigDocumentSaved, this, &ProjectsController::documentSaved);
    connect(this, &ProjectsController::someoneElseIsEditing, this, [](const QString &, const QString &user) {
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
        if (window && window->viewManager()) {
            window->viewManager()->showFloatingMessage(i18n("%1 is editing this file", user), QIcon());
        }
    });
    const QList<QPointer<KisDocument>> documents = part->documents();
    for (const QPointer<KisDocument> &document : documents) {
        if (document) {
            watch(document);
        }
    }
}
