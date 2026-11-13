/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LOREDRIVER_H
#define LOREDRIVER_H

#include <memory>

#include <KisVersioningDriver.h>

struct LoreMemory;
struct LoreSettings;

/**
 * Keeps projects on a Lore server, through the library's C API. A project is a Lore repository;
 * a downloaded project is a repository instance, always cloned with the local cache so that a
 * version can be created with no network.
 *
 * Every operation runs the library's asynchronous calls one after the other from the thread of
 * the driver and ends its job there. Jobs are safe when the driver is destroyed first. A
 * cancellation is honoured between two calls; a call in flight runs to its end.
 *
 * Geppetto stores no tokens: the session stays with the library. Against a server with no
 * authentication, signIn() succeeds and the operating system user name is the identity.
 */
class LoreDriver : public KisVersioningDriver
{
    Q_OBJECT
public:
    explicit LoreDriver(QObject *parent = nullptr);
    ~LoreDriver() override;

    QString id() const override;
    KisVersioning::Capabilities capabilities() const override;
    QString currentUser() const override;

    KisVersioningJob *signIn(const QString &serverAddress) override;
    KisVersioningJob *signOut() override;
    KisVersioningJob *listProjects(const QString &serverAddress) override;
    KisVersioningJob *listFolders(const KisVersioning::ProjectInfo &project) override;
    KisVersioningJob *downloadProject(const KisVersioning::ProjectInfo &project,
                                      const QString &localPath, const QStringList &folders) override;
    /// An empty list means the whole project.
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

    /// Tests only: every call runs with the library's offline option, as with no network.
    void setOfflineForTesting(bool offline);
    /// Tests only: the identity sent with every call and reported by currentUser(), so that a
    /// test can act as two people.
    void setIdentityForTesting(const QString &identity);

private:
    LoreSettings settings() const;

    QString m_user;              ///< who signed in during this session of the application
    QString m_identity;          ///< sent with every call; empty lets the library use its session
    QString m_testIdentity;
    bool m_offline {false};
    std::shared_ptr<LoreMemory> m_memory;
};

#endif // LOREDRIVER_H
