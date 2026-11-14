/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSMODEL_H
#define PROJECTSMODEL_H

#include <functional>
#include <memory>
#include <vector>

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <KisVersioningConflictMarkers.h>
#include <KisVersioningDriver.h>
#include <KisVersioningSettings.h>
#include <KisVersioningTypes.h>

class ProjectsDocuments;

/**
 * The projects of the panel's driver, as a tree: each project, then its
 * folders and files as they are on disk, with the state of each file.
 *
 * Column 0 holds the name; column 1 the notice shown beside it ("changed",
 * "Bruno editing", "conflict to resolve", "not sent: outside your folders",
 * or an error for a project).
 *
 * The downloaded projects come from the settings. With a driver that has
 * RemoteProjects, the projects on the server join them, and the folders the
 * driver lists can be ticked: ticking downloads the folder, unticking removes
 * it from the choice. A folder is shown ticked unless the driver left it out
 * of this computer. A file on disk inside a folder the driver left out is never
 * sent: it is shown as such and counted in the summary. A folder cannot be
 * unticked while one of its documents is open, nor when it is the last one
 * ticked. A project takes one choice at a time; a choice made while the
 * project is busy waits until the list shows the previous one. A choice shows
 * in its box at once, and the project shows that it is busy, with the
 * progress of the operation, until the list shows what it did.
 * Files never synchronised and hidden folders (where the drivers keep their
 * own data) are not shown.
 *
 * Every refresh reads the disk on a worker thread and asks the driver without
 * waiting; the model changes in one step when all the answers are in. It is
 * safe to destroy the model while that is going on. The folders of a
 * downloaded project are listed again only when its version on the server
 * changed; those of a project not downloaded, at most every few minutes.
 */
class ProjectsModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Roles { PathRole = Qt::UserRole + 1, StateRole, EditingUserRole, ConflictCopyRole, CheckedFolderRole, OutsideRole };

    /// @p documents (which may be null) tells which documents are open.
    ProjectsModel(KisVersioningDriver *driver, KisVersioningSettings *settings, ProjectsDocuments *documents = nullptr,
                  QObject *parent = nullptr);
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

    /// While false, ticks and unticks are refused (the boxes cannot be changed).
    void setChoicesAllowed(bool allowed);
    bool choicesAllowed() const;
    /// True while a refresh is reading the disk or waiting for the driver.
    bool isRefreshing() const;
    /// True while a folder choice of some project is under way, or not shown in the list yet.
    bool isBusy() const;

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
    /// isBusy() changed.
    void busyChanged();

private:
    struct Node;
    struct Project;
    struct Refresh;

    void readFolder(const std::shared_ptr<Refresh> &refresh, int project);
    void askStatus(const std::shared_ptr<Refresh> &refresh, int project);
    void askWhoIsEditing(const std::shared_ptr<Refresh> &refresh, int project);
    void askRemoteProjects(const std::shared_ptr<Refresh> &refresh);
    void askFolders(const std::shared_ptr<Refresh> &refresh, int project);
    /// Lists the project's folders, or takes the ones listed for the same version of the server.
    void settleFolders(const std::shared_ptr<Refresh> &refresh, int project);
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
    /// The folders of a downloaded project shown ticked: those of the server the driver did not leave out.
    QStringList keptFolders(const KisVersioningSettings::Project &stored) const;
    /// Why @p folder of @p stored may not be unticked now; empty when it may.
    QString untickRefusal(const KisVersioningSettings::Project &stored, const QString &folder) const;
    void runProjectJob(const QString &projectId, KisVersioningJob *job, const std::function<void(KisVersioningJob *)> &onSuccess);
    void releaseAfterRefresh(const QString &projectId);
    void setBusy(const QString &projectId, bool busy);
    /// Shows @p chosen in the box of @p folder until the list shows the outcome.
    void showChoice(const QString &projectId, const QString &folder, bool chosen);
    void forgetChoice(const QString &projectId, const QString &folder);
    bool isChecked(const Node *node) const;
    /// Tells the views that the row of the project and its folders' boxes changed.
    void projectChanged(const QString &projectId);

    KisVersioningDriver *m_driver;
    KisVersioningSettings *m_settings;
    ProjectsDocuments *m_documents;
    std::shared_ptr<Refresh> m_refresh;
    std::vector<Project> m_projects;
    std::unique_ptr<Node> m_root;
    bool m_signInNeeded {false};
    bool m_choicesAllowed {true};
    QString m_backupSuffix;                 ///< Krita's backup suffix, as the last refresh read it
    int m_changedFiles {0};
    int m_outsideFiles {0};
    int m_incoming {0};
    int m_pendingUpload {0};
    /// Projects with an operation under way, or whose operation the list does not show yet.
    QSet<QString> m_busyProjects;
    QHash<QString, QList<Choice>> m_waitingChoices;   ///< choices made while their project was busy
    QHash<QString, QString> m_downloadTargets;        ///< project id to the folder a download is filling
    /// The folders last listed for a project, by id, with the version of the server they belong to.
    struct ListedFolders {
        QString serverVersion;
        QStringList folders;
        QElapsedTimer age;
    };
    QHash<QString, ListedFolders> m_listedFolders;
    /// What the operation of a busy project last reported, by project id.
    struct Progress {
        qint64 done = 0;
        qint64 total = 0;
        QString label;
    };
    QHash<QString, Progress> m_progress;
    /// The choices shown in their boxes before the list shows them: project id, then folder in lower case.
    QHash<QString, QHash<QString, bool>> m_shownChoices;
};

#endif // PROJECTSMODEL_H
