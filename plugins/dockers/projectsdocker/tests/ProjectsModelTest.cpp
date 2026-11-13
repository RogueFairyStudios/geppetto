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

#include <functional>

#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KoDockFactoryBase.h>
#include <KoDockRegistry.h>

#include <KisVersioningConflictMarkers.h>
#include <KisVersioningSettings.h>
#include <KisVersioningSyncSession.h>

#include "KisVersioningFakeDriver.h"
#include "ProjectsDocumentHost.h"
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
    QList<std::function<void(bool)>> held;

    QStringList modifiedOpenDocuments(const QString &projectPath) const override
    {
        return modified.value(projectPath);
    }
    void saveDocument(const QString &, std::function<void(bool)> done) override { held.append(done); }
    bool isOpenAndUnmodified(const QString &) const override { return false; }
    void reloadDocument(const QString &) override {}
};

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
    f.driver.setCapabilities(RemoteProjects | PartialDownload);
    f.write(QStringLiteral("game/ui/botao.kra"));
    f.write(QStringLiteral("game/art/heroi.kra"));
    f.addProject(QStringLiteral("game"), {QStringLiteral("ui"), QStringLiteral("art")});
    f.driver.setProjects({ProjectInfo{QStringLiteral("game"), QStringLiteral("game"), QString()}});
    f.driver.setProjectFolders(QStringLiteral("game"), {QStringLiteral("art"), QStringLiteral("ui")});
    f.driver.setStatus(statusWith(QStringLiteral("ui/botao.kra"), FileState::Modified));
    ProjectsModel model(&f.driver, &f.settings);
    QVERIFY(refreshAndWait(model));

    QSignalSpy failures(&model, &ProjectsModel::operationFailed);
    QVERIFY(!model.setData(findPath(model, f.path(QStringLiteral("game/ui"))), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(failures.size(), 1);
    QCOMPARE(f.driver.callCount(QStringLiteral("setFolders")), 0);
    QCOMPARE(f.settings.projects().first().folders, (QStringList{QStringLiteral("ui"), QStringLiteral("art")}));

    // A version that was not sent yet holds work too.
    ProjectStatus pending;
    pending.pendingUpload = 1;
    f.driver.setStatus(pending);
    QVERIFY(refreshAndWait(model));
    QVERIFY(!model.setData(findPath(model, f.path(QStringLiteral("game/art"))), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(failures.size(), 2);
    QCOMPARE(f.driver.callCount(QStringLiteral("setFolders")), 0);
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

void ProjectsModelTest::testPathKeyIgnoresSeparatorsAndCase()
{
    const QString path = QStringLiteral("C:/Work/Game/ui/botao.kra");
    QCOMPARE(ProjectsDocumentHost::pathKey(QDir::toNativeSeparators(path)), ProjectsDocumentHost::pathKey(path));
    QCOMPARE(ProjectsDocumentHost::pathKey(QStringLiteral("C:/Work/Game/art/../ui/botao.kra")),
             ProjectsDocumentHost::pathKey(path));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    QCOMPARE(ProjectsDocumentHost::pathKey(QStringLiteral("c:/work/GAME/ui/Botao.kra")),
             ProjectsDocumentHost::pathKey(path));
#endif
    QVERIFY(ProjectsDocumentHost::isInside(path, QStringLiteral("C:/Work/Game")));
    QVERIFY(ProjectsDocumentHost::isInside(path, QStringLiteral("C:/Work/Game/")));
    QVERIFY(ProjectsDocumentHost::isInside(QDir::toNativeSeparators(path), QStringLiteral("C:/Work/Game")));
    QVERIFY(!ProjectsDocumentHost::isInside(QStringLiteral("C:/Work/Game2/ui/botao.kra"), QStringLiteral("C:/Work/Game")));
    QVERIFY(!ProjectsDocumentHost::isInside(QString(), QStringLiteral("C:/Work/Game")));
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
    QCOMPARE(i18n("%1 was replaced by the other version while you kept working. "
                  "Save your work under another name.",
                  QStringLiteral("botao.kra")),
             QString::fromUtf8("botao.kra foi trocado pela outra versão enquanto você continuava trabalhando. "
                               "Salve o seu trabalho com outro nome."));
}

SIMPLE_TEST_MAIN(ProjectsModelTest)
