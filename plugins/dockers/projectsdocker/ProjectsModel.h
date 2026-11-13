/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSMODEL_H
#define PROJECTSMODEL_H

#include <functional>
#include <memory>
#include <vector>

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <KisVersioningConflictMarkers.h>
#include <KisVersioningDriver.h>
#include <KisVersioningSettings.h>
#include <KisVersioningTypes.h>

/**
 * The projects of the panel's driver, as a tree: each project, then its
 * folders and files as they are on disk, with the state of each file.
 *
 * Column 0 holds the name; column 1 the notice shown beside it ("changed",
 * "Bruno editing", "conflict to resolve", or an error for a project).
 *
 * The downloaded projects come from the settings. With a driver that has
 * RemoteProjects, the projects on the server join them, and the folders the
 * driver lists can be ticked: ticking downloads the folder, unticking removes
 * it from the choice. A project takes one choice at a time; a choice made
 * while the project is busy waits until the list shows the previous one.
 * Files never synchronised and hidden folders (where the drivers keep their
 * own data) are not shown.
 *
 * Every refresh reads the disk on a worker thread and asks the driver without
 * waiting; the model changes in one step when all the answers are in. It is
 * safe to destroy the model while that is going on.
 */
class ProjectsModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Roles { PathRole = Qt::UserRole + 1, StateRole, EditingUserRole, ConflictCopyRole, CheckedFolderRole };

    ProjectsModel(KisVersioningDriver *driver, KisVersioningSettings *settings, QObject *parent = nullptr);
    ~ProjectsModel() override;

    void refresh();                                  // status + whoIsEditing of each project
    QString summaryText() const;                     // "1 file changed · 2 updates"
    int pendingUploadCount() const;

    /// The local folders of the driver's downloaded projects, from the settings.
    QStringList downloadedProjectPaths() const;
    /// True when the last refresh found that the driver needs the artist to sign in.
    bool signInNeeded() const;
    /// For a driver without RemoteProjects: adds a folder on this computer as a project.
    void addFolder(const QString &path);
    /// Removes the "conflict to resolve" marker of the copy at @p index. No file is deleted.
    void markConflictResolved(const QModelIndex &index);

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

Q_SIGNALS:
    void refreshed();
    /// Something the artist asked for could not be done; @p text says why.
    void operationFailed(const QString &text);

private:
    struct Node;
    struct Project;
    struct Refresh;

    void readFolder(const std::shared_ptr<Refresh> &refresh, int project);
    void askStatus(const std::shared_ptr<Refresh> &refresh, int project);
    void askWhoIsEditing(const std::shared_ptr<Refresh> &refresh, int project);
    void askRemoteProjects(const std::shared_ptr<Refresh> &refresh);
    void askFolders(const std::shared_ptr<Refresh> &refresh, int project);
    void stepDone(const std::shared_ptr<Refresh> &refresh);
    void apply(const Refresh &refresh);
    std::unique_ptr<Node> buildProject(int project) const;

    /// A tick or an untick of a folder, as the artist made it.
    struct Choice {
        KisVersioning::ProjectInfo info;
        QString folder;
        bool chosen = false;
    };

    Node *nodeAt(const QModelIndex &index) const;
    QString noticeFor(const Node *node) const;
    QString absolutePath(const Node *node) const;
    bool chooseFolder(Node *folder, bool chosen);
    bool startChoice(const Choice &choice);
    void untick(const Choice &choice, const QString &localPath);
    void startWaitingChoice(const QString &projectId);
    bool storedProject(const QString &projectId, KisVersioningSettings::Project *project) const;
    QString freeFolderFor(const KisVersioning::ProjectInfo &info) const;
    void runProjectJob(const QString &projectId, KisVersioningJob *job, const std::function<void()> &onSuccess);
    void releaseAfterRefresh(const QString &projectId);

    KisVersioningDriver *m_driver;
    KisVersioningSettings *m_settings;
    std::shared_ptr<Refresh> m_refresh;
    std::vector<Project> m_projects;
    std::unique_ptr<Node> m_root;
    bool m_signInNeeded {false};
    int m_changedFiles {0};
    int m_incoming {0};
    int m_pendingUpload {0};
    /// Projects with an operation under way, or whose operation the list does not show yet.
    QSet<QString> m_busyProjects;
    QHash<QString, QList<Choice>> m_waitingChoices;   ///< choices made while their project was busy
    QHash<QString, QString> m_downloadTargets;        ///< project id to the folder a download is filling
};

#endif // PROJECTSMODEL_H
