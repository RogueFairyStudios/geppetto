/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGFAKEDRIVER_H
#define KISVERSIONINGFAKEDRIVER_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QVector>

#include <KisVersioningDriver.h>

/**
 * An in-memory driver for tests. The test scripts what every operation
 * returns; the driver records every call it receives, in order.
 *
 * Honouring the driver contract, resolve(path, Theirs) writes the scripted
 * "their" bytes into the working file, and resolve(path, MergedFile) copies
 * the merged file over it.
 */
class KisVersioningFakeDriver : public KisVersioningDriver
{
    Q_OBJECT
public:
    struct Call {
        QString name;        ///< the method name, for example "synchronise"
        QStringList args;    ///< the arguments as text; a Resolution is "Mine", "Theirs" or "MergedFile"
    };

    explicit KisVersioningFakeDriver(QObject *parent = nullptr);

    // Scripting
    void setCurrentUser(const QString &user);
    void setCapabilities(KisVersioning::Capabilities capabilities);
    /// Queue what the next synchronise() call returns. When the queue is empty the call returns an empty SyncResult.
    void queueSyncResult(const KisVersioning::SyncResult &result);
    void queueSyncError(KisVersioningJob::Error error, const QString &text = QString());
    void setStatus(const KisVersioning::ProjectStatus &status);
    void setStatusError(KisVersioningJob::Error error, const QString &text = QString());
    void setEditing(const QVector<KisVersioning::EditingInfo> &editing);
    void setWhoIsEditingError(KisVersioningJob::Error error, const QString &text = QString());
    void setAnnounceOutcome(KisVersioningJob::Error error, const QString &text = QString());
    void setResolveError(KisVersioningJob::Error error, const QString &text = QString());
    /// The bytes resolve(path, Theirs) puts into the working file at @p path.
    void setTheirContent(const QString &path, const QByteArray &content);
    /// When set, every call also appends "driver:<name> <args>" to @p log, so a test can order driver and host calls.
    void setSharedLog(QStringList *log);

    // Recording
    const QList<Call> &calls() const;
    int callCount(const QString &name) const;
    QList<Call> callsNamed(const QString &name) const;

    // KisVersioningDriver
    QString id() const override;
    KisVersioning::Capabilities capabilities() const override;
    QString currentUser() const override;

    KisVersioningJob *signIn(const QString &serverAddress) override;
    KisVersioningJob *signOut() override;
    KisVersioningJob *listProjects(const QString &serverAddress) override;
    KisVersioningJob *listFolders(const KisVersioning::ProjectInfo &project) override;
    KisVersioningJob *downloadProject(const KisVersioning::ProjectInfo &project,
                                      const QString &localPath, const QStringList &folders) override;
    KisVersioningJob *setFolders(const QString &localPath, const QStringList &folders) override;
    KisVersioningJob *status(const QString &localPath) override;
    KisVersioningJob *synchronise(const QString &localPath, const QString &notes) override;
    KisVersioningJob *resolve(const QString &localPath, const QString &path,
                              KisVersioning::Resolution resolution, const QString &mergedFile) override;
    KisVersioningJob *fileHistory(const QString &localPath, const QString &path, int offset, int limit) override;
    KisVersioningJob *fetchFileAtVersion(const QString &localPath, const QString &path,
                                         const QString &versionId, const QString &outputPath) override;
    KisVersioningJob *announceEditing(const QString &localPath, const QString &path, bool editing) override;
    KisVersioningJob *whoIsEditing(const QString &localPath, const QStringList &paths) override;

private:
    struct Outcome {
        KisVersioningJob::Error error = KisVersioningJob::NoError;
        QString text;
        QVariant result;
    };

    void record(const QString &name, const QStringList &args);
    KisVersioningJob *makeJob(const Outcome &outcome);
    static Outcome failure(KisVersioningJob::Error error, const QString &text);

    QString m_user;
    KisVersioning::Capabilities m_capabilities;
    QList<Outcome> m_syncQueue;
    Outcome m_status;
    Outcome m_editing;
    Outcome m_announce;
    Outcome m_resolve;
    QHash<QString, QByteArray> m_theirContent;
    QStringList *m_sharedLog {nullptr};
    QList<Call> m_calls;
};

#endif // KISVERSIONINGFAKEDRIVER_H
