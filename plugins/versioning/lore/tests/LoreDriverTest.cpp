/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreDriverTest.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTimer>

#include <KisVersioningConflictMarkers.h>
#include <KisVersioningDriverRegistry.h>
#include <KisVersioningSyncSession.h>

#include "LoreCall.h"
#include "LoreDriver.h"

using namespace KisVersioning;

namespace {

QStringList capturedLoreWarnings;
QtMessageHandler previousHandler = nullptr;

void captureLoreWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg && message.startsWith(QLatin1String("Lore:"))) {
        capturedLoreWarnings << message;
        return;
    }
    if (previousHandler) {
        previousHandler(type, context, message);
    }
}

/// No document is open: the session works on the files alone.
class NoOpenDocuments : public KisVersioningDocumentHost
{
public:
    QStringList modifiedOpenDocuments(const QString &) const override
    {
        return QStringList();
    }
    void saveDocument(const QString &, std::function<void(bool)> done) override
    {
        done(true);
    }
    bool isOpenAndUnmodified(const QString &) const override
    {
        return false;
    }
    void reloadDocument(const QString &) override
    {
    }
};

} // namespace

LoreDriverTest::LoreDriverTest()
{
    // Lore keeps its per-user configuration and instance registry here, not in the user profile.
    qputenv("LORE_GLOBAL_PATH", QFile::encodeName(QDir::toNativeSeparators(m_loreData.path())));
    qputenv("LORE_USE_SERVICE", "0");
    qputenv("LORE_SERVICE_SOCKET", "lore_service-geppetto-test-" + QByteArray::number(QCoreApplication::applicationPid()));
}

QString LoreDriverTest::server() const
{
    return qEnvironmentVariable("KRITA_LORE_TEST_SERVER").trimmed();
}

bool LoreDriverTest::backendAvailable(QString *reason) const
{
    if (server().isEmpty()) {
        *reason = QStringLiteral("KRITA_LORE_TEST_SERVER is not set: set it to the address of a Lore test server, "
                                 "for example lore://127.0.0.1:41337");
        return false;
    }
    if (!m_loreData.isValid()) {
        *reason = QStringLiteral("No temporary folder for Lore's per-user data");
        return false;
    }
    return true;
}

KisVersioningDriver *LoreDriverTest::createDriver(QObject *parent)
{
    return new LoreDriver(parent);
}

bool LoreDriverTest::createProject(KisVersioningDriver *driver, const QString &localPath)
{
    // Repository names are unique on the server, and its store outlives the test run.
    static int counter = 0;
    const QString name = QStringLiteral("Space-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++counter);
    QTemporaryDir seed;
    if (!seed.isValid()) {
        return false;
    }

    const QByteArray seedPath = QDir::fromNativeSeparators(seed.path()).toUtf8();
    const QByteArray url = (server() + QLatin1Char('/') + name).toUtf8();
    const QByteArray identity("A");
    lore_global_args_t globals;
    std::memset(&globals, 0, sizeof globals);
    globals.repository_path = toLore(seedPath);
    globals.working_directory = toLore(seedPath);
    globals.identity = toLore(identity);

    QString id;
    int status = -1;
    bool completed = false;
    QEventLoop loop;
    LoreCall call;
    connect(&call, &LoreCall::event, &loop, [&id](const LoreEvent &event) {
        if (event.tag == LORE_EVENT_REPOSITORY_CREATE) {
            id = event.fields.value(QStringLiteral("id")).toString();
        }
    });
    connect(&call, &LoreCall::completed, &loop, [&](int code, const QString &rawText) {
        status = code;
        completed = true;
        if (code != 0) {
            qWarning("creating the test project failed (%d): %s", code, qUtf8Printable(rawText));
        }
        loop.quit();
    });
    call.start(globals, [&url](const lore_global_args_t *g, lore_event_callback_config_t callback) {
        lore_repository_create_args_t args;
        std::memset(&args, 0, sizeof args);
        args.repository_url = toLore(url);
        lore_repository_create_async(g, &args, callback);
    });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    if (!completed) {
        loop.exec();
    }
    if (status != 0) {
        return false;
    }

    m_project.id = id;
    m_project.name = name;
    m_project.serverAddress = server();
    static_cast<LoreDriver *>(driver)->setIdentityForTesting(QStringLiteral("A"));
    const Outcome outcome = run(driver->downloadProject(m_project, localPath, QStringList()));
    if (!outcome.done || outcome.error != KisVersioningJob::NoError) {
        qWarning("downloading the test project failed: %s", qUtf8Printable(outcome.errorText));
        return false;
    }
    return true;
}

bool LoreDriverTest::cloneProject(KisVersioningDriver *driver, const QString &localPath)
{
    static_cast<LoreDriver *>(driver)->setIdentityForTesting(QStringLiteral("B"));
    const Outcome outcome = run(driver->downloadProject(m_project, localPath, QStringList()));
    return outcome.done && outcome.error == KisVersioningJob::NoError;
}

void LoreDriverTest::setOffline(KisVersioningDriver *driver, bool offline)
{
    static_cast<LoreDriver *>(driver)->setOfflineForTesting(offline);
}

void LoreDriverTest::testRegistryFindsThePlugin()
{
    // As in the local folder driver's test: a folder next to the test holding only this plugin.
    // lore.dll is found next to the test executable.
    const QFileInfo pluginFile(QString::fromUtf8(LORE_PLUGIN_FILE));
    QVERIFY2(pluginFile.exists(), qPrintable(pluginFile.absoluteFilePath()));
    const QString pluginDir = QDir(QCoreApplication::applicationDirPath()).filePath("lore-discovery");
    QVERIFY(QDir().mkpath(pluginDir));
    const QString copy = QDir(pluginDir).filePath(pluginFile.fileName());
    QFile::remove(copy);
    QVERIFY(QFile::copy(pluginFile.absoluteFilePath(), copy));
    qputenv("KRITA_PLUGIN_PATH", QFile::encodeName(pluginDir));

    KisVersioningDriverRegistry *registry = KisVersioningDriverRegistry::instance();
    QVERIFY(registry->contains("lore"));
    KisVersioningDriverFactory *factory = registry->get("lore");
    QVERIFY(factory);
    QCOMPARE(factory->id(), QString("lore"));
    QVERIFY(!factory->name().isEmpty());

    QScopedPointer<KisVersioningDriver> driver(factory->create(nullptr));
    QVERIFY(driver);
    QCOMPARE(driver->id(), QString("lore"));
    qunsetenv("KRITA_PLUGIN_PATH");
}

void LoreDriverTest::testDeclaresItsCapabilities()
{
    LoreDriver driver;
    QCOMPARE(driver.id(), QString("lore"));
    QCOMPARE(driver.capabilities(), Capabilities(SignIn | RemoteProjects | PartialDownload | Presence | Notes));
    // Nobody signed in yet.
    QVERIFY(driver.currentUser().isEmpty());
    driver.setIdentityForTesting(QStringLiteral("A"));
    QCOMPARE(driver.currentUser(), QString("A"));
}

void LoreDriverTest::testExpiredSessionIsNotSignedIn()
{
    const QString authServer = qEnvironmentVariable("KRITA_LORE_TEST_AUTH_SERVER").trimmed();
    if (authServer.isEmpty()) {
        QSKIP("Needs a Lore server with authentication: set KRITA_LORE_TEST_AUTH_SERVER to its address");
    }
    // Lore's per-user data is a fresh temporary folder, so no session is stored: the call meets
    // the server with none, as after the session expired.
    LoreDriver driver;
    const Outcome outcome = run(driver.listProjects(authServer));
    QVERIFY2(outcome.done, "the job did not finish in time");
    QCOMPARE(outcome.error, KisVersioningJob::NotSignedIn);
    QVERIFY(!outcome.errorText.isEmpty());
}

void LoreDriverTest::testRawLoreErrorNeverReachesErrorText()
{
    REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ProjectInfo missing;
    missing.name = QStringLiteral("Space-missing-%1").arg(QDateTime::currentMSecsSinceEpoch());
    missing.serverAddress = server();

    capturedLoreWarnings.clear();
    previousHandler = qInstallMessageHandler(captureLoreWarnings);
    LoreDriver driver;
    const Outcome outcome = run(driver.downloadProject(missing, dir.filePath("project"), QStringList()));
    qInstallMessageHandler(previousHandler);

    QVERIFY2(outcome.done, "the job did not finish in time");
    QVERIFY(outcome.error != KisVersioningJob::NoError);
    QVERIFY(!outcome.errorText.isEmpty());

    // The library's own text went to the log, and none of it reached the artist.
    QVERIFY(!capturedLoreWarnings.isEmpty());
    for (const QString &warning : std::as_const(capturedLoreWarnings)) {
        const int start = warning.indexOf(QLatin1String("): "));
        QVERIFY2(start > 0, qPrintable(warning));
        const QString raw = warning.mid(start + 3).trimmed();
        QVERIFY2(!raw.isEmpty(), qPrintable(warning));
        QVERIFY2(!outcome.errorText.contains(raw), qPrintable(outcome.errorText));
    }
    QVERIFY2(!outcome.errorText.contains(QLatin1String("Repository"), Qt::CaseInsensitive), qPrintable(outcome.errorText));
    // Nothing was left behind for the project.
    QVERIFY(!QFileInfo::exists(dir.filePath("project")));
}

void LoreDriverTest::testLargeFileReportsProgress()
{
    REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LoreDriver driver;
    const QString project = dir.filePath("project");
    QVERIFY(createProject(&driver, project));

    // 64 MiB that do not compress.
    QByteArray bytes(64 << 20, Qt::Uninitialized);
    quint32 x = 2463534242u;
    for (int i = 0; i < bytes.size(); ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        bytes[i] = char(x);
    }
    QVERIFY(writeFile(project, "large.kra", bytes));

    KisVersioningJob *job = driver.synchronise(project, "large file");
    QSignalSpy progress(job, &KisVersioningJob::progress);
    const Outcome outcome = run(job, 300000);
    VERIFY_OK(outcome);
    QVERIFY(outcome.result.value<SyncResult>().sent.contains("large.kra"));

    int withSize = 0;
    for (const QList<QVariant> &signal : std::as_const(progress)) {
        QVERIFY(!signal.at(2).toString().isEmpty());
        if (signal.at(1).toLongLong() > 0) {
            ++withSize;
        }
    }
    QVERIFY2(withSize >= 2, qPrintable(QString("%1 progress signals, %2 with a size").arg(progress.size()).arg(withSize)));
}

void LoreDriverTest::testSessionKeepsBothVersionsOfALineMergedFile()
{
    REQUIRE_BACKEND();
    // A text file each person changed in a different line: the library would merge the lines by
    // itself. The session must instead keep both versions, as for any other file.
    const QByteArray base("line 1\nline 2\nline 3\nline 4\nline 5\nline 6\n");
    const QByteArray first("line 1 by the first\nline 2\nline 3\nline 4\nline 5\nline 6\n");
    const QByteArray second("line 1\nline 2\nline 3\nline 4\nline 5\nline 6 by the second\n");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Pair pair;
    QVERIFY2(setUpPair(dir.path(), &pair, {{"notes/ideia.txt", base}}), "could not set up two copies of the project");

    QVERIFY(writeFile(pair.pathA, "notes/ideia.txt", first));
    QVERIFY(writeFile(pair.pathB, "notes/ideia.txt", second));
    VERIFY_OK(run(pair.a->synchronise(pair.pathA, "first")));

    NoOpenDocuments host;
    KisVersioningSyncSession session(pair.b.data(), &host);
    QSignalSpy finished(&session, &KisVersioningSyncSession::finished);
    session.start(pair.pathB, "second");
    QVERIFY(finished.wait(120000));
    const KisVersioningSyncSession::Report report = finished.at(0).at(0).value<KisVersioningSyncSession::Report>();
    QVERIFY2(report.error == KisVersioningJob::NoError, qPrintable(report.errorText));
    QCOMPARE(report.keptBoth, QStringList({"notes/ideia.txt"}));
    QCOMPARE(report.conflictCopies.size(), 1);
    QVERIFY(!report.pendingUpload);

    // The server's version at the path, the second person's beside it, both byte for byte.
    const QString copy = report.conflictCopies.at(0);
    QCOMPARE(readFile(QDir(pair.pathB).filePath("notes/ideia.txt")), first);
    QCOMPARE(readFile(QDir(pair.pathB).filePath(copy)), second);

    // The first person receives both, and the conflict marker.
    VERIFY_OK(run(pair.a->synchronise(pair.pathA, QString())));
    QCOMPARE(readFile(QDir(pair.pathA).filePath("notes/ideia.txt")), first);
    QCOMPARE(readFile(QDir(pair.pathA).filePath(copy)), second);
    QCOMPARE(KisVersioningConflictMarkers(pair.pathA).list().size(), 1);
}

void LoreDriverTest::testListsProjectsAndFoldersWithoutDownloading()
{
    REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LoreDriver driver;
    const QString project = dir.filePath("project");
    QVERIFY(createProject(&driver, project));

    // A project with nothing in it yet has no folders.
    const Outcome empty = run(driver.listFolders(m_project));
    VERIFY_OK(empty);
    QVERIFY(empty.result.toStringList().isEmpty());

    QVERIFY(writeFile(project, "ui/botao.kra", "ui"));
    QVERIFY(writeFile(project, "concepts/esboco.kra", "concepts"));
    QVERIFY(writeFile(project, "raiz.kra", "root"));
    VERIFY_OK(run(driver.synchronise(project, "folders")));

    const Outcome listed = run(driver.listProjects(server()));
    VERIFY_OK(listed);
    bool found = false;
    for (const ProjectInfo &info : listed.result.value<QVector<ProjectInfo>>()) {
        if (info.id == m_project.id) {
            found = true;
            QCOMPARE(info.name, m_project.name);
            QCOMPARE(info.serverAddress, server());
        }
    }
    QVERIFY(found);

    LoreDriver other;
    const Outcome folders = run(other.listFolders(m_project));
    VERIFY_OK(folders);
    QCOMPARE(folders.result.toStringList(), QStringList({"concepts", "ui"}));
}

void LoreDriverTest::testSignInWithoutAuthenticationUsesTheSystemUser()
{
    REQUIRE_BACKEND();
    // The test server has no authentication: signing in succeeds and the system user is who
    // this is. No browser is involved.
    LoreDriver driver;
    QVERIFY(driver.currentUser().isEmpty());
    VERIFY_OK(run(driver.signIn(server())));
    QVERIFY(!driver.currentUser().isEmpty());
    VERIFY_OK(run(driver.signOut()));
    QVERIFY(driver.currentUser().isEmpty());

    const Outcome nowhere = run(driver.signIn(QString()));
    QVERIFY(nowhere.done);
    QCOMPARE(nowhere.error, KisVersioningJob::Failed);
}

void LoreDriverTest::testResolveMineKeepsMyBytes_data()
{
    addDivergenceContents();
}

void LoreDriverTest::testResolveMineKeepsMyBytes()
{
    REQUIRE_BACKEND();
    QFETCH(QByteArray, base);
    QFETCH(QByteArray, first);
    QFETCH(QByteArray, second);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Pair pair;
    QVERIFY2(setUpPair(dir.path(), &pair, {{"a.kra", base}}), "could not set up two copies of the project");

    QVERIFY(writeFile(pair.pathA, "a.kra", first));
    QVERIFY(writeFile(pair.pathB, "a.kra", second));
    VERIFY_OK(run(pair.a->synchronise(pair.pathA, "first")));
    const Outcome diverged = run(pair.b->synchronise(pair.pathB, "second"));
    VERIFY_OK(diverged);
    QCOMPARE(diverged.result.value<SyncResult>().divergences.size(), 1);

    // Keeping my version: the working file stays as it is and is what goes out.
    VERIFY_OK(run(pair.b->resolve(pair.pathB, "a.kra", Resolution::Mine, QString())));
    QCOMPARE(readFile(QDir(pair.pathB).filePath("a.kra")), second);
    const Outcome accepted = run(pair.b->synchronise(pair.pathB, "kept mine"));
    VERIFY_OK(accepted);
    QVERIFY(accepted.result.value<SyncResult>().divergences.isEmpty());
    QVERIFY(!accepted.result.value<SyncResult>().pendingUpload);

    VERIFY_OK(run(pair.a->synchronise(pair.pathA, QString())));
    QCOMPARE(readFile(QDir(pair.pathA).filePath("a.kra")), second);
}

void LoreDriverTest::testUnsettledDivergenceIsReportedAgain_data()
{
    addDivergenceContents();
}

void LoreDriverTest::testUnsettledDivergenceIsReportedAgain()
{
    REQUIRE_BACKEND();
    QFETCH(QByteArray, base);
    QFETCH(QByteArray, first);
    QFETCH(QByteArray, second);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Pair pair;
    QVERIFY2(setUpPair(dir.path(), &pair, {{"a.kra", base}}), "could not set up two copies of the project");

    QVERIFY(writeFile(pair.pathA, "a.kra", first));
    QVERIFY(writeFile(pair.pathB, "a.kra", second));
    VERIFY_OK(run(pair.a->synchronise(pair.pathA, "first")));
    const Outcome diverged = run(pair.b->synchronise(pair.pathB, "second"));
    VERIFY_OK(diverged);
    QCOMPARE(diverged.result.value<SyncResult>().divergences.size(), 1);

    // The application stopped before the divergence was settled: a new driver, with nothing
    // remembered, finds it again and sends nothing.
    LoreDriver restarted;
    restarted.setIdentityForTesting(QStringLiteral("B"));
    const Outcome again = run(restarted.synchronise(pair.pathB, "second, again"));
    VERIFY_OK(again);
    const SyncResult result = again.result.value<SyncResult>();
    QCOMPARE(result.divergences.size(), 1);
    QCOMPARE(result.divergences[0].path, QString("a.kra"));
    QVERIFY(result.divergences[0].kind == DivergenceKind::BothModified);
    QVERIFY(result.sent.isEmpty());
    QCOMPARE(readFile(QDir(pair.pathB).filePath("a.kra")), second);

    VERIFY_OK(run(pair.a->synchronise(pair.pathA, QString())));
    QCOMPARE(readFile(QDir(pair.pathA).filePath("a.kra")), first);

    // Settled now, it goes out as for any divergence.
    VERIFY_OK(run(restarted.resolve(pair.pathB, "a.kra", Resolution::Theirs, QString())));
    QCOMPARE(readFile(QDir(pair.pathB).filePath("a.kra")), first);
    const Outcome accepted = run(restarted.synchronise(pair.pathB, "settled"));
    VERIFY_OK(accepted);
    QVERIFY(accepted.result.value<SyncResult>().divergences.isEmpty());
}

SIMPLE_TEST_MAIN(LoreDriverTest)
