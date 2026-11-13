/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningJob.h"

#include <QMetaObject>
#include <QMutexLocker>

KisVersioningJob::KisVersioningJob(QObject *parent)
    : QObject(parent)
{
}

KisVersioningJob::~KisVersioningJob() = default;

bool KisVersioningJob::isFinished() const
{
    QMutexLocker locker(&m_mutex);
    return m_finished;
}

KisVersioningJob::Error KisVersioningJob::error() const
{
    QMutexLocker locker(&m_mutex);
    return m_error;
}

QString KisVersioningJob::errorText() const
{
    QMutexLocker locker(&m_mutex);
    return m_errorText;
}

QVariant KisVersioningJob::resultVariant() const
{
    QMutexLocker locker(&m_mutex);
    return m_result;
}

void KisVersioningJob::requestCancel()
{
    QMutexLocker locker(&m_mutex);
    m_cancelRequested = true;
}

bool KisVersioningJob::cancelRequested() const
{
    QMutexLocker locker(&m_mutex);
    return m_cancelRequested;
}

void KisVersioningJob::reportProgress(qint64 done, qint64 total, const QString &label)
{
    QMetaObject::invokeMethod(this, [this, done, total, label]() {
        Q_EMIT progress(done, total, label);
    }, Qt::QueuedConnection);
}

void KisVersioningJob::finish(const QVariant &result)
{
    complete(NoError, QString(), result);
}

void KisVersioningJob::fail(Error error, const QString &text)
{
    complete(error, text, QVariant());
}

void KisVersioningJob::complete(Error error, const QString &text, const QVariant &result)
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_error = error;
        m_errorText = text;
        m_result = result;
    }

    // Always queued, so the signal arrives on the job's thread and only after
    // the call that produced the job has returned.
    QMetaObject::invokeMethod(this, [this]() {
        Q_EMIT finished(this);
        deleteLater();
    }, Qt::QueuedConnection);
}
