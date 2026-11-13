/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningSyncSession.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QtConcurrent>

#include <utility>

#include <klocalizedstring.h>

#include "KisVersioningConflictMarkers.h"
#include "KisVersioningFileRules.h"

using namespace KisVersioning;

namespace {

void appendUnique(QStringList &list, const QStringList &items)
{
    for (const QString &item : items) {
        if (!list.contains(item)) {
            list << item;
        }
    }
}

struct CopyOutcome {
    bool ok = false;
    QString name;
};

// Runs on a worker thread and touches nothing but its arguments: the copy of a
// large art file must not block the interface. QFile::copy never overwrites, it
// fails when the destination exists; then the next name is tried.
CopyOutcome copyWithoutOverwriting(const QString &source, const QString &directoryPath, const QString &fileName,
                                   const QString &user, QStringList taken)
{
    const QDir directory(directoryPath);
    for (int tries = 0; tries < 1000; ++tries) {
        const QString name = conflictCopyName(fileName, user, taken);
        const QString destination = directory.filePath(name);
        if (QFile::copy(source, destination)) {
            if (QFileInfo(destination).size() != QFileInfo(source).size()) {
                QFile::remove(destination);
                return {};
            }
            return {true, name};
        }
        if (!QFileInfo::exists(destination)) {
            return {};
        }
        // Someone created that name since the folder was listed: take the next one.
        taken << name;
    }
    return {};
}

} // namespace

KisVersioningDocumentHost::~KisVersioningDocumentHost() = default;

KisVersioningSyncSession::KisVersioningSyncSession(KisVersioningDriver *driver, KisVersioningDocumentHost *host,
                                                   QObject *parent)
    : QObject(parent)
    , m_driver(driver)
    , m_host(host)
{
    qRegisterMetaType<KisVersioningSyncSession::Report>("KisVersioningSyncSession::Report");
}

KisVersioningSyncSession::~KisVersioningSyncSession() = default;

void KisVersioningSyncSession::start(const QString &projectPath, const QString &notes)
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_projectPath = projectPath;
    m_notes = notes;
    m_attempts = 0;
    m_report = Report();
    m_reloaded.clear();

    // What the artist is editing goes to disk first: the driver only sees files.
    const QStringList modified = m_host->modifiedOpenDocuments(projectPath);
    for (const QString &document : modified) {
        if (!m_host->saveDocument(document)) {
            endWithError(KisVersioningJob::Failed,
                         i18n("Could not save %1 before synchronising.", QDir::toNativeSeparators(document)));
            return;
        }
    }

    synchroniseOnce();
}

void KisVersioningSyncSession::watch(KisVersioningJob *job)
{
    connect(job, &KisVersioningJob::progress, this, &KisVersioningSyncSession::progress);
}

void KisVersioningSyncSession::synchroniseOnce()
{
    ++m_attempts;
    KisVersioningJob *job = m_driver->synchronise(m_projectPath, m_notes);
    watch(job);
    // The job deletes itself after finished(): everything is read in the slot.
    connect(job, &KisVersioningJob::finished, this, &KisVersioningSyncSession::onSynchronised);
}

void KisVersioningSyncSession::onSynchronised(KisVersioningJob *job)
{
    if (job->error() != KisVersioningJob::NoError) {
        endWithDriverError(job);
        return;
    }

    const SyncResult result = job->result<SyncResult>();
    appendUnique(m_report.sent, result.sent);
    appendUnique(m_report.received, result.received);
    // A newer version of a file already reloaded must be shown again.
    for (const QString &path : result.received) {
        m_reloaded.removeAll(path);
    }
    m_report.pendingUpload = result.pendingUpload;
    m_serverChanged = result.serverChanged;
    m_hadDivergence = !result.divergences.isEmpty();
    m_divergences = result.divergences;
    m_nextDivergence = 0;

    resolveNext();
}

void KisVersioningSyncSession::resolveNext()
{
    if (m_nextDivergence >= m_divergences.size()) {
        afterResolutions();
        return;
    }

    const Divergence divergence = m_divergences.at(m_nextDivergence++);
    switch (divergence.kind) {
    case DivergenceKind::BothModified:
        // My content goes into a copy before the driver replaces the working file with theirs;
        // the state machine goes on when the copy is done.
        startKeepBoth(divergence.path);
        break;
    case DivergenceKind::DeletedHereModifiedThere:
        runResolve(divergence.path, Resolution::Theirs);
        break;
    case DivergenceKind::ModifiedHereDeletedThere:
        runResolve(divergence.path, Resolution::Mine);
        break;
    }
}

void KisVersioningSyncSession::runResolve(const QString &path, Resolution resolution)
{
    m_resolvingPath = path;
    m_resolvingTheirs = (resolution == Resolution::Theirs);
    KisVersioningJob *job = m_driver->resolve(m_projectPath, path, resolution, QString());
    watch(job);
    connect(job, &KisVersioningJob::finished, this, &KisVersioningSyncSession::onResolved);
}

void KisVersioningSyncSession::onResolved(KisVersioningJob *job)
{
    if (job->error() != KisVersioningJob::NoError) {
        endWithDriverError(job);
        return;
    }
    if (m_resolvingTheirs) {
        // The file now holds the other version: an open, unchanged document must show it,
        // or the next save would put mine back over it. Mine is safe in the conflict copy.
        // The same path may have been reloaded for an earlier version: this one is newer.
        m_reloaded.removeAll(m_resolvingPath);
        reloadIfClean(m_resolvingPath);
    }
    resolveNext();
}

void KisVersioningSyncSession::afterResolutions()
{
    if (!m_hadDivergence && !m_serverChanged) {
        conclude();
        return;
    }
    if (m_attempts >= MaxAttempts) {
        // The server keeps changing under us: what is still local goes up next time.
        m_report.pendingUpload = true;
        conclude();
        return;
    }
    synchroniseOnce();
}

void KisVersioningSyncSession::startKeepBoth(const QString &path)
{
    const QDir root(m_projectPath);
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const QString folder = slash >= 0 ? path.left(slash + 1) : QString();
    const QString fileName = path.mid(slash + 1);
    const QString directoryPath = root.filePath(folder);

    // The artist can save this file while the copy runs. What the file was when we started is
    // compared with what it is when the copy ends: a different file means a copy of old content.
    const QFileInfo before(root.filePath(path));
    m_keepBothSize = before.size();
    m_keepBothModified = before.lastModified();

    // Everything in the folder counts as taken, folders and hidden files included.
    const QStringList taken = QDir(directoryPath).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    const QString user = m_driver->currentUser();
    const QString source = root.filePath(path);

    m_keepBothPath = path;
    m_keepBothFolder = folder;
    m_keepBothUser = user;

    auto *watcher = new QFutureWatcher<CopyOutcome>(this);
    connect(watcher, &QFutureWatcher<CopyOutcome>::finished, this, [this, watcher]() {
        const CopyOutcome outcome = watcher->result();
        watcher->deleteLater();
        onCopyFinished(outcome.ok, outcome.name);
    });
    // The task works on copies of its arguments, so it is harmless if the session is destroyed
    // first: the watcher goes with the session and nobody is told.
    watcher->setFuture(QtConcurrent::run(copyWithoutOverwriting, source, directoryPath, fileName, user, taken));
}

void KisVersioningSyncSession::onCopyFinished(bool ok, const QString &name)
{
    const QString path = m_keepBothPath;
    const QString fileName = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    if (ok) {
        const QFileInfo now(QDir(m_projectPath).filePath(path));
        if (!now.exists() || now.size() != m_keepBothSize || now.lastModified() != m_keepBothModified) {
            // The working file changed during the copy, so the copy may hold older content than
            // the file; replacing the file with the other version would lose the newer save.
            QFile::remove(QDir(m_projectPath).filePath(m_keepBothFolder + name));
            endWithError(KisVersioningJob::Failed,
                         i18n("%1 changed while it was being copied, so it was left as it is. Synchronise again.", fileName));
            return;
        }
    }
    if (!ok) {
        endWithError(KisVersioningJob::Failed,
                     i18n("Could not keep a copy of %1, so it was left as it is.", fileName));
        return;
    }

    KisVersioningConflictMarkers::Marker marker;
    marker.copyPath = m_keepBothFolder + name;
    marker.originalPath = path;
    marker.author = m_keepBothUser;
    marker.date = QDateTime::currentDateTime();
    if (!KisVersioningConflictMarkers(m_projectPath).add(marker)) {
        // Without the marker nothing would show the artist that their work sits in the copy:
        // take the copy back and leave the file as it is.
        QFile::remove(QDir(m_projectPath).filePath(marker.copyPath));
        endWithError(KisVersioningJob::Failed,
                     i18n("Could not mark the copy of %1 as a conflict to resolve, so it was left as it is.", fileName));
        return;
    }

    appendUnique(m_report.keptBoth, {path});
    appendUnique(m_report.conflictCopies, {marker.copyPath});
    runResolve(path, Resolution::Theirs);
}

void KisVersioningSyncSession::reloadIfClean(const QString &path)
{
    // Asked now, not remembered from the start: the artist may have kept working meanwhile.
    const QString absolute = QDir(m_projectPath).filePath(path);
    if (!m_reloaded.contains(path) && m_host->isOpenAndUnmodified(absolute)) {
        m_reloaded << path;
        m_host->reloadDocument(absolute);
    }
}

void KisVersioningSyncSession::endWithDriverError(KisVersioningJob *job)
{
    const KisVersioningJob::Error error = job->error();
    const QString text = job->errorText();
    if (error == KisVersioningJob::Offline || error == KisVersioningJob::NotSignedIn) {
        m_report.pendingUpload = true;
    }
    endWithError(error, text);
}

void KisVersioningSyncSession::endWithError(KisVersioningJob::Error error, const QString &text)
{
    m_report.error = error;
    m_report.errorText = text;
    conclude();
}

void KisVersioningSyncSession::conclude()
{
    // Files that arrived from the server and are open without changes are shown again.
    for (const QString &path : std::as_const(m_report.received)) {
        reloadIfClean(path);
    }

    // Never from inside start(): the caller connects after calling it, or before, and both work.
    const Report report = m_report;
    m_running = false;
    QMetaObject::invokeMethod(this, [this, report]() {
        Q_EMIT finished(report);
    }, Qt::QueuedConnection);
}
