/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsSyncRunner.h"

#include <QMetaObject>

ProjectsSyncRunner::ProjectsSyncRunner(KisVersioningDriver *driver, KisVersioningDocumentHost *host, QObject *parent)
    : QObject(parent)
    , m_driver(driver)
    , m_host(host)
{
}

ProjectsSyncRunner::~ProjectsSyncRunner() = default;

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
    auto *session = new KisVersioningSyncSession(m_driver, m_host, this);
    connect(session, &KisVersioningSyncSession::progress, this,
            [this, index](qint64 done, qint64 total, const QString &label) {
                Q_EMIT progress(index, m_paths.size(), done, total, label);
            });
    // The session emits finished() once, after its work is over: the next project starts then.
    connect(session, &KisVersioningSyncSession::finished, this,
            [this, session, index, path](const KisVersioningSyncSession::Report &report) {
                session->deleteLater();
                m_results.append({path, report});
                Q_EMIT projectFinished(index);
                startNext();
            });
    Q_EMIT progress(index, m_paths.size(), 0, 0, QString());
    session->start(path, m_notes);
}
