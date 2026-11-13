/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGDRIVER_H
#define KISVERSIONINGDRIVER_H

#include <QObject>
#include <QString>
#include <QStringList>

#include "KisVersioningTypes.h"
#include "KisVersioningJob.h"
#include "kritaversioning_export.h"

/**
 * What a versioning backend must provide. Nothing here blocks: every call
 * returns a job at once and the job reports the outcome later. An operation
 * the driver does not support ends with fail(Failed, ...).
 *
 * The comment on each call gives the type of KisVersioningJob::result<T>().
 *
 * When synchronise() reports a divergence on a path, the working file at that
 * path still holds the caller's own content; resolve(..., Theirs, ...) swaps
 * it for the server's. fileHistory() returns versions from oldest to newest.
 *
 * The panels show the text a driver gives its jobs (the error text of fail()
 * and the label of reportProgress()) to the artist as it is. That text must be
 * translated, must not carry messages of the backend, and must not use
 * version control vocabulary. Raw backend text goes to the log.
 */
class KRITAVERSIONING_EXPORT KisVersioningDriver : public QObject
{
    Q_OBJECT
public:
    explicit KisVersioningDriver(QObject *parent = nullptr);
    ~KisVersioningDriver() override;

    virtual QString id() const = 0;
    virtual KisVersioning::Capabilities capabilities() const = 0;
    virtual QString currentUser() const = 0;

    virtual KisVersioningJob *signIn(const QString &serverAddress) = 0;
    virtual KisVersioningJob *signOut() = 0;
    virtual KisVersioningJob *listProjects(const QString &serverAddress) = 0;             // QVector<ProjectInfo>
    virtual KisVersioningJob *listFolders(const KisVersioning::ProjectInfo &project) = 0; // QStringList
    virtual KisVersioningJob *downloadProject(const KisVersioning::ProjectInfo &project,
                                              const QString &localPath, const QStringList &folders) = 0;
    virtual KisVersioningJob *setFolders(const QString &localPath, const QStringList &folders) = 0;
    virtual KisVersioningJob *status(const QString &localPath) = 0;                       // ProjectStatus
    virtual KisVersioningJob *synchronise(const QString &localPath, const QString &notes) = 0; // SyncResult
    virtual KisVersioningJob *resolve(const QString &localPath, const QString &path,
                                      KisVersioning::Resolution resolution, const QString &mergedFile) = 0;
    virtual KisVersioningJob *fileHistory(const QString &localPath, const QString &path,
                                          int offset, int limit) = 0;                     // QVector<VersionInfo>
    virtual KisVersioningJob *fetchFileAtVersion(const QString &localPath, const QString &path,
                                                 const QString &versionId, const QString &outputPath) = 0;
    virtual KisVersioningJob *announceEditing(const QString &localPath, const QString &path, bool editing) = 0;
    virtual KisVersioningJob *whoIsEditing(const QString &localPath, const QStringList &paths) = 0; // QVector<EditingInfo>
};

/// Creates drivers of one kind; plugins register one of these in the registry.
class KRITAVERSIONING_EXPORT KisVersioningDriverFactory
{
public:
    virtual ~KisVersioningDriverFactory();
    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual KisVersioningDriver *create(QObject *parent) const = 0;
};

#endif // KISVERSIONINGDRIVER_H
