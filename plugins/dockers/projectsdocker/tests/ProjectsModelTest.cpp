/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsModelTest.h"

#include <QAbstractItemModelTester>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
#include <functional>

#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KoDockFactoryBase.h>
#include <KoDockRegistry.h>

#include <KisVersioningConflictMarkers.h>
#include <KisVersioningSettings.h>
#include <KisVersioningSyncSession.h>

#include "KisVersioningFakeDriver.h"
#include "ProjectsDocuments.h"
#include "ProjectsModel.h"
#include "ProjectsSyncRunner.h"

using namespace KisVersioning;

namespace {

struct Fixture {
    QTemporaryDir dir;
    KSharedConfigPtr config;
    KisVersioningSettings settings;
    KisVersioningFakeDriver driver;

    Fixture()
        : config(KSharedConfig::openConfig(dir.filePath(QStringLiteral("settings")), KConfig::SimpleConfig))
        , settings(config)
    {
        settings.setProjectsRoot(dir.filePath(QStringLiteral("projects")));
    }

    QString path(const QString &relative) const { return dir.filePath(relative); }

    void write(const QString &relative, const QByteArray &content = "content")
    {
        QDir().mkpath(QFileInfo(path(relative)).absolutePath());
        QFile file(path(relative));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
    }

    void addProject(const QString &name, const QStringList &folders = {},
                    const QString &driverId = QStringLiteral("fake"))
    {
        KisVersioningSettings::Project project;
        project.driverId = driverId;
        project.info.id = name;
        project.info.name = name;
        project.localPath = path(name);
        project.folders = folders;
        settings.addProject(project);
    }

    /// One project, "game", with two folders and three files.
    void addGame()
    {
        write(QStringLiteral("game/ui/botao.kra"));
        write(QStringLiteral("game/ui/menu.kra"));
        write(QStringLiteral("game/art/heroi.kra"));
        addProject(QStringLiteral("game"));
    }
};

bool refreshAndWait(ProjectsModel &model)
{
    QSignalSpy spy(&model, &ProjectsModel::refreshed);
    model.refresh();
    return spy.wait(5000);
}

QModelIndex findPath(const QAbstractItemModel &model, const QString &path, const QModelIndex &parent = QModelIndex())
{
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (index.data(ProjectsModel::PathRole).toString() == path) {
            return index;
        }
        const QModelIndex found = findPath(model, path, index);
        if (found.isValid()) {
            return found;
        }
    }
    return QModelIndex();
}

QModelIndex childNamed(const QAbstractItemModel &model, const QModelIndex &parent, const QString &name)
{
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (index.data().toString() == name) {
            return index;
        }
    }
    return QModelIndex();
}

QStringList childNames(const QAbstractItemModel &model, const QModelIndex &parent)
{
    QStringList names;
    for (int row = 0; row < model.rowCount(parent); ++row) {
        names << model.index(row, 0, parent).data().toString();
    }
    return names;
}

/// The text shown beside the name.
QString notice(const QModelIndex &index)
{
    return index.sibling(index.row(), 1).data().toString();
}

ProjectStatus statusWith(const QString &path, FileState state, int incoming = 0)
{
    ProjectStatus status;
    FileStatus file;
    file.path = path;
    file.state = state;
    status.files.append(file);
    status.incoming = incoming;
    return status;
}

/// A driver whose status() jobs only end when the test ends them.
class HeldStatusDriver : public KisVersioningFakeDriver
{
public:
    QList<QPointer<KisVersioningJob>> held;

    KisVersioningJob *status(const QString &) override
    {
        auto *job = new KisVersioningJob(this);
        held.append(job);
        return job;
    }
};

/// Open documents for the runner tests. Saves wait until the test lets them end.
class HeldSaves : public KisVersioningDocumentHost
{
public:
    QHash<QString, QStringList> modified;  ///< modified documents per project folder
    QStringList openUnmodified;            ///< documents open without changes
    QList<std::function<void(bool)>> held;

    QStringList modifiedOpenDocuments(const QString &projectPath) const override
    {
        return modified.value(projectPath);
    }
    void saveDocument(const QString &, std::function<void(bool)> done) override { held.append(done); }
    bool isOpenAndUnmodified(const QString &path) const override { return openUnmodified.contains(path); }
    void reloadDocument(const QString &) override {}
    bool savedSince(const QString &, const QDateTime &) const override { return false; }
};

/// A driver whose downloadProject() jobs only end when the test ends them.
class HeldDownloadDriver : public KisVersioningFakeDriver
{
public:
    QList<QPointer<KisVersioningJob>> held;
    QStringList downloadedTo;

    KisVersioningJob *downloadProject(const ProjectInfo &, const QString &localPath, const QStringList &folders) override
    {
        downloadedTo << localPath;
        for (const QString &folder : folders) {
            QDir(localPath).mkpath(folder);
        }
        auto *job = new KisVersioningJob(this);
        held.append(job);
        return job;
    }
};

/// "game", downloaded with the folders "ui" and "art", from a driver with RemoteProjects.
void addRemoteGame(Fixture &f)
{
    f.driver.setCapabilities(RemoteProjects | PartialDownload);
    f.write(QStringLiteral("game/ui/botao.kra"));
    f.write(QStringLiteral("game/art/heroi.kra"));
    f.addProject(QStringLiteral("game"), {QStringLiteral("ui"), QStringLiteral("art")});
    f.driver.setProjects({ProjectInfo{QStringLiteral("game"), QStringLiteral("game"), QString()}});
    f.driver.setProjectFolders(QStringLiteral("game"), {QStringLiteral("art"), QStringLiteral("ui")});
    f.driver.setLocalProject(f.path(QStringLiteral("game")), QStringLiteral("game"));
}

/// Documents open in a pretend application, with or without changes.
class OpenDocuments : public ProjectsDocuments
{
public:
    QStringList open;

    QStringList modifiedOpenDocuments(const QString &) const override { return {}; }
    void saveDocument(const QString &, std::function<void(bool)> done) override { done(true); }
    bool isOpenAndUnmodified(const QString &path) const override { return open.contains(path); }
    void reloadDocument(const QString &) override {}
    bool savedSince(const QString &, const QDateTime &) const override { return false; }
    QStringList openDocumentsIn(const QString &folder) const override
    {
        QStringList inside;
        const QString prefix = QDir::cleanPath(folder) + QLatin1Char('/');
        for (const QString &path : open) {
            if (QDir::cleanPath(path).startsWith(prefix, Qt::CaseInsensitive)) {
                inside << path;
            }
        }
        return inside;
    }
    QStringList unsavedForeignDocuments(const QString &) const override { return {}; }
};

/// Server projects that each offer the folder "ui".
void serveProjects(KisVersioningFakeDriver &driver, const QVector<ProjectInfo> &projects)
{
    driver.setCapabilities(RemoteProjects | PartialDownload);
    driver.setProjects(projects);
    for (const ProjectInfo &project : projects) {
        driver.setProjectFolders(project.id, {QStringLiteral("ui")});
    }
}

/// Where each downloadProject() call put its project, in the order of the calls.
QStringList downloadTargets(const KisVersioningFakeDriver &driver)
{
    QStringList targets;
    const QList<KisVersioningFakeDriver::Call> calls = driver.callsNamed(QStringLiteral("downloadProject"));
    for (const KisVersioningFakeDriver::Call &call : calls) {
        targets << call.args.at(1);
    }
    return targets;
}

} // namespace

void ProjectsModelTest::init()
{
    // The assertions use the English source strings, whatever the machine's language.
    KLocalizedString::setLanguages({QStringLiteral("en_US")});
}

void ProjectsModelTest::testFoldersAndFilesAppear()
{
    Fixture f;
    f.addGame();
    ProjectsModel model(&f.driver, &f.settings);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QVERIFY(refreshAndWait(model));

    QCOMPARE(model.rowCount(), 1);
    const QModelIndex project = model.index(0, 0);
    QCOMPARE(project.data().toString(), QStringLiteral("game"));
    QCOMPARE(project.data(ProjectsModel::PathRole).toString(), f.path(QStringLiteral("game")));
    QCOMPARE(childNames(model, project), (QStringList{QStringLiteral("art"), QStringLiteral("ui")}));

    const QModelIndex ui = childNamed(model, project, QStringLiteral("ui"));
    QCOMPARE(ui.data(ProjectsModel::PathRole).toString(), f.path(QStringLiteral("game/ui")));
    QCOMPARE(childNames(model, ui), (QStringList{QStringLiteral("botao.kra"), QStringLiteral("menu.kra")}));
    QCOMPARE(childNames(model, childNamed(model, project, QStringLiteral("art"))),
             QStringList{QStringLiteral("heroi.kra")});

    const QModelIndex botao = childNamed(model, ui, QStringLiteral("botao.kra"));
    QCOMPARE(botao.data(ProjectsModel::PathRole).toString(), f.path(QStringLiteral("game/ui/botao.kra")));
    QCOMPARE(model.rowCount(botao), 0);
    QCOMPARE(f.driver.callsNamed(QStringLiteral("status")).size(), 1);
    QCOMPARE(f.driver.callsNamed(QStringLiteral("status")).first().args.first(), f.path(QStringLiteral("game")));
}

void ProjectsModelTest::testStatesComeFromStatus()
{
    Fixture f;
    f.addGame();
    f.driver.setStatus(statusWith(QStringLiteral("ui/botao.kra"), FileState::Modified));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    const QModelIndex botao = findPath(model, f.path(QStringLiteral("game/ui/botao.kra")));
    QVERIFY(botao.isValid());
    QCOMPARE(botao.data(ProjectsModel::StateRole).toInt(), int(FileState::Modified));
    QCOMPARE(notice(botao), QStringLiteral("changed"));

    const QModelIndex menu = findPath(model, f.path(QStringLiteral("game/ui/menu.kra")));
    QCOMPARE(menu.data(ProjectsModel::StateRole).toInt(), int(FileState::Unchanged));
    QVERIFY(notice(menu).isEmpty());
}

void ProjectsModelTest::testEditingUserIsShown()
{
    Fixture f;
    f.addGame();
    f.driver.setCapabilities(Presence);
    f.driver.setEditing({{QStringLiteral("ui/menu.kra"), QStringLiteral("Bruno")}});
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    const QModelIndex menu = findPath(model, f.path(QStringLiteral("game/ui/menu.kra")));
    QCOMPARE(menu.data(ProjectsModel::EditingUserRole).toString(), QStringLiteral("Bruno"));
    QCOMPARE(notice(menu), QStringLiteral("Bruno editing"));
    const QModelIndex botao = findPath(model, f.path(QStringLiteral("game/ui/botao.kra")));
    QVERIFY(botao.data(ProjectsModel::EditingUserRole).toString().isEmpty());

    const QList<KisVersioningFakeDriver::Call> asked = f.driver.callsNamed(QStringLiteral("whoIsEditing"));
    QCOMPARE(asked.size(), 1);
    QCOMPARE(asked.first().args.at(0), f.path(QStringLiteral("game")));
    QVERIFY(asked.first().args.at(1).split(QLatin1Char(',')).contains(QStringLiteral("ui/menu.kra")));
}

void ProjectsModelTest::testConflictCopyIsFlagged()
{
    Fixture f;
    f.addGame();
    f.write(QStringLiteral("game/ui/botao (Bruno).kra"));
    KisVersioningConflictMarkers::Marker marker;
    marker.copyPath = QStringLiteral("ui/botao (Bruno).kra");
    marker.originalPath = QStringLiteral("ui/botao.kra");
    marker.author = QStringLiteral("Ana");
    marker.date = QDateTime::currentDateTime();
    QVERIFY(KisVersioningConflictMarkers(f.path(QStringLiteral("game"))).add(marker));

    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    const QModelIndex copy = findPath(model, f.path(QStringLiteral("game/ui/botao (Bruno).kra")));
    QVERIFY(copy.isValid());
    QVERIFY(copy.data(ProjectsModel::ConflictCopyRole).toBool());
    QCOMPARE(notice(copy), QStringLiteral("conflict to resolve"));
    QVERIFY(!findPath(model, f.path(QStringLiteral("game/ui/botao.kra"))).data(ProjectsModel::ConflictCopyRole).toBool());

    // The markers themselves live in a hidden folder of the project, which is not listed.
    QCOMPARE(childNames(model, model.index(0, 0)), (QStringList{QStringLiteral("art"), QStringLiteral("ui")}));
}

void ProjectsModelTest::testSummaryText()
{
    Fixture f;
    f.addGame();
    f.driver.setStatus(statusWith(QStringLiteral("ui/botao.kra"), FileState::Modified, 2));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QCOMPARE(model.summaryText(), QString::fromUtf8("1 file changed \u00b7 2 updates"));
    QCOMPARE(model.pendingUploadCount(), 0);

    ProjectStatus status = statusWith(QStringLiteral("ui/botao.kra"), FileState::Modified);
    status.files.append({QStringLiteral("ui/menu.kra"), FileState::Added});
    status.pendingUpload = 1;
    f.driver.setStatus(status);
    QVERIFY(refreshAndWait(model));
    QCOMPARE(model.summaryText(), QStringLiteral("2 files changed"));
    QCOMPARE(model.pendingUploadCount(), 1);

    f.driver.setStatus(ProjectStatus());
    QVERIFY(refreshAndWait(model));
    QCOMPARE(model.summaryText(), QStringLiteral("No changes"));
}

void ProjectsModelTest::testNeverSyncedFilesAreHidden()
{
    Fixture f;
    f.addGame();
    f.write(QStringLiteral("game/ui/botao.kra~"));
    f.write(QStringLiteral("game/ui/botao.kra-autosave.kra"));
    // Even when a driver lists it, a backup file is not shown or counted.
    f.driver.setStatus(statusWith(QStringLiteral("ui/botao.kra~"), FileState::Added));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QVERIFY(!findPath(model, f.path(QStringLiteral("game/ui/botao.kra~"))).isValid());
    QVERIFY(!findPath(model, f.path(QStringLiteral("game/ui/botao.kra-autosave.kra"))).isValid());
    const QModelIndex ui = findPath(model, f.path(QStringLiteral("game/ui")));
    QCOMPARE(childNames(model, ui), (QStringList{QStringLiteral("botao.kra"), QStringLiteral("menu.kra")}));
    QCOMPARE(model.summaryText(), QStringLiteral("No changes"));
}

void ProjectsModelTest::testMissingProjectFolderShowsError()
{
    Fixture f;
    f.addGame();
    f.addProject(QStringLiteral("gone"));
    QVERIFY(!QFileInfo::exists(f.path(QStringLiteral("gone"))));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QCOMPARE(model.rowCount(), 2);
    const QModelIndex gone = findPath(model, f.path(QStringLiteral("gone")));
    QVERIFY(gone.isValid());
    QCOMPARE(gone.parent(), QModelIndex());
    QCOMPARE(model.rowCount(gone), 0);
    QCOMPARE(notice(gone), QStringLiteral("Folder not found"));
    QVERIFY(notice(model.index(0, 0)).isEmpty());

    // The driver is not asked about a folder that is not there; the other project is still read.
    const QList<KisVersioningFakeDriver::Call> asked = f.driver.callsNamed(QStringLiteral("status"));
    QCOMPARE(asked.size(), 1);
    QCOMPARE(asked.first().args.first(), f.path(QStringLiteral("game")));
    QVERIFY(findPath(model, f.path(QStringLiteral("game/ui/botao.kra"))).isValid());
}

void ProjectsModelTest::testNotSignedInKeepsProjectsListed()
{
    Fixture f;
    f.addGame();
    f.driver.setCapabilities(SignIn | RemoteProjects);
    f.driver.setStatusError(KisVersioningJob::NotSignedIn, QStringLiteral("signed out"));
    f.driver.setListProjectsError(KisVersioningJob::NotSignedIn, QStringLiteral("signed out"));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QVERIFY(model.signInNeeded());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.index(0, 0).data(ProjectsModel::PathRole).toString(), f.path(QStringLiteral("game")));
    QVERIFY(findPath(model, f.path(QStringLiteral("game/ui/botao.kra"))).isValid());
    QVERIFY(notice(model.index(0, 0)).isEmpty());

    f.driver.setStatus(ProjectStatus());
    f.driver.setProjects({});
    QVERIFY(refreshAndWait(model));
    QVERIFY(!model.signInNeeded());
}

void ProjectsModelTest::testOtherDriversProjectsAreHidden()
{
    Fixture f;
    f.addGame();
    f.write(QStringLiteral("other/a.kra"));
    f.addProject(QStringLiteral("other"), {}, QStringLiteral("localfolder"));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.index(0, 0).data(ProjectsModel::PathRole).toString(), f.path(QStringLiteral("game")));
    QCOMPARE(model.downloadedProjectPaths(), QStringList{f.path(QStringLiteral("game"))});
}

void ProjectsModelTest::testRemovedFileIsListed()
{
    Fixture f;
    f.addGame();
    f.driver.setStatus(statusWith(QStringLiteral("ui/antigo.kra"), FileState::Deleted));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    const QModelIndex removed = findPath(model, f.path(QStringLiteral("game/ui/antigo.kra")));
    QVERIFY(removed.isValid());
    QCOMPARE(removed.data(ProjectsModel::StateRole).toInt(), int(FileState::Deleted));
    QCOMPARE(notice(removed), QStringLiteral("removed"));
    QCOMPARE(model.summaryText(), QStringLiteral("1 file changed"));
}

void ProjectsModelTest::testOwnEditingIsNotShown()
{
    Fixture f;
    f.addGame();
    f.driver.setCapabilities(Presence);
    f.driver.setCurrentUser(QStringLiteral("Ana"));
    f.driver.setEditing({{QStringLiteral("ui/menu.kra"), QStringLiteral("Ana")}});
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    const QModelIndex menu = findPath(model, f.path(QStringLiteral("game/ui/menu.kra")));
    QVERIFY(menu.data(ProjectsModel::EditingUserRole).toString().isEmpty());
    QVERIFY(notice(menu).isEmpty());

    // Without Presence the driver is not asked.
    f.driver.setCapabilities(Capabilities());
    const int asked = f.driver.callCount(QStringLiteral("whoIsEditing"));
    QVERIFY(refreshAndWait(model));
    QCOMPARE(f.driver.callCount(QStringLiteral("whoIsEditing")), asked);
}

void ProjectsModelTest::testAddFolderAddsProject()
{
    Fixture f;
    f.write(QStringLiteral("Folder A/a.kra"));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    QCOMPARE(model.rowCount(), 0);

    QSignalSpy spy(&model, &ProjectsModel::refreshed);
    model.addFolder(f.path(QStringLiteral("Folder A")));
    QVERIFY(spy.wait(5000));

    const QList<KisVersioningFakeDriver::Call> downloads = f.driver.callsNamed(QStringLiteral("downloadProject"));
    QCOMPARE(downloads.size(), 1);
    QCOMPARE(downloads.first().args,
             (QStringList{f.path(QStringLiteral("Folder A")), f.path(QStringLiteral("Folder A")), QString()}));

    const QVector<KisVersioningSettings::Project> projects = f.settings.projects();
    QCOMPARE(projects.size(), 1);
    QCOMPARE(projects.first().driverId, QStringLiteral("fake"));
    QCOMPARE(projects.first().info.name, QStringLiteral("Folder A"));
    QCOMPARE(projects.first().localPath, f.path(QStringLiteral("Folder A")));

    QCOMPARE(model.rowCount(), 1);
    QVERIFY(findPath(model, f.path(QStringLiteral("Folder A/a.kra"))).isValid());
}

void ProjectsModelTest::testTickingFolderDownloadsProject()
{
    Fixture f;
    f.driver.setCapabilities(SignIn | RemoteProjects | PartialDownload);
    f.settings.setServerAddress(QStringLiteral("server.example"));
    ProjectInfo space;
    space.id = QStringLiteral("p2");
    space.name = QStringLiteral("Space");
    f.driver.setProjects({space});
    f.driver.setProjectFolders(QStringLiteral("p2"), {QStringLiteral("art"), QStringLiteral("ui")});
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    QCOMPARE(f.driver.callsNamed(QStringLiteral("listProjects")).first().args,
             QStringList{QStringLiteral("server.example")});

    QCOMPARE(model.rowCount(), 1);
    const QModelIndex project = model.index(0, 0);
    QCOMPARE(project.data().toString(), QStringLiteral("Space"));
    QVERIFY(project.data(ProjectsModel::PathRole).toString().isEmpty());
    QModelIndex ui = childNamed(model, project, QStringLiteral("ui"));
    QVERIFY(ui.isValid());
    QVERIFY(model.flags(ui) & Qt::ItemIsUserCheckable);
    QCOMPARE(ui.data(ProjectsModel::CheckedFolderRole).toBool(), false);
    QCOMPARE(ui.data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));

    QSignalSpy spy(&model, &ProjectsModel::refreshed);
    QVERIFY(model.setData(ui, Qt::Checked, Qt::CheckStateRole));
    QVERIFY(spy.wait(5000));

    const QString localPath = f.path(QStringLiteral("projects/Space"));
    const QList<KisVersioningFakeDriver::Call> downloads = f.driver.callsNamed(QStringLiteral("downloadProject"));
    QCOMPARE(downloads.size(), 1);
    QCOMPARE(downloads.first().args, (QStringList{QStringLiteral("p2"), localPath, QStringLiteral("ui")}));

    const QVector<KisVersioningSettings::Project> projects = f.settings.projects();
    QCOMPARE(projects.size(), 1);
    QCOMPARE(projects.first().driverId, QStringLiteral("fake"));
    QCOMPARE(projects.first().info.id, QStringLiteral("p2"));
    QCOMPARE(projects.first().localPath, localPath);
    QCOMPARE(projects.first().folders, QStringList{QStringLiteral("ui")});
    // What the driver left out is kept: it is what the list shows unticked.
    QCOMPARE(projects.first().leftOut, QStringList{QStringLiteral("art")});

    // The downloaded project takes the place of the remote one.
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.index(0, 0).data(ProjectsModel::PathRole).toString(), localPath);
    ui = childNamed(model, model.index(0, 0), QStringLiteral("ui"));
    QCOMPARE(ui.data(ProjectsModel::CheckedFolderRole).toBool(), true);
    QCOMPARE(childNamed(model, model.index(0, 0), QStringLiteral("art")).data(ProjectsModel::CheckedFolderRole).toBool(),
             false);

    // A later tick changes the choice of the downloaded project.
    const QModelIndex art = childNamed(model, model.index(0, 0), QStringLiteral("art"));
    QVERIFY(model.setData(art, Qt::Checked, Qt::CheckStateRole));
    QVERIFY(spy.wait(5000));
    QCOMPARE(f.driver.callsNamed(QStringLiteral("setFolders")).first().args,
             (QStringList{localPath, QStringLiteral("ui,art")}));
    QCOMPARE(f.settings.projects().first().folders, (QStringList{QStringLiteral("ui"), QStringLiteral("art")}));
    QVERIFY(f.settings.projects().first().leftOut.isEmpty());
    QCOMPARE(f.driver.callCount(QStringLiteral("downloadProject")), 1);
}

void ProjectsModelTest::testUntickingFolderNarrowsTheChoice()
{
    Fixture f;
    f.driver.setCapabilities(RemoteProjects | PartialDownload);
    f.write(QStringLiteral("game/ui/botao.kra"));
    f.write(QStringLiteral("game/art/heroi.kra"));
    f.addProject(QStringLiteral("game"), {QStringLiteral("ui"), QStringLiteral("art")});
    f.driver.setProjects({ProjectInfo{QStringLiteral("game"), QStringLiteral("game"), QString()}});
    f.driver.setProjectFolders(QStringLiteral("game"), {QStringLiteral("art"), QStringLiteral("ui")});
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    QCOMPARE(model.rowCount(), 1);

    const QModelIndex art = findPath(model, f.path(QStringLiteral("game/art")));
    QCOMPARE(art.data(ProjectsModel::CheckedFolderRole).toBool(), true);
    QSignalSpy spy(&model, &ProjectsModel::refreshed);
    QVERIFY(model.setData(art, Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(spy.wait(5000));

    QCOMPARE(f.driver.callsNamed(QStringLiteral("setFolders")).first().args,
             (QStringList{f.path(QStringLiteral("game")), QStringLiteral("ui")}));
    QCOMPARE(f.settings.projects().first().folders, QStringList{QStringLiteral("ui")});
}

void ProjectsModelTest::testUntickingFolderWithUnsentWorkIsRefused()
{
    Fixture f;
    addRemoteGame(f);
    f.driver.setStatus(statusWith(QStringLiteral("ui/botao.kra"), FileState::Modified));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QSignalSpy failures(&model, &ProjectsModel::operationFailed);
    QVERIFY(model.setData(findPath(model, f.path(QStringLiteral("game/ui"))), Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(failures.wait(5000));
    QCOMPARE(f.driver.callCount(QStringLiteral("setFolders")), 0);
    QCOMPARE(f.settings.projects().first().folders, (QStringList{QStringLiteral("ui"), QStringLiteral("art")}));

    // A version that was not sent yet holds work too.
    ProjectStatus pending;
    pending.pendingUpload = 1;
    f.driver.setStatus(pending);
    QVERIFY(refreshAndWait(model));
    QVERIFY(model.setData(findPath(model, f.path(QStringLiteral("game/art"))), Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(failures.wait(5000));
    QCOMPARE(failures.size(), 2);
    QCOMPARE(f.driver.callCount(QStringLiteral("setFolders")), 0);
}

void ProjectsModelTest::testUntickAsksForTheStatusAgain()
{
    Fixture f;
    addRemoteGame(f);
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    QCOMPARE(findPath(model, f.path(QStringLiteral("game/ui/botao.kra"))).data(ProjectsModel::StateRole).toInt(),
             int(FileState::Unchanged));

    // The file changes after the last refresh, outside the application: the list does not know.
    f.driver.setStatus(statusWith(QStringLiteral("ui/botao.kra"), FileState::Modified));
    const int asked = f.driver.callCount(QStringLiteral("status"));
    QSignalSpy failures(&model, &ProjectsModel::operationFailed);
    QVERIFY(model.setData(findPath(model, f.path(QStringLiteral("game/ui"))), Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(failures.wait(5000));

    QCOMPARE(f.driver.callsNamed(QStringLiteral("status")).at(asked).args, QStringList{f.path(QStringLiteral("game"))});
    QCOMPARE(f.driver.callCount(QStringLiteral("setFolders")), 0);
    QCOMPARE(f.settings.projects().first().folders, (QStringList{QStringLiteral("ui"), QStringLiteral("art")}));
}

void ProjectsModelTest::testUntickGoesAheadWhenTheStatusIsClean()
{
    Fixture f;
    addRemoteGame(f);
    f.driver.setStatus(statusWith(QStringLiteral("ui/botao.kra"), FileState::Modified));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    QCOMPARE(findPath(model, f.path(QStringLiteral("game/ui/botao.kra"))).data(ProjectsModel::StateRole).toInt(),
             int(FileState::Modified));

    // Synchronised since the last refresh: the status is clean now, whatever the list shows.
    f.driver.setStatus(ProjectStatus());
    QSignalSpy failures(&model, &ProjectsModel::operationFailed);
    QVERIFY(model.setData(findPath(model, f.path(QStringLiteral("game/ui"))), Qt::Unchecked, Qt::CheckStateRole));
    QTRY_COMPARE(f.driver.callCount(QStringLiteral("setFolders")), 1);
    QCOMPARE(f.driver.callsNamed(QStringLiteral("setFolders")).first().args,
             (QStringList{f.path(QStringLiteral("game")), QStringLiteral("art")}));
    QTRY_COMPARE(f.settings.projects().first().folders, QStringList{QStringLiteral("art")});
    QVERIFY(failures.isEmpty());
}

void ProjectsModelTest::testSecondTickWaitsForTheFirst()
{
    Fixture f;
    HeldDownloadDriver driver;
    serveProjects(driver, {ProjectInfo{QStringLiteral("p2"), QStringLiteral("Space"), QString()}});
    driver.setProjectFolders(QStringLiteral("p2"), {QStringLiteral("art"), QStringLiteral("ui")});
    ProjectsModel model(&driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    const QModelIndex project = model.index(0, 0);

    QVERIFY(model.setData(childNamed(model, project, QStringLiteral("ui")), Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(driver.held.size(), 1);

    // The download ends; the refresh that would show it has not landed yet.
    const QPointer<KisVersioningJob> download = driver.held.first();
    // As a driver does, it says which folders of the server it left out.
    download->finish(QVariant::fromValue(QStringList{QStringLiteral("art")}));
    QCoreApplication::sendPostedEvents(download, QEvent::MetaCall);
    QCOMPARE(f.settings.projects().size(), 1);

    // A second tick, on a list that still shows the project as not downloaded: it waits its turn.
    QVERIFY(model.setData(childNamed(model, project, QStringLiteral("art")), Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(driver.downloadedTo.size(), 1);
    QCOMPARE(driver.callCount(QStringLiteral("setFolders")), 0);

    // Its turn comes once the list shows the download, and it keeps the first folder.
    const QString localPath = f.path(QStringLiteral("projects/Space"));
    QTRY_COMPARE(driver.callCount(QStringLiteral("setFolders")), 1);
    QCOMPARE(driver.callsNamed(QStringLiteral("setFolders")).first().args,
             (QStringList{localPath, QStringLiteral("ui,art")}));
    QTRY_COMPARE(f.settings.projects().first().folders, (QStringList{QStringLiteral("ui"), QStringLiteral("art")}));
    QCOMPARE(driver.downloadedTo, QStringList{localPath});
    QCOMPARE(f.settings.projects().size(), 1);
}

void ProjectsModelTest::testDownloadFolderStaysInsideTheProjectsFolder()
{
    Fixture f;
    serveProjects(f.driver, {ProjectInfo{QStringLiteral("dots"), QStringLiteral(".."), QString()},
                             ProjectInfo{QStringLiteral("dot"), QStringLiteral("."), QString()},
                             ProjectInfo{QStringLiteral("empty"), QString(), QString()},
                             ProjectInfo{QStringLiteral("slash"), QStringLiteral("a/b"), QString()},
                             ProjectInfo{QStringLiteral("backslash"), QStringLiteral("c\\d"), QString()}});
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    QCOMPARE(model.rowCount(), 5);
    for (int row = 0; row < 5; ++row) {
        QVERIFY(model.setData(childNamed(model, model.index(row, 0), QStringLiteral("ui")), Qt::Checked,
                              Qt::CheckStateRole));
    }
    QTRY_COMPARE(f.driver.callCount(QStringLiteral("downloadProject")), 5);

    const QString root = f.path(QStringLiteral("projects"));
    const QStringList expected{root + QStringLiteral("/dots"), root + QStringLiteral("/dot"),
                               root + QStringLiteral("/empty"), root + QStringLiteral("/a_b"),
                               root + QStringLiteral("/c_d")};
    QCOMPARE(downloadTargets(f.driver), expected);
    for (const QString &target : expected) {
        QCOMPARE(QFileInfo(target).absolutePath(), root);
    }
    // Nothing was made beside the projects folder.
    QVERIFY(!QFileInfo::exists(f.path(QStringLiteral("ui"))));
}

void ProjectsModelTest::testSameNameGetsAFreeFolder()
{
    Fixture f;
    serveProjects(f.driver, {ProjectInfo{QStringLiteral("p1"), QStringLiteral("Space"), QString()},
                             ProjectInfo{QStringLiteral("p2"), QStringLiteral("Space"), QString()},
                             ProjectInfo{QStringLiteral("p3"), QStringLiteral("Space"), QString()},
                             ProjectInfo{QStringLiteral("p4"), QStringLiteral("Space"), QString()}});
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    const QString root = f.path(QStringLiteral("projects"));

    // One after the other: the second finds the first in the settings.
    QSignalSpy spy(&model, &ProjectsModel::refreshed);
    QVERIFY(model.setData(childNamed(model, model.index(0, 0), QStringLiteral("ui")), Qt::Checked, Qt::CheckStateRole));
    QVERIFY(spy.wait(5000));
    QVERIFY(model.setData(childNamed(model, model.index(1, 0), QStringLiteral("ui")), Qt::Checked, Qt::CheckStateRole));
    QVERIFY(spy.wait(5000));
    QCOMPARE(downloadTargets(f.driver), (QStringList{root + QStringLiteral("/Space"), root + QStringLiteral("/Space (2)")}));

    // At the same time: the second finds the first's download under way.
    QVERIFY(model.setData(childNamed(model, model.index(2, 0), QStringLiteral("ui")), Qt::Checked, Qt::CheckStateRole));
    QVERIFY(model.setData(childNamed(model, model.index(3, 0), QStringLiteral("ui")), Qt::Checked, Qt::CheckStateRole));
    QTRY_COMPARE(f.settings.projects().size(), 4);
    QCOMPARE(downloadTargets(f.driver).mid(2),
             (QStringList{root + QStringLiteral("/Space (3)"), root + QStringLiteral("/Space (4)")}));
}

void ProjectsModelTest::testFolderInUseIsNotReused()
{
    Fixture f;
    f.write(QStringLiteral("projects/Space/notes.txt"), "someone's notes");
    QVERIFY(QDir().mkpath(f.path(QStringLiteral("projects/Empty"))));
    serveProjects(f.driver, {ProjectInfo{QStringLiteral("p1"), QStringLiteral("Space"), QString()},
                             ProjectInfo{QStringLiteral("p2"), QStringLiteral("Empty"), QString()}});
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QVERIFY(model.setData(childNamed(model, model.index(0, 0), QStringLiteral("ui")), Qt::Checked, Qt::CheckStateRole));
    QVERIFY(model.setData(childNamed(model, model.index(1, 0), QStringLiteral("ui")), Qt::Checked, Qt::CheckStateRole));
    QTRY_COMPARE(f.settings.projects().size(), 2);

    // A folder that holds something is left alone; an empty one is used.
    const QString root = f.path(QStringLiteral("projects"));
    QCOMPARE(downloadTargets(f.driver), (QStringList{root + QStringLiteral("/Space (2)"), root + QStringLiteral("/Empty")}));
    QFile notes(f.path(QStringLiteral("projects/Space/notes.txt")));
    QVERIFY(notes.open(QIODevice::ReadOnly));
    QCOMPARE(notes.readAll(), QByteArray("someone's notes"));
}

void ProjectsModelTest::testOtherDriversProjectIsLeftAlone()
{
    Fixture f;
    KisVersioningSettings::Project other;
    other.driverId = QStringLiteral("localfolder");
    other.info.id = f.path(QStringLiteral("projects/Space"));
    other.info.name = QStringLiteral("Space");
    other.localPath = f.path(QStringLiteral("projects/Space"));
    f.settings.addProject(other);
    serveProjects(f.driver, {ProjectInfo{QStringLiteral("p1"), QStringLiteral("Space"), QString()}});
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QVERIFY(model.setData(childNamed(model, model.index(0, 0), QStringLiteral("ui")), Qt::Checked, Qt::CheckStateRole));
    QTRY_COMPARE(f.settings.projects().size(), 2);

    QCOMPARE(downloadTargets(f.driver), QStringList{f.path(QStringLiteral("projects/Space (2)"))});
    const QVector<KisVersioningSettings::Project> projects = f.settings.projects();
    const auto kept = std::find_if(projects.begin(), projects.end(), [](const KisVersioningSettings::Project &p) {
        return p.driverId == QStringLiteral("localfolder");
    });
    QVERIFY(kept != projects.end());
    QCOMPARE(kept->localPath, other.localPath);
    QCOMPARE(kept->info.id, other.info.id);
    QCOMPARE(kept->info.name, other.info.name);
}

void ProjectsModelTest::testConflictResolvedRemovesOnlyTheMarker()
{
    Fixture f;
    f.addGame();
    f.write(QStringLiteral("game/ui/botao (Bruno).kra"));
    KisVersioningConflictMarkers markers(f.path(QStringLiteral("game")));
    KisVersioningConflictMarkers::Marker marker;
    marker.copyPath = QStringLiteral("ui/botao (Bruno).kra");
    marker.originalPath = QStringLiteral("ui/botao.kra");
    QVERIFY(markers.add(marker));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QSignalSpy spy(&model, &ProjectsModel::refreshed);
    model.markConflictResolved(findPath(model, f.path(QStringLiteral("game/ui/botao (Bruno).kra"))));
    QVERIFY(spy.wait(5000));

    QVERIFY(markers.list().isEmpty());
    QVERIFY(QFileInfo::exists(f.path(QStringLiteral("game/ui/botao (Bruno).kra"))));
    QVERIFY(QFileInfo::exists(f.path(QStringLiteral("game/ui/botao.kra"))));
    const QModelIndex copy = findPath(model, f.path(QStringLiteral("game/ui/botao (Bruno).kra")));
    QVERIFY(copy.isValid());
    QVERIFY(!copy.data(ProjectsModel::ConflictCopyRole).toBool());
}

void ProjectsModelTest::testDestroyedWhileRefreshingIsSafe()
{
    Fixture f;
    f.addGame();
    HeldStatusDriver driver;
    driver.setCapabilities(Presence);

    // Gone while the folder is being read.
    auto *model = new ProjectsModel(&driver, &f.settings);
    model->refresh();
    delete model;
    QTest::qWait(100);
    QVERIFY(driver.held.isEmpty());

    // Gone while the driver is working.
    model = new ProjectsModel(&driver, &f.settings);
    model->refresh();
    QTRY_VERIFY(!driver.held.isEmpty());
    delete model;
    for (const QPointer<KisVersioningJob> &job : std::as_const(driver.held)) {
        QVERIFY(job);
        job->finish(QVariant::fromValue(ProjectStatus()));
    }
    QTest::qWait(100);
    for (const QPointer<KisVersioningJob> &job : std::as_const(driver.held)) {
        QVERIFY(!job); // ended and deleted itself, with nobody listening
    }
}

void ProjectsModelTest::testFileOutsideTheChoiceIsShownAndCounted()
{
    Fixture f;
    f.driver.setCapabilities(RemoteProjects | PartialDownload);
    f.write(QStringLiteral("game/ui/botao.kra"));
    f.write(QStringLiteral("game/raiz.kra"));
    // Made here: a file in a folder the driver left out, and a folder the server does not have.
    f.write(QStringLiteral("game/concepts/meu.kra"));
    f.write(QStringLiteral("game/icons/novo.kra"));
    KisVersioningSettings::Project project;
    project.driverId = QStringLiteral("fake");
    project.info = ProjectInfo{QStringLiteral("game"), QStringLiteral("game"), QString()};
    project.localPath = f.path(QStringLiteral("game"));
    project.folders = QStringList{QStringLiteral("ui")};
    project.leftOut = QStringList{QStringLiteral("concepts")};
    f.settings.addProject(project);
    f.driver.setProjects({project.info});
    f.driver.setProjectFolders(QStringLiteral("game"), {QStringLiteral("concepts"), QStringLiteral("ui")});
    f.driver.setStatus(statusWith(QStringLiteral("icons/novo.kra"), FileState::Added));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    // Never sent, and said so.
    const QModelIndex mine = findPath(model, f.path(QStringLiteral("game/concepts/meu.kra")));
    QVERIFY(mine.isValid());
    QVERIFY(mine.data(ProjectsModel::OutsideRole).toBool());
    QCOMPARE(notice(mine), QStringLiteral("not sent: outside your folders"));
    QCOMPARE(findPath(model, f.path(QStringLiteral("game/concepts"))).data(ProjectsModel::CheckedFolderRole).toBool(), false);

    // The folder made here is sent like any other: it shows what the driver says of it.
    const QModelIndex fresh = findPath(model, f.path(QStringLiteral("game/icons/novo.kra")));
    QVERIFY(!fresh.data(ProjectsModel::OutsideRole).toBool());
    QCOMPARE(notice(fresh), QStringLiteral("new"));
    QVERIFY(!findPath(model, f.path(QStringLiteral("game/ui/botao.kra"))).data(ProjectsModel::OutsideRole).toBool());
    QVERIFY(!findPath(model, f.path(QStringLiteral("game/raiz.kra"))).data(ProjectsModel::OutsideRole).toBool());
    QCOMPARE(findPath(model, f.path(QStringLiteral("game/ui"))).data(ProjectsModel::CheckedFolderRole).toBool(), true);

    QCOMPARE(model.summaryText(), QString::fromUtf8("1 file changed \u00b7 1 file outside your folders"));
}

void ProjectsModelTest::testFoldersNotLeftOutStayInTheChoice()
{
    Fixture f;
    f.driver.setCapabilities(RemoteProjects | PartialDownload);
    f.write(QStringLiteral("game/ui/botao.kra"));
    f.write(QStringLiteral("game/icons/novo.kra"));
    KisVersioningSettings::Project project;
    project.driverId = QStringLiteral("fake");
    project.info = ProjectInfo{QStringLiteral("game"), QStringLiteral("game"), QString()};
    project.localPath = f.path(QStringLiteral("game"));
    project.folders = QStringList{QStringLiteral("ui")};
    project.leftOut = QStringList{QStringLiteral("concepts")};
    f.settings.addProject(project);
    f.driver.setProjects({project.info});
    // "icons" was made here and sent since: the server has it, and the driver never left it out.
    f.driver.setProjectFolders(QStringLiteral("game"),
                               {QStringLiteral("concepts"), QStringLiteral("icons"), QStringLiteral("ui")});
    f.driver.setLocalProject(project.localPath, QStringLiteral("game"));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    QCOMPARE(findPath(model, f.path(QStringLiteral("game/icons"))).data(ProjectsModel::CheckedFolderRole).toBool(), true);

    // Ticking another folder keeps it: the driver is given every folder shown ticked.
    QSignalSpy spy(&model, &ProjectsModel::refreshed);
    QVERIFY(model.setData(findPath(model, f.path(QStringLiteral("game/concepts"))), Qt::Checked, Qt::CheckStateRole));
    QVERIFY(spy.wait(5000));
    QCOMPARE(f.driver.callsNamed(QStringLiteral("setFolders")).first().args,
             (QStringList{f.path(QStringLiteral("game")), QStringLiteral("icons,ui,concepts")}));
    QVERIFY(f.settings.projects().first().leftOut.isEmpty());
    for (const QString &folder : {QStringLiteral("concepts"), QStringLiteral("icons"), QStringLiteral("ui")}) {
        QCOMPARE(findPath(model, f.path(QStringLiteral("game/") + folder)).data(ProjectsModel::CheckedFolderRole).toBool(),
                 true);
    }
}

void ProjectsModelTest::testUntickRefusedWhileADocumentOfTheFolderIsOpen()
{
    Fixture f;
    addRemoteGame(f);
    OpenDocuments documents;
    documents.open << f.path(QStringLiteral("game/art/heroi.kra"));
    ProjectsModel model(&f.driver, &f.settings, &documents);
    QVERIFY(refreshAndWait(model));

    // Open, with or without changes: the folder stays chosen, and the artist is told why.
    QSignalSpy failures(&model, &ProjectsModel::operationFailed);
    QVERIFY(!model.setData(findPath(model, f.path(QStringLiteral("game/art"))), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(failures.size(), 1);
    QCOMPARE(failures.first().first().toString(), QStringLiteral("Close heroi.kra before removing art from your choice."));
    QCOMPARE(f.driver.callCount(QStringLiteral("setFolders")), 0);
    QCOMPARE(f.settings.projects().first().folders, (QStringList{QStringLiteral("ui"), QStringLiteral("art")}));

    // Opened while the state was being asked: still refused.
    documents.open.clear();
    f.driver.setHeld(QStringLiteral("status"), true);
    QVERIFY(model.setData(findPath(model, f.path(QStringLiteral("game/art"))), Qt::Unchecked, Qt::CheckStateRole));
    QTRY_COMPARE(f.driver.heldCount(QStringLiteral("status")), 1);
    documents.open << f.path(QStringLiteral("game/art/heroi.kra"));
    f.driver.setHeld(QStringLiteral("status"), false);
    f.driver.releaseHeld(QStringLiteral("status"));
    QTRY_COMPARE(failures.size(), 2);
    QCOMPARE(f.driver.callCount(QStringLiteral("setFolders")), 0);

    // Closed: it goes ahead. A document elsewhere does not stop it.
    documents.open = QStringList{f.path(QStringLiteral("game/ui/botao.kra"))};
    QTRY_VERIFY(!model.isBusy());
    QVERIFY(model.setData(findPath(model, f.path(QStringLiteral("game/art"))), Qt::Unchecked, Qt::CheckStateRole));
    QTRY_COMPARE(f.driver.callCount(QStringLiteral("setFolders")), 1);
    QCOMPARE(f.driver.callsNamed(QStringLiteral("setFolders")).first().args,
             (QStringList{f.path(QStringLiteral("game")), QStringLiteral("ui")}));
    QTRY_COMPARE(f.settings.projects().first().leftOut, QStringList{QStringLiteral("art")});
}

void ProjectsModelTest::testLastFolderCannotBeUnticked()
{
    Fixture f;
    addRemoteGame(f);
    KisVersioningSettings::Project project = f.settings.projects().first();
    f.settings.setFolders(project.localPath, {QStringLiteral("ui")}, {QStringLiteral("art")});
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));
    const QModelIndex ui = findPath(model, f.path(QStringLiteral("game/ui")));
    QCOMPARE(ui.data(ProjectsModel::CheckedFolderRole).toBool(), true);

    // No folder left would mean the whole project to the driver: the last one stays.
    QSignalSpy failures(&model, &ProjectsModel::operationFailed);
    QVERIFY(!model.setData(ui, Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(failures.size(), 1);
    QCOMPARE(failures.first().first().toString(), QStringLiteral("Keep at least one folder of game on this computer."));
    QTest::qWait(50);
    QCOMPARE(f.driver.callCount(QStringLiteral("setFolders")), 0);
    QCOMPARE(f.settings.projects().first().folders, QStringList{QStringLiteral("ui")});
    QCOMPARE(findPath(model, f.path(QStringLiteral("game/ui"))).data(ProjectsModel::CheckedFolderRole).toBool(), true);
}

void ProjectsModelTest::testRunnerWaitsForEachProject()
{
    Fixture f;
    QDir().mkpath(f.path(QStringLiteral("a")));
    QDir().mkpath(f.path(QStringLiteral("b")));
    HeldSaves host;
    host.modified.insert(f.path(QStringLiteral("a")), {f.path(QStringLiteral("a/x.kra"))});
    ProjectsSyncRunner runner(&f.driver, &host);
    QSignalSpy finished(&runner, &ProjectsSyncRunner::finished);

    runner.start({f.path(QStringLiteral("a")), f.path(QStringLiteral("b"))}, QStringLiteral("notes"));
    QVERIFY(runner.isRunning());
    QTest::qWait(50);
    // The first project is still saving its open document: the second has not started.
    QCOMPARE(host.held.size(), 1);
    QCOMPARE(f.driver.callCount(QStringLiteral("synchronise")), 0);

    host.held.takeFirst()(true);
    QVERIFY(finished.wait(5000));
    QVERIFY(!runner.isRunning());

    const QList<KisVersioningFakeDriver::Call> calls = f.driver.callsNamed(QStringLiteral("synchronise"));
    QCOMPARE(calls.size(), 2);
    QCOMPARE(calls.at(0).args, (QStringList{f.path(QStringLiteral("a")), QStringLiteral("notes")}));
    QCOMPARE(calls.at(1).args, (QStringList{f.path(QStringLiteral("b")), QStringLiteral("notes")}));

    const QVector<ProjectsSyncRunner::Result> results = runner.results();
    QCOMPARE(results.size(), 2);
    QCOMPARE(results.at(0).projectPath, f.path(QStringLiteral("a")));
    QCOMPARE(results.at(1).projectPath, f.path(QStringLiteral("b")));
    QCOMPARE(results.at(0).report.error, KisVersioningJob::NoError);
}

void ProjectsModelTest::testRunnerGoesOnAfterAFailure()
{
    Fixture f;
    QDir().mkpath(f.path(QStringLiteral("a")));
    QDir().mkpath(f.path(QStringLiteral("b")));
    HeldSaves host;
    f.driver.queueSyncError(KisVersioningJob::Offline, QStringLiteral("no network"));
    ProjectsSyncRunner runner(&f.driver, &host);
    QSignalSpy finished(&runner, &ProjectsSyncRunner::finished);
    QSignalSpy projectFinished(&runner, &ProjectsSyncRunner::projectFinished);

    runner.start({f.path(QStringLiteral("a")), f.path(QStringLiteral("b"))}, QString());
    QVERIFY(finished.wait(5000));
    QCOMPARE(finished.size(), 1);
    QCOMPARE(projectFinished.size(), 2);
    QCOMPARE(projectFinished.at(0).first().toInt(), 0);
    QCOMPARE(projectFinished.at(1).first().toInt(), 1);

    const QVector<ProjectsSyncRunner::Result> results = runner.results();
    QCOMPARE(results.size(), 2);
    QCOMPARE(results.at(0).report.error, KisVersioningJob::Offline);
    QVERIFY(results.at(0).report.pendingUpload);
    QCOMPARE(results.at(1).report.error, KisVersioningJob::NoError);

    // Nothing to synchronise still ends, once.
    runner.start({}, QString());
    QVERIFY(finished.wait(5000));
    QVERIFY(runner.results().isEmpty());
}

void ProjectsModelTest::testStaleDocumentsAreTheModifiedOnes()
{
    const QString project = QStringLiteral("/work/game");
    HeldSaves host;
    QStringList modified{project + QStringLiteral("/ui/received-modified.kra")};
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // The application may keep a path with other separators, and another case on these systems.
    modified << QDir::toNativeSeparators(QStringLiteral("/WORK/game/ui/kept-modified.kra"));
#else
    modified << QDir::toNativeSeparators(project + QStringLiteral("/ui/kept-modified.kra"));
#endif
    host.modified.insert(project, modified);
    host.openUnmodified << project + QStringLiteral("/ui/received-clean.kra")
                        << project + QStringLiteral("/ui/kept-clean.kra");

    KisVersioningSyncSession::Report report;
    report.received = QStringList{QStringLiteral("ui/received-modified.kra"), QStringLiteral("ui/received-clean.kra"),
                                  QStringLiteral("ui/received-not-open.kra")};
    report.keptBoth = QStringList{QStringLiteral("ui/kept-modified.kra"), QStringLiteral("ui/kept-clean.kra"),
                                  QStringLiteral("ui/kept-not-open.kra")};

    // Received and modified, kept both and modified: warned. Unmodified or not open: not warned.
    QCOMPARE(ProjectsSyncRunner::staleDocuments(project, report, host),
             (QStringList{QStringLiteral("ui/received-modified.kra"), QStringLiteral("ui/kept-modified.kra")}));

    host.modified.clear();
    QVERIFY(ProjectsSyncRunner::staleDocuments(project, report, host).isEmpty());

    // Saved by the artist while the other version took the file's place: the session says so.
    report.staleDocuments = QStringList{QStringLiteral("ui/kept-clean.kra")};
    QCOMPARE(ProjectsSyncRunner::staleDocuments(project, report, host), QStringList{QStringLiteral("ui/kept-clean.kra")});
}

void ProjectsModelTest::testRunnerReportsStaleDocuments()
{
    Fixture f;
    QDir().mkpath(f.path(QStringLiteral("a")));
    HeldSaves host;
    host.modified.insert(f.path(QStringLiteral("a")), {f.path(QStringLiteral("a/ui/botao.kra"))});
    SyncResult received;
    received.received = QStringList{QStringLiteral("ui/botao.kra")};
    f.driver.queueSyncResult(received);
    ProjectsSyncRunner runner(&f.driver, &host);
    QSignalSpy finished(&runner, &ProjectsSyncRunner::finished);

    runner.start({f.path(QStringLiteral("a"))}, QString());
    QTRY_COMPARE(host.held.size(), 1);
    // Saved before synchronising, then changed again while the other version arrived.
    host.held.takeFirst()(true);
    QVERIFY(finished.wait(5000));

    const ProjectsSyncRunner::Result result = runner.results().first();
    QCOMPARE(result.report.received, QStringList{QStringLiteral("ui/botao.kra")});
    QCOMPARE(result.staleDocuments, QStringList{QStringLiteral("ui/botao.kra")});
}

void ProjectsModelTest::testPluginRegistersThePanel()
{
    // The plugin is loaded from a folder that holds only it, as the application loads its plugins.
    // The folder is next to the test and replaced on every run: Windows keeps a loaded plugin locked.
    const QFileInfo pluginFile(QString::fromUtf8(PROJECTSDOCKER_PLUGIN_FILE));
    QVERIFY2(pluginFile.exists(), qPrintable(pluginFile.absoluteFilePath()));
    const QString pluginDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("projectsdocker-discovery"));
    QVERIFY(QDir().mkpath(pluginDir));
    const QString copy = QDir(pluginDir).filePath(pluginFile.fileName());
    QFile::remove(copy);
    QVERIFY(QFile::copy(pluginFile.absoluteFilePath(), copy));
    qputenv("KRITA_PLUGIN_PATH", QFile::encodeName(pluginDir));

    KoDockFactoryBase *factory = KoDockRegistry::instance()->value(QStringLiteral("ProjectsDocker"));
    QVERIFY(factory);
    QCOMPARE(factory->defaultDockPosition(), KoDockFactoryBase::DockRight);

    // No driver plugin is in that folder: the panel says so instead of showing projects.
    QScopedPointer<QDockWidget> dock(factory->createDockWidget());
    QVERIFY(dock);
    QCOMPARE(dock->objectName(), QStringLiteral("ProjectsDocker"));
    QCOMPARE(dock->windowTitle(), QStringLiteral("Projects"));
    qunsetenv("KRITA_PLUGIN_PATH");
}

void ProjectsModelTest::testTitleIsTranslated()
{
    // The catalogue built from po/pt_BR/geppetto.po, in the build tree.
    KLocalizedString::addDomainLocaleDir("geppetto", QStringLiteral(GEPPETTO_LOCALE_DIR));
    KLocalizedString::setLanguages({QStringLiteral("pt_BR")});
    QCOMPARE(i18n("Projects"), QStringLiteral("Projetos"));
    QCOMPARE(i18np("1 update", "%1 updates", 2), QString::fromUtf8("2 atualizações"));
    // Strings written as the panel writes them: with characters beyond ASCII, or in two pieces.
    QCOMPARE(i18n("%1 · signed in", QStringLiteral("Ana")), QString::fromUtf8("Ana · sessão ativa"));
    QCOMPARE(i18n("Add folder…"), QString::fromUtf8("Adicionar pasta…"));
    QCOMPARE(i18n("%1 is editing this file", QStringLiteral("Bruno")),
             QString::fromUtf8("Bruno está editando este arquivo"));
    QCOMPARE(i18n("%1 was replaced by the other version while you kept working. "
                  "Save your work under another name.",
                  QStringLiteral("botao.kra")),
             QString::fromUtf8("botao.kra foi trocado pela outra versão enquanto você continuava trabalhando. "
                               "Salve o seu trabalho com outro nome."));
}

SIMPLE_TEST_MAIN(ProjectsModelTest)
