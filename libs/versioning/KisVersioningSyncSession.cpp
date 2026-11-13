/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningSyncSession.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>

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

bool samePath(const QString &a, const QString &b)
{
#ifdef Q_OS_LINUX
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#endif
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), sensitivity) == 0;
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
    m_savedDocuments.clear();

    // What the artist is editing goes to disk first: the driver only sees files.
    const QStringList modified = m_host->modifiedOpenDocuments(projectPath);
    for (const QString &document : modified) {
        if (!m_host->saveDocument(document)) {
            endWithError(KisVersioningJob::Failed,
                         i18n("Could not save %1 before synchronising.", QDir::toNativeSeparators(document)));
            return;
        }
        m_savedDocuments << document;
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
    case DivergenceKind::BothModified: {
        // My content goes into a copy before the driver replaces the working file with theirs.
        QString copyPath;
        QString errorText;
        if (!keepBoth(divergence.path, &copyPath, &errorText)) {
            endWithError(KisVersioningJob::Failed, errorText);
            return;
        }
        appendUnique(m_report.keptBoth, {divergence.path});
        appendUnique(m_report.conflictCopies, {copyPath});
        runResolve(divergence.path, Resolution::Theirs);
        break;
    }
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

bool KisVersioningSyncSession::keepBoth(const QString &path, QString *copyPath, QString *errorText)
{
    const QDir root(m_projectPath);
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const QString folder = slash >= 0 ? path.left(slash + 1) : QString();
    const QString fileName = path.mid(slash + 1);
    const QDir directory(root.filePath(folder));
    const QString source = root.filePath(path);

    const QString failure = i18n("Could not keep a copy of %1, so it was left as it is.", fileName);

    // Everything in the folder counts as taken, folders and hidden files included.
    QStringList taken = directory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    const QString user = m_driver->currentUser();

    for (int tries = 0; tries < 1000; ++tries) {
        const QString name = conflictCopyName(fileName, user, taken);
        const QString destination = directory.filePath(name);
        // QFile::copy never overwrites: it fails when the destination exists.
        if (QFile::copy(source, destination)) {
            if (QFileInfo(destination).size() != QFileInfo(source).size()) {
                QFile::remove(destination);
                break;
            }

            KisVersioningConflictMarkers::Marker marker;
            marker.copyPath = folder + name;
            marker.originalPath = path;
            marker.author = user;
            marker.date = QDateTime::currentDateTime();
            // Without a marker the copy still holds my content, so the work goes on.
            KisVersioningConflictMarkers(m_projectPath).add(marker);

            *copyPath = marker.copyPath;
            return true;
        }
        if (!QFileInfo::exists(destination)) {
            break;
        }
        // Someone created that name since we looked: take the next one.
        taken << name;
    }

    *errorText = failure;
    return false;
}

bool KisVersioningSyncSession::wasSavedHere(const QString &absolutePath) const
{
    for (const QString &saved : m_savedDocuments) {
        if (samePath(saved, absolutePath)) {
            return true;
        }
    }
    return false;
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
    // What the session saved itself is left alone: its content was just sent or kept.
    const QDir root(m_projectPath);
    for (const QString &path : std::as_const(m_report.received)) {
        const QString absolute = root.filePath(path);
        if (!wasSavedHere(absolute) && m_host->isOpenAndUnmodified(absolute)) {
            m_host->reloadDocument(absolute);
        }
    }

    // Never from inside start(): the caller connects after calling it, or before, and both work.
    const Report report = m_report;
    m_running = false;
    QMetaObject::invokeMethod(this, [this, report]() {
        Q_EMIT finished(report);
    }, Qt::QueuedConnection);
}
