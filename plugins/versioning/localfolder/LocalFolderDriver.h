/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LOCALFOLDERDRIVER_H
#define LOCALFOLDERDRIVER_H

#include <KisVersioningDriver.h>

/**
 * Keeps the versions of a project in a folder, with no server. A project is an
 * existing folder; its versions live in ".geppetto/versions/": an index with
 * the author, the date (UTC) and the notes of each version, and, in one folder
 * per version, copies of the files that version changed.
 *
 * The work on the disk runs on a worker thread, and a job stays safe when the
 * driver is destroyed before it ends. Operations that only make sense with a
 * server end with fail(Failed, ...).
 */
class LocalFolderDriver : public KisVersioningDriver
{
    Q_OBJECT
public:
    explicit LocalFolderDriver(QObject *parent = nullptr);
    ~LocalFolderDriver() override;

    /// How long synchronise() waits for another process writing the same store before it fails.
    static void setLockTimeout(int milliseconds);

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
    /// A limit of zero or less means no limit.
    KisVersioningJob *fileHistory(const QString &localPath, const QString &path, int offset, int limit) override;
    KisVersioningJob *fetchFileAtVersion(const QString &localPath, const QString &path,
                                         const QString &versionId, const QString &outputPath) override;
    KisVersioningJob *announceEditing(const QString &localPath, const QString &path, bool editing) override;
    KisVersioningJob *whoIsEditing(const QString &localPath, const QStringList &paths) override;
};

#endif // LOCALFOLDERDRIVER_H
