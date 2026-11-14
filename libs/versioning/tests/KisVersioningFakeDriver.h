/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGFAKEDRIVER_H
#define KISVERSIONINGFAKEDRIVER_H

#include <functional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
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
    /// Called every time currentUser() is asked; the session asks after it has listed a folder, so a
    /// hook can create a file that the listing did not see.
    void setCurrentUserHook(const std::function<void()> &hook);
    /// When set, every call also appends "driver:<name> <args>" to @p log, so a test can order driver and host calls.
    void setSharedLog(QStringList *log);
    void setSignInError(KisVersioningJob::Error error, const QString &text = QString());
    /// The label a sign-in reports while it runs, as a driver shows the code the artist must confirm.
    void setSignInProgress(const QString &label);
    /// What resumeSession() finds: by default whoever currentUser() gives. A successful one makes
    /// @p user the current user.
    void setResumedUser(const QString &user);
    void setResumeSessionError(KisVersioningJob::Error error, const QString &text = QString());
    /// What listProjects() returns; by default no project.
    void setProjects(const QVector<KisVersioning::ProjectInfo> &projects);
    void setListProjectsError(KisVersioningJob::Error error, const QString &text = QString());
    /// What listFolders() returns for the project with @p projectId; by default no folder.
    void setProjectFolders(const QString &projectId, const QStringList &folders);
    /// A successful downloadProject() creates the local folder and the chosen folders in it, and ends
    /// with the folders of the project (setProjectFolders) it left out, as a real driver does.
    void setDownloadError(KisVersioningJob::Error error, const QString &text = QString());
    /// The project downloaded at @p localPath, so that setFolders() knows which folders it leaves out.
    /// A successful downloadProject() records it too.
    void setLocalProject(const QString &localPath, const QString &projectId);
    void setSetFoldersError(KisVersioningJob::Error error, const QString &text = QString());
    /// While held, the jobs of the operation named @p name (for example "synchronise") do not end;
    /// releaseHeld() ends them with the outcome they would have had.
    void setHeld(const QString &name, bool held);
    /// Ends the held jobs of @p name, in the order they were made; returns how many ended.
    int releaseHeld(const QString &name);
    int heldCount(const QString &name) const;

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
    KisVersioningJob *resumeSession(const QString &serverAddress) override;
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
    KisVersioningJob *makeJob(const QString &name, const Outcome &outcome);
    static Outcome failure(KisVersioningJob::Error error, const QString &text);

    QString m_user;
    KisVersioning::Capabilities m_capabilities;
    QList<Outcome> m_syncQueue;
    Outcome m_status;
    Outcome m_editing;
    Outcome m_announce;
    Outcome m_resolve;
    Outcome m_signIn;
    QString m_signInProgress;
    Outcome m_resume;
    bool m_resumeScripted {false};
    QString m_resumedUser;
    Outcome m_listProjects;
    Outcome m_download;
    Outcome m_setFolders;
    QHash<QString, QStringList> m_projectFolders;
    QHash<QString, QString> m_localProjects;   ///< local path to project id
    QHash<QString, QByteArray> m_theirContent;
    std::function<void()> m_userHook;
    QStringList *m_sharedLog {nullptr};
    QList<Call> m_calls;
    struct Held {
        QPointer<KisVersioningJob> job;
        Outcome outcome;
    };
    QSet<QString> m_holding;
    QHash<QString, QList<Held>> m_held;
};

#endif // KISVERSIONINGFAKEDRIVER_H
