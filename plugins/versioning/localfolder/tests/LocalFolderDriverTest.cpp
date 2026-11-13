/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LocalFolderDriverTest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <KisVersioningDriverRegistry.h>

#include "LocalFolderDriver.h"

using namespace KisVersioning;

bool LocalFolderDriverTest::backendAvailable(QString *) const
{
    return true;
}

KisVersioningDriver *LocalFolderDriverTest::createDriver(QObject *parent)
{
    return new LocalFolderDriver(parent);
}

bool LocalFolderDriverTest::createProject(KisVersioningDriver *driver, const QString &localPath)
{
    // A project is an existing folder.
    if (!QDir().mkpath(localPath)) {
        return false;
    }
    const Outcome outcome = run(driver->downloadProject(ProjectInfo(), localPath, QStringList()));
    return outcome.done && outcome.error == KisVersioningJob::NoError;
}

bool LocalFolderDriverTest::cloneProject(KisVersioningDriver *, const QString &)
{
    return false; // no RemoteProjects, so the conformance tests never ask
}

void LocalFolderDriverTest::setOffline(KisVersioningDriver *, bool)
{
}

void LocalFolderDriverTest::testRegistryFindsThePlugin()
{
    // Plugin discovery looks in KRITA_PLUGIN_PATH first. Point it at a folder holding only the
    // plugin of this test's build tree, before anything has asked the plugin loader. The folder is
    // next to the test and replaced on every run: the loaded plugin keeps its file locked on
    // Windows, so a temporary folder could not be removed.
    const QFileInfo pluginFile(QString::fromUtf8(LOCALFOLDER_PLUGIN_FILE));
    QVERIFY2(pluginFile.exists(), qPrintable(pluginFile.absoluteFilePath()));
    const QString pluginDir = QDir(QCoreApplication::applicationDirPath()).filePath("localfolder-discovery");
    QVERIFY(QDir().mkpath(pluginDir));
    const QString copy = QDir(pluginDir).filePath(pluginFile.fileName());
    QFile::remove(copy);
    QVERIFY(QFile::copy(pluginFile.absoluteFilePath(), copy));
    qputenv("KRITA_PLUGIN_PATH", QFile::encodeName(pluginDir));

    KisVersioningDriverRegistry *registry = KisVersioningDriverRegistry::instance();
    QVERIFY(registry->contains("localfolder"));
    KisVersioningDriverFactory *factory = registry->get("localfolder");
    QVERIFY(factory);
    QCOMPARE(factory->id(), QString("localfolder"));
    QVERIFY(!factory->name().isEmpty());

    QScopedPointer<KisVersioningDriver> driver(factory->create(nullptr));
    QVERIFY(driver);
    QCOMPARE(driver->id(), QString("localfolder"));
    qunsetenv("KRITA_PLUGIN_PATH");
}

void LocalFolderDriverTest::testDeclaresOnlyNotes()
{
    LocalFolderDriver driver;
    QCOMPARE(driver.capabilities(), Capabilities(Notes));
    QVERIFY(!driver.currentUser().isEmpty());
}

void LocalFolderDriverTest::testDownloadCreatesVersionsFolder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalFolderDriver driver;
    QVERIFY(createProject(&driver, dir.filePath("project")));
    QVERIFY(QFileInfo(dir.filePath("project/.geppetto/versions")).isDir());

    // A folder that is not there cannot be a project.
    const Outcome missing = run(driver.downloadProject(ProjectInfo(), dir.filePath("nowhere"), QStringList()));
    QVERIFY(missing.done);
    QCOMPARE(missing.error, KisVersioningJob::Failed);
    QVERIFY(!QFileInfo::exists(dir.filePath("nowhere")));
}

void LocalFolderDriverTest::testVersionsStoreIsNotVersionedButConflictMarkersAre()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalFolderDriver driver;
    const QString project = dir.filePath("project");
    QVERIFY(createProject(&driver, project));

    QVERIFY(writeFile(project, "a.kra", "a"));
    QVERIFY(writeFile(project, ".geppetto/conflicts/ab12.json", "{}"));
    const Outcome first = run(driver.synchronise(project, "one"));
    VERIFY_OK(first);
    const SyncResult sent = first.result.value<SyncResult>();
    QVERIFY(sent.sent.contains("a.kra"));
    QVERIFY(sent.sent.contains(".geppetto/conflicts/ab12.json"));
    for (const QString &path : sent.sent) {
        QVERIFY2(!path.startsWith(".geppetto/versions"), qPrintable(path));
    }

    // The copies the driver made are not changes.
    const Outcome second = run(driver.synchronise(project, "two"));
    VERIFY_OK(second);
    QVERIFY(second.result.value<SyncResult>().sent.isEmpty());
}

void LocalFolderDriverTest::testDeletedFileGetsAVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalFolderDriver driver;
    const QString project = dir.filePath("project");
    QVERIFY(createProject(&driver, project));

    QVERIFY(writeFile(project, "a.kra", "a"));
    QVERIFY(writeFile(project, "b.kra", "b"));
    VERIFY_OK(run(driver.synchronise(project, "base")));
    QVERIFY(QFile::remove(QDir(project).filePath("a.kra")));
    const Outcome sync = run(driver.synchronise(project, "removed a"));
    VERIFY_OK(sync);
    QVERIFY(sync.result.value<SyncResult>().sent.contains("a.kra"));

    const QVector<VersionInfo> history = historyOf(&driver, project, "a.kra");
    QCOMPARE(history.size(), 2);
    QCOMPARE(history[1].number, 2);
    QCOMPARE(history[1].notes, QString("removed a"));
    QCOMPARE(historyOf(&driver, project, "b.kra").size(), 1);
}

void LocalFolderDriverTest::testFetchOfDeletedFileFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalFolderDriver driver;
    const QString project = dir.filePath("project");
    QVERIFY(createProject(&driver, project));

    QVERIFY(writeFile(project, "a.kra", "a"));
    VERIFY_OK(run(driver.synchronise(project, "base")));
    QVERIFY(QFile::remove(QDir(project).filePath("a.kra")));
    VERIFY_OK(run(driver.synchronise(project, "removed")));

    const Outcome fetched = run(driver.fetchFileAtVersion(project, "a.kra", "2", dir.filePath("out.kra")));
    QVERIFY(fetched.done);
    QCOMPARE(fetched.error, KisVersioningJob::Failed);
    QVERIFY(!QFileInfo::exists(dir.filePath("out.kra")));

    // A version that does not exist fails too.
    const Outcome unknown = run(driver.fetchFileAtVersion(project, "a.kra", "99", dir.filePath("out.kra")));
    QVERIFY(unknown.done);
    QCOMPARE(unknown.error, KisVersioningJob::Failed);
}

void LocalFolderDriverTest::testUnsupportedOperationsFail()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalFolderDriver driver;
    const QString project = dir.filePath("project");
    QVERIFY(createProject(&driver, project));

    const QVector<Outcome> outcomes = {
        run(driver.signIn("anywhere")),
        run(driver.signOut()),
        run(driver.listFolders(ProjectInfo())),
        run(driver.setFolders(project, {"ui"})),
        run(driver.resolve(project, "a.kra", Resolution::Mine, QString())),
        run(driver.announceEditing(project, "a.kra", true)),
        run(driver.whoIsEditing(project, {"a.kra"})),
    };
    for (const Outcome &outcome : outcomes) {
        QVERIFY(outcome.done);
        QCOMPARE(outcome.error, KisVersioningJob::Failed);
        QVERIFY(!outcome.errorText.isEmpty());
    }
}

void LocalFolderDriverTest::testListProjectsIsEmpty()
{
    LocalFolderDriver driver;
    const Outcome outcome = run(driver.listProjects(QString()));
    VERIFY_OK(outcome);
    QVERIFY(outcome.result.value<QVector<ProjectInfo>>().isEmpty());
}

void LocalFolderDriverTest::testDriverMayBeDestroyedWhileWorking()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto *driver = new LocalFolderDriver;
    const QString project = dir.filePath("project");
    QVERIFY(createProject(driver, project));
    for (int i = 0; i < 20; ++i) {
        QVERIFY(writeFile(project, QString("file %1.kra").arg(i), QByteArray(200000, char('a' + i))));
    }

    // The job outlives its driver and still ends, once.
    KisVersioningJob *job = driver->synchronise(project, "many files");
    QSignalSpy finished(job, &KisVersioningJob::finished);
    delete driver;
    QVERIFY(finished.wait(30000));
    QCOMPARE(finished.count(), 1);

    LocalFolderDriver another;
    QCOMPARE(historyOf(&another, project, "file 0.kra").size(), 1);
}

SIMPLE_TEST_MAIN(LocalFolderDriverTest)
