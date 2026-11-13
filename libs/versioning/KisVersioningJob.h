/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGJOB_H
#define KISVERSIONINGJOB_H

#include <QMutex>
#include <QObject>
#include <QString>
#include <QVariant>

#include "kritaversioning_export.h"

/**
 * The handle of one asynchronous versioning operation.
 *
 * The driver does the work and calls finish() or fail(), from any thread.
 * The finished() signal is then delivered on the job's own thread, never
 * before the call that created the job has returned, and exactly once. After
 * delivering it the job deletes itself.
 */
class KRITAVERSIONING_EXPORT KisVersioningJob : public QObject
{
    Q_OBJECT
public:
    enum Error { NoError, Cancelled, NotSignedIn, Offline, Failed };
    Q_ENUM(Error)

    explicit KisVersioningJob(QObject *parent = nullptr);
    ~KisVersioningJob() override;

    bool isFinished() const;
    Error error() const;
    QString errorText() const;

    template <typename T>
    T result() const
    {
        return resultVariant().value<T>();
    }

    /// Ask the driver to stop. It is a request only: the job still ends with finish() or fail().
    void requestCancel();
    bool cancelRequested() const;

    /// For drivers. Emits progress() on the job's thread.
    ///
    /// The panels show @p label to the artist as it is: it must be translated, must not carry
    /// messages of the backend, and must not use version control vocabulary. Raw backend text
    /// goes to the log.
    void reportProgress(qint64 done, qint64 total, const QString &label);
    /// For drivers. Ends the job successfully; ignored if the job already ended.
    void finish(const QVariant &result = QVariant());
    /// For drivers. Ends the job with an error; ignored if the job already ended.
    ///
    /// The panels show @p text to the artist as it is: it must be translated, must not carry
    /// messages of the backend, and must not use version control vocabulary. Raw backend text
    /// goes to the log.
    void fail(Error error, const QString &text);

Q_SIGNALS:
    void progress(qint64 done, qint64 total, const QString &label);
    void finished(KisVersioningJob *job);

private:
    QVariant resultVariant() const;
    void complete(Error error, const QString &text, const QVariant &result);

    mutable QMutex m_mutex;
    bool m_finished {false};
    bool m_cancelRequested {false};
    Error m_error {NoError};
    QString m_errorText;
    QVariant m_result;
};

#endif // KISVERSIONINGJOB_H
