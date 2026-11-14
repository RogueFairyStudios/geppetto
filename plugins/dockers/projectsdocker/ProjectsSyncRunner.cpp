/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsSyncRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSet>

#include <klocalizedstring.h>

#include "ProjectsDocuments.h"

namespace {

/// How two absolute paths are compared: the file systems of Windows and macOS ignore case.
QString comparable(const QString &path)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return clean.toLower();
#else
    return clean;
#endif
}

} // namespace

ProjectsSyncRunner::ProjectsSyncRunner(KisVersioningDriver *driver, ProjectsDocuments *host, QObject *parent)
    : QObject(parent)
    , m_driver(driver)
    , m_host(host)
{
}

ProjectsSyncRunner::~ProjectsSyncRunner() = default;

QStringList ProjectsSyncRunner::staleDocuments(const QString &projectPath,
                                               const KisVersioningSyncSession::Report &report,
                                               const KisVersioningDocumentHost &host)
{
    QSet<QString> modified;
    const QStringList open = host.modifiedOpenDocuments(projectPath);
    for (const QString &path : open) {
        modified.insert(comparable(path));
    }
    // The session already knows those whose newest work it could not keep in a file.
    QStringList stale = report.staleDocuments;
    const QDir project(projectPath);
    for (const QString &path : report.received + report.keptBoth) {
        if (!stale.contains(path) && modified.contains(comparable(project.filePath(path)))) {
            stale << path;
        }
    }
    return stale;
}

void ProjectsSyncRunner::start(const QStringList &projectPaths, const QString &notes)
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_paths = projectPaths;
    m_notes = notes;
    m_results.clear();
    // The first session starts on the next turn of the event loop, so that finished() is never
    // emitted from inside start(), not even with no project.
    QMetaObject::invokeMethod(this, &ProjectsSyncRunner::startNext, Qt::QueuedConnection);
}

bool ProjectsSyncRunner::isRunning() const
{
    return m_running;
}

QVector<ProjectsSyncRunner::Result> ProjectsSyncRunner::results() const
{
    return m_results;
}

void ProjectsSyncRunner::startNext()
{
    if (m_results.size() >= m_paths.size()) {
        m_running = false;
        Q_EMIT finished();
        return;
    }

    const int index = m_results.size();
    const QString path = m_paths.at(index);

    const QStringList foreign = m_host->unsavedForeignDocuments(path);
    if (!foreign.isEmpty()) {
        QStringList texts;
        for (const QString &document : foreign) {
            texts << i18n("%1 has unsaved changes and is not a Krita file. Save it yourself, then synchronize again.",
                          QFileInfo(document).fileName());
        }
        KisVersioningSyncSession::Report report;
        report.error = KisVersioningJob::Failed;
        report.errorText = texts.join(QLatin1Char('\n'));
        m_results.append({path, report, QStringList()});
        Q_EMIT projectFinished(index);
        // On the next turn of the event loop, as when a session ends.
        QMetaObject::invokeMethod(this, &ProjectsSyncRunner::startNext, Qt::QueuedConnection);
        return;
    }

    auto *session = new KisVersioningSyncSession(m_driver, m_host, this);
    connect(session, &KisVersioningSyncSession::progress, this,
            [this, index](qint64 done, qint64 total, const QString &label) {
                Q_EMIT progress(index, m_paths.size(), done, total, label);
            });
    // The session emits finished() once, after its work is over: the next project starts then.
    // The open documents are looked at now, as the session ends.
    connect(session, &KisVersioningSyncSession::finished, this,
            [this, session, index, path](const KisVersioningSyncSession::Report &report) {
                session->deleteLater();
                m_results.append({path, report, staleDocuments(path, report, *m_host)});
                Q_EMIT projectFinished(index);
                startNext();
            });
    Q_EMIT progress(index, m_paths.size(), 0, 0, QString());
    session->start(path, m_notes);
}
