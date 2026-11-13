/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsModel.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QMetaObject>
#include <QRegularExpression>
#include <QtConcurrent>

#include <klocalizedstring.h>

#include <KisVersioningFileRules.h>

using namespace KisVersioning;

namespace {

/// What a worker thread finds in a project folder. Paths are relative to the folder, with '/'.
struct FolderContents {
    bool exists = false;
    QStringList folders;
    QStringList files;
    QVector<KisVersioningConflictMarkers::Marker> markers;
};

/// Files never synchronised are not shown, nor anything in a hidden folder, where drivers keep their data.
bool isShown(const QString &relativePath)
{
    if (relativePath.isEmpty() || isNeverSynced(relativePath)) {
        return false;
    }
    const QStringList parts = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return std::none_of(parts.begin(), parts.end(), [](const QString &part) {
        return part.startsWith(QLatin1Char('.'));
    });
}

void collect(const QDir &directory, const QString &prefix, FolderContents *contents)
{
    const QFileInfoList entries = directory.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString relative = prefix + entry.fileName();
        if (!isShown(relative)) {
            continue;
        }
        if (entry.isDir()) {
            contents->folders << relative;
            collect(QDir(entry.absoluteFilePath()), relative + QLatin1Char('/'), contents);
        } else {
            contents->files << relative;
        }
    }
}

// Runs on a worker thread and touches nothing but its argument.
FolderContents readProjectFolder(const QString &root)
{
    FolderContents contents;
    contents.exists = !root.isEmpty() && QFileInfo(root).isDir();
    if (contents.exists) {
        collect(QDir(root), QString(), &contents);
        contents.markers = KisVersioningConflictMarkers(root).list();
    }
    return contents;
}

/// How a relative path is looked up: the file systems of Windows and macOS ignore case.
QString lookupKey(const QString &relativePath)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return relativePath.toLower();
#else
    return relativePath;
#endif
}

QString cleanPath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QString stateText(FileState state)
{
    switch (state) {
    case FileState::Modified:
        return i18n("changed");
    case FileState::Added:
        return i18n("new");
    case FileState::Deleted:
        return i18n("removed");
    case FileState::Unchanged:
        break;
    }
    return QString();
}

} // namespace

struct ProjectsModel::Node {
    enum Kind { ProjectNode, FolderNode, FileNode };

    Kind kind = FileNode;
    QString name;
    QString relativePath;       ///< relative to the project folder; empty for the project itself
    int project = -1;           ///< index in m_projects
    FileState state = FileState::Unchanged;
    QString editingUser;        ///< someone else editing the file
    QString markerCopyPath;     ///< for a conflict copy: the copy path its marker records
    QString choiceName;         ///< for a folder the driver lets the artist choose: the driver's own spelling
    bool checked = false;
    Node *parent = nullptr;
    int row = 0;
    std::vector<std::unique_ptr<Node>> children;
};

struct ProjectsModel::Project {
    KisVersioningSettings::Project entry;   ///< for a project that is not downloaded, localPath is empty
    bool downloaded = false;
    bool folderMissing = false;
    QString errorText;
    QStringList folders;
    QStringList files;
    QVector<KisVersioningConflictMarkers::Marker> markers;
    ProjectStatus status;
    QVector<EditingInfo> editing;
    QStringList remoteFolders;
};

/// The answers gathered by one refresh. Answers to an older refresh are ignored.
struct ProjectsModel::Refresh {
    std::vector<Project> projects;
    int outstanding = 0;
    bool signInNeeded = false;
};

ProjectsModel::ProjectsModel(KisVersioningDriver *driver, KisVersioningSettings *settings, QObject *parent)
    : QAbstractItemModel(parent)
    , m_driver(driver)
    , m_settings(settings)
    , m_root(std::make_unique<Node>())
{
}

// The jobs and the folder reading still under way are connected to this object only, so they end
// with nobody listening.
ProjectsModel::~ProjectsModel() = default;

void ProjectsModel::refresh()
{
    auto refresh = std::make_shared<Refresh>();
    m_refresh = refresh;

    const QString driverId = m_driver->id();
    const QVector<KisVersioningSettings::Project> stored = m_settings->projects();
    for (const KisVersioningSettings::Project &entry : stored) {
        if (entry.driverId == driverId) {
            Project project;
            project.entry = entry;
            project.downloaded = true;
            refresh->projects.push_back(project);
        }
    }

    // This step ends on the next turn of the event loop, so refreshed() is never emitted from
    // inside refresh(), not even with nothing to ask.
    ++refresh->outstanding;
    for (int i = 0; i < int(refresh->projects.size()); ++i) {
        readFolder(refresh, i);
    }
    if (m_driver->capabilities() & RemoteProjects) {
        askRemoteProjects(refresh);
    }
    QMetaObject::invokeMethod(this, [this, refresh]() { stepDone(refresh); }, Qt::QueuedConnection);
}

void ProjectsModel::readFolder(const std::shared_ptr<Refresh> &refresh, int project)
{
    ++refresh->outstanding;
    auto *watcher = new QFutureWatcher<FolderContents>(this);
    connect(watcher, &QFutureWatcher<FolderContents>::finished, this, [this, refresh, project, watcher]() {
        const FolderContents contents = watcher->result();
        watcher->deleteLater();
        if (refresh != m_refresh) {
            return;
        }
        Project &p = refresh->projects[size_t(project)];
        p.folderMissing = !contents.exists;
        p.folders = contents.folders;
        p.files = contents.files;
        p.markers = contents.markers;
        if (contents.exists) {
            // A folder that is not there is not asked about: the panel already says it is missing.
            askStatus(refresh, project);
            if (m_driver->capabilities() & Presence) {
                askWhoIsEditing(refresh, project);
            }
        }
        stepDone(refresh);
    });
    watcher->setFuture(QtConcurrent::run(readProjectFolder, refresh->projects[size_t(project)].entry.localPath));
}

void ProjectsModel::askStatus(const std::shared_ptr<Refresh> &refresh, int project)
{
    ++refresh->outstanding;
    KisVersioningJob *job = m_driver->status(refresh->projects[size_t(project)].entry.localPath);
    // The job deletes itself after finished(): its answer is read in the slot.
    connect(job, &KisVersioningJob::finished, this, [this, refresh, project](KisVersioningJob *job) {
        if (refresh != m_refresh) {
            return;
        }
        Project &p = refresh->projects[size_t(project)];
        if (job->error() == KisVersioningJob::NoError) {
            p.status = job->result<ProjectStatus>();
        } else if (job->error() == KisVersioningJob::NotSignedIn) {
            refresh->signInNeeded = true;
        } else {
            p.errorText = job->errorText();
        }
        stepDone(refresh);
    });
}

void ProjectsModel::askWhoIsEditing(const std::shared_ptr<Refresh> &refresh, int project)
{
    const Project &p = refresh->projects[size_t(project)];
    if (p.files.isEmpty()) {
        return;
    }
    ++refresh->outstanding;
    KisVersioningJob *job = m_driver->whoIsEditing(p.entry.localPath, p.files);
    connect(job, &KisVersioningJob::finished, this, [this, refresh, project](KisVersioningJob *job) {
        if (refresh != m_refresh) {
            return;
        }
        // Presence only informs: without an answer, nobody is shown as editing.
        if (job->error() == KisVersioningJob::NoError) {
            refresh->projects[size_t(project)].editing = job->result<QVector<EditingInfo>>();
        } else if (job->error() == KisVersioningJob::NotSignedIn) {
            refresh->signInNeeded = true;
        }
        stepDone(refresh);
    });
}

void ProjectsModel::askRemoteProjects(const std::shared_ptr<Refresh> &refresh)
{
    ++refresh->outstanding;
    KisVersioningJob *job = m_driver->listProjects(m_settings->serverAddress());
    connect(job, &KisVersioningJob::finished, this, [this, refresh](KisVersioningJob *job) {
        if (refresh != m_refresh) {
            return;
        }
        if (job->error() != KisVersioningJob::NoError) {
            // The downloaded projects stay listed either way.
            if (job->error() == KisVersioningJob::NotSignedIn) {
                refresh->signInNeeded = true;
            }
            stepDone(refresh);
            return;
        }
        const QVector<ProjectInfo> remote = job->result<QVector<ProjectInfo>>();
        for (const ProjectInfo &info : remote) {
            int index = -1;
            for (int i = 0; i < int(refresh->projects.size()); ++i) {
                const Project &p = refresh->projects[size_t(i)];
                if (p.downloaded && p.entry.info.id == info.id) {
                    index = i;
                    break;
                }
            }
            if (index < 0) {
                Project p;
                p.entry.driverId = m_driver->id();
                p.entry.info = info;
                refresh->projects.push_back(p);
                index = int(refresh->projects.size()) - 1;
            }
            askFolders(refresh, index);
        }
        stepDone(refresh);
    });
}

void ProjectsModel::askFolders(const std::shared_ptr<Refresh> &refresh, int project)
{
    ++refresh->outstanding;
    KisVersioningJob *job = m_driver->listFolders(refresh->projects[size_t(project)].entry.info);
    connect(job, &KisVersioningJob::finished, this, [this, refresh, project](KisVersioningJob *job) {
        if (refresh != m_refresh) {
            return;
        }
        if (job->error() == KisVersioningJob::NoError) {
            QStringList folders;
            const QStringList listed = job->result<QStringList>();
            for (const QString &folder : listed) {
                const QString clean = cleanPath(folder).remove(QRegularExpression(QStringLiteral("^/+")));
                if (isShown(clean) && !folders.contains(clean)) {
                    folders << clean;
                }
            }
            refresh->projects[size_t(project)].remoteFolders = folders;
        } else if (job->error() == KisVersioningJob::NotSignedIn) {
            refresh->signInNeeded = true;
        }
        stepDone(refresh);
    });
}

void ProjectsModel::stepDone(const std::shared_ptr<Refresh> &refresh)
{
    if (refresh != m_refresh || --refresh->outstanding > 0) {
        return;
    }
    m_refresh.reset();
    apply(*refresh);
    Q_EMIT refreshed();
}

void ProjectsModel::apply(const Refresh &refresh)
{
    beginResetModel();
    m_projects = refresh.projects;
    m_signInNeeded = refresh.signInNeeded;
    m_root = std::make_unique<Node>();
    m_changedFiles = 0;
    m_incoming = 0;
    m_pendingUpload = 0;
    for (int i = 0; i < int(m_projects.size()); ++i) {
        std::unique_ptr<Node> node = buildProject(i);
        node->parent = m_root.get();
        node->row = i;
        m_root->children.push_back(std::move(node));

        const Project &project = m_projects[size_t(i)];
        m_incoming += project.status.incoming;
        m_pendingUpload += project.status.pendingUpload;
        for (const FileStatus &file : project.status.files) {
            if (file.state != FileState::Unchanged && isShown(file.path)) {
                ++m_changedFiles;
            }
        }
    }
    endResetModel();
}

std::unique_ptr<ProjectsModel::Node> ProjectsModel::buildProject(int index) const
{
    const Project &project = m_projects[size_t(index)];
    auto root = std::make_unique<Node>();
    root->kind = Node::ProjectNode;
    root->name = project.entry.info.name.isEmpty() ? QFileInfo(project.entry.localPath).fileName()
                                                    : project.entry.info.name;
    root->project = index;

    QHash<QString, Node *> nodes;
    nodes.insert(QString(), root.get());
    std::function<Node *(const QString &, Node::Kind)> ensure = [&](const QString &relative, Node::Kind kind) {
        const QString key = lookupKey(relative);
        if (Node *existing = nodes.value(key)) {
            return existing;
        }
        const int slash = relative.lastIndexOf(QLatin1Char('/'));
        Node *parent = ensure(slash < 0 ? QString() : relative.left(slash), Node::FolderNode);
        auto node = std::make_unique<Node>();
        node->kind = kind;
        node->name = relative.mid(slash + 1);
        node->relativePath = relative;
        node->project = index;
        node->parent = parent;
        Node *raw = node.get();
        parent->children.push_back(std::move(node));
        nodes.insert(key, raw);
        return raw;
    };

    for (const QString &folder : project.folders) {
        ensure(folder, Node::FolderNode);
    }
    for (const QString &file : project.files) {
        ensure(file, Node::FileNode);
    }
    // A removed file is no longer on disk; it is listed from the status.
    for (const FileStatus &file : project.status.files) {
        if (isShown(file.path)) {
            ensure(file.path, Node::FileNode)->state = file.state;
        }
    }
    for (const QString &folder : project.remoteFolders) {
        Node *node = ensure(folder, Node::FolderNode);
        if (node->kind == Node::FolderNode) {
            node->choiceName = folder;
            node->checked = project.downloaded && project.entry.folders.contains(folder);
        }
    }
    for (const KisVersioningConflictMarkers::Marker &marker : project.markers) {
        Node *node = nodes.value(lookupKey(marker.copyPath));
        if (node && node->kind == Node::FileNode) {
            node->markerCopyPath = marker.copyPath;
        }
    }
    const QString me = m_driver->currentUser();
    for (const EditingInfo &editing : project.editing) {
        Node *node = nodes.value(lookupKey(editing.path));
        if (node && node->kind == Node::FileNode && !editing.user.isEmpty() && editing.user != me) {
            node->editingUser = editing.user;
        }
    }

    // Folders first, then files, each by name.
    std::function<void(Node *)> sort = [&sort](Node *node) {
        std::stable_sort(node->children.begin(), node->children.end(),
                         [](const std::unique_ptr<Node> &a, const std::unique_ptr<Node> &b) {
                             const bool aIsFile = a->kind == Node::FileNode;
                             const bool bIsFile = b->kind == Node::FileNode;
                             if (aIsFile != bIsFile) {
                                 return bIsFile;
                             }
                             return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
                         });
        for (int row = 0; row < int(node->children.size()); ++row) {
            node->children[size_t(row)]->row = row;
            sort(node->children[size_t(row)].get());
        }
    };
    sort(root.get());
    return root;
}

QString ProjectsModel::summaryText() const
{
    QStringList parts;
    if (m_changedFiles > 0) {
        parts << i18np("1 file changed", "%1 files changed", m_changedFiles);
    }
    if (m_incoming > 0) {
        parts << i18np("1 update", "%1 updates", m_incoming);
    }
    return parts.isEmpty() ? i18n("No changes") : parts.join(QStringLiteral(" · "));
}

int ProjectsModel::pendingUploadCount() const
{
    return m_pendingUpload;
}

QStringList ProjectsModel::downloadedProjectPaths() const
{
    QStringList paths;
    const QString driverId = m_driver->id();
    const QVector<KisVersioningSettings::Project> stored = m_settings->projects();
    for (const KisVersioningSettings::Project &entry : stored) {
        if (entry.driverId == driverId) {
            paths << entry.localPath;
        }
    }
    return paths;
}

bool ProjectsModel::signInNeeded() const
{
    return m_signInNeeded;
}

void ProjectsModel::addFolder(const QString &path)
{
    const QString folder = cleanPath(path);
    if (folder.isEmpty() || m_busyProjects.contains(folder)) {
        return;
    }
    KisVersioningSettings::Project entry;
    entry.driverId = m_driver->id();
    entry.info.id = folder;
    entry.info.name = QFileInfo(folder).fileName().isEmpty() ? folder : QFileInfo(folder).fileName();
    entry.localPath = folder;
    runProjectJob(folder, m_driver->downloadProject(entry.info, folder, QStringList()),
                  [this, entry]() { m_settings->addProject(entry); });
}

void ProjectsModel::markConflictResolved(const QModelIndex &index)
{
    const Node *node = nodeAt(index);
    if (!node || node->markerCopyPath.isEmpty()) {
        return;
    }
    // Only the marker goes: both files stay where they are.
    KisVersioningConflictMarkers(m_projects[size_t(node->project)].entry.localPath).remove(node->markerCopyPath);
    refresh();
}

bool ProjectsModel::hasUnsentWork(const Node *folder) const
{
    if (m_projects[size_t(folder->project)].status.pendingUpload > 0) {
        return true;
    }
    std::function<bool(const Node *)> changed = [&changed](const Node *node) {
        if (node->kind == Node::FileNode && node->state != FileState::Unchanged) {
            return true;
        }
        return std::any_of(node->children.begin(), node->children.end(),
                           [&changed](const std::unique_ptr<Node> &child) { return changed(child.get()); });
    };
    return changed(folder);
}

bool ProjectsModel::chooseFolder(Node *folder, bool chosen)
{
    const Project &project = m_projects[size_t(folder->project)];
    const QString id = project.entry.info.id;
    if (chosen == folder->checked || m_busyProjects.contains(id) || (!chosen && !project.downloaded)) {
        return false;
    }

    QStringList folders = project.downloaded ? project.entry.folders : QStringList();
    if (chosen) {
        folders << folder->choiceName;
    } else {
        // The driver may take the folder's files off this computer: work not sent yet would be lost.
        if (hasUnsentWork(folder)) {
            Q_EMIT operationFailed(i18n("%1 has work that was not sent yet. Synchronize before removing it from your choice.",
                                        folder->choiceName));
            return false;
        }
        folders.removeAll(folder->choiceName);
    }

    if (project.downloaded) {
        const QString localPath = project.entry.localPath;
        runProjectJob(id, m_driver->setFolders(localPath, folders),
                      [this, localPath, folders]() { m_settings->setFolders(localPath, folders); });
    } else {
        KisVersioningSettings::Project entry = project.entry;
        static const QRegularExpression notAllowed(QStringLiteral("[/\\\\:*?\"<>|]"));
        QString folderName = entry.info.name.isEmpty() ? entry.info.id : entry.info.name;
        folderName.replace(notAllowed, QStringLiteral("_"));
        entry.localPath = cleanPath(m_settings->projectsRoot() + QLatin1Char('/') + folderName);
        entry.folders = folders;
        runProjectJob(id, m_driver->downloadProject(entry.info, entry.localPath, folders),
                      [this, entry]() { m_settings->addProject(entry); });
    }
    return true;
}

void ProjectsModel::runProjectJob(const QString &projectId, KisVersioningJob *job, const std::function<void()> &onSuccess)
{
    m_busyProjects.insert(projectId);
    connect(job, &KisVersioningJob::finished, this, [this, projectId, onSuccess](KisVersioningJob *job) {
        m_busyProjects.remove(projectId);
        if (job->error() == KisVersioningJob::NoError) {
            onSuccess();
        } else {
            Q_EMIT operationFailed(job->errorText());
        }
        refresh();
    });
}

ProjectsModel::Node *ProjectsModel::nodeAt(const QModelIndex &index) const
{
    return index.isValid() ? static_cast<Node *>(index.internalPointer()) : nullptr;
}

QString ProjectsModel::absolutePath(const Node *node) const
{
    const Project &project = m_projects[size_t(node->project)];
    if (!project.downloaded) {
        return QString();
    }
    return node->relativePath.isEmpty() ? project.entry.localPath
                                        : project.entry.localPath + QLatin1Char('/') + node->relativePath;
}

QString ProjectsModel::noticeFor(const Node *node) const
{
    if (node->kind == Node::ProjectNode) {
        const Project &project = m_projects[size_t(node->project)];
        return project.folderMissing ? i18n("Folder not found") : project.errorText;
    }
    QStringList parts;
    const QString state = stateText(node->state);
    if (!state.isEmpty()) {
        parts << state;
    }
    if (!node->editingUser.isEmpty()) {
        parts << i18n("%1 editing", node->editingUser);
    }
    if (!node->markerCopyPath.isEmpty()) {
        parts << i18n("conflict to resolve");
    }
    return parts.join(QStringLiteral(" · "));
}

QModelIndex ProjectsModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= columnCount() || (parent.isValid() && parent.column() != 0)) {
        return QModelIndex();
    }
    const Node *parentNode = parent.isValid() ? nodeAt(parent) : m_root.get();
    if (row >= int(parentNode->children.size())) {
        return QModelIndex();
    }
    return createIndex(row, column, parentNode->children[size_t(row)].get());
}

QModelIndex ProjectsModel::parent(const QModelIndex &child) const
{
    const Node *node = nodeAt(child);
    if (!node || !node->parent || node->parent == m_root.get()) {
        return QModelIndex();
    }
    return createIndex(node->parent->row, 0, node->parent);
}

int ProjectsModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() && parent.column() != 0) {
        return 0;
    }
    const Node *node = parent.isValid() ? nodeAt(parent) : m_root.get();
    return int(node->children.size());
}

int ProjectsModel::columnCount(const QModelIndex &) const
{
    return 2;
}

QVariant ProjectsModel::data(const QModelIndex &index, int role) const
{
    const Node *node = nodeAt(index);
    if (!node) {
        return QVariant();
    }
    switch (role) {
    case Qt::DisplayRole:
        return index.column() == 0 ? node->name : noticeFor(node);
    case Qt::ToolTipRole: {
        const QString notice = noticeFor(node);
        if (!notice.isEmpty()) {
            return notice;
        }
        const QString path = absolutePath(node);
        return path.isEmpty() ? QVariant() : QVariant(QDir::toNativeSeparators(path));
    }
    case Qt::CheckStateRole:
        if (index.column() == 0 && !node->choiceName.isEmpty()) {
            return int(node->checked ? Qt::Checked : Qt::Unchecked);
        }
        return QVariant();
    case PathRole:
        return absolutePath(node);
    case StateRole:
        return node->kind == Node::FileNode ? QVariant(int(node->state)) : QVariant();
    case EditingUserRole:
        return node->editingUser;
    case ConflictCopyRole:
        return !node->markerCopyPath.isEmpty();
    case CheckedFolderRole:
        return !node->choiceName.isEmpty() ? QVariant(node->checked) : QVariant();
    default:
        return QVariant();
    }
}

bool ProjectsModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    Node *node = nodeAt(index);
    if (!node || role != Qt::CheckStateRole || index.column() != 0 || node->choiceName.isEmpty()) {
        return false;
    }
    return chooseFolder(node, value.toInt() == Qt::Checked);
}

Qt::ItemFlags ProjectsModel::flags(const QModelIndex &index) const
{
    const Node *node = nodeAt(index);
    if (!node) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == 0 && !node->choiceName.isEmpty()) {
        flags |= Qt::ItemIsUserCheckable;
    }
    return flags;
}
