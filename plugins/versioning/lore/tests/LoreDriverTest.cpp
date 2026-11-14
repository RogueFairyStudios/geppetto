/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreDriverTest.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTimer>
#include <QUrl>

#include <KisVersioningConflictMarkers.h>
#include <KisVersioningDriverRegistry.h>
#include <KisVersioningSyncSession.h>

#include "LoreCall.h"
#include "LoreDriver.h"
#include "LoreOperation.h"

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
    bool savedSince(const QString &, const QDateTime &) const override
    {
        return false;
    }
};

} // namespace

LoreDriverTest::LoreDriverTest()
{
    // Lore keeps its per-user configuration and instance registry here, not in the user profile.
    qputenv("LORE_GLOBAL_PATH", QFile::encodeName(QDir::toNativeSeparators(m_loreData.path())));
    qputenv("LORE_USE_SERVICE", "0");
    const QByteArray socket = QByteArray("lore_service-geppetto-test-") + QByteArray::number(QCoreApplication::applicationPid());
    qputenv("LORE_SERVICE_SOCKET", socket);
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

void LoreDriverTest::testSessionIsResumedWithoutTheArtist()
{
    REQUIRE_BACKEND();
    // The test server has no authentication: the session is the system user's, found with no
    // browser and no sign-in of the artist.
    LoreDriver driver;
    QVERIFY(driver.currentUser().isEmpty());
    const Outcome resumed = run(driver.resumeSession(server()));
    VERIFY_OK(resumed);
    QVERIFY(!resumed.result.toString().isEmpty());
    QCOMPARE(driver.currentUser(), resumed.result.toString());

    // With no network nobody can be told: the driver says so, and does not claim anyone.
    LoreDriver offline;
    offline.setOfflineForTesting(true);
    const Outcome unknown = run(offline.resumeSession(server()));
    QVERIFY2(unknown.done, "the job did not finish in time");
    QCOMPARE(unknown.error, KisVersioningJob::Offline);
    QVERIFY(offline.currentUser().isEmpty());
}

void LoreDriverTest::testOnlySafeSignInAddressesAreOpened()
{
    QVERIFY(loreSignInAddressIsSafe(QUrl(QStringLiteral("https://login.example/device?code=1"))));
    QVERIFY(loreSignInAddressIsSafe(QUrl(QStringLiteral("http://localhost:8080/callback"))));
    QVERIFY(loreSignInAddressIsSafe(QUrl(QStringLiteral("http://LOCALHOST/"))));
    QVERIFY(loreSignInAddressIsSafe(QUrl(QStringLiteral("http://127.0.0.1:9000/"))));
    QVERIFY(loreSignInAddressIsSafe(QUrl(QStringLiteral("http://127.5.6.7/"))));
    QVERIFY(loreSignInAddressIsSafe(QUrl(QStringLiteral("http://[::1]:9000/"))));

    QVERIFY(!loreSignInAddressIsSafe(QUrl(QStringLiteral("http://login.example/"))));
    QVERIFY(!loreSignInAddressIsSafe(QUrl(QStringLiteral("http://127.example/"))));
    QVERIFY(!loreSignInAddressIsSafe(QUrl(QStringLiteral("http://localhost.example/"))));
    QVERIFY(!loreSignInAddressIsSafe(QUrl(QStringLiteral("file:///C:/Windows/System32/calc.exe"))));
    QVERIFY(!loreSignInAddressIsSafe(QUrl(QStringLiteral("javascript:alert(1)"))));
    QVERIFY(!loreSignInAddressIsSafe(QUrl(QStringLiteral("ftp://login.example/"))));
    QVERIFY(!loreSignInAddressIsSafe(QUrl(QStringLiteral("https:///nohost"))));
    QVERIFY(!loreSignInAddressIsSafe(QUrl()));
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

void LoreDriverTest::testPreparedButNotCreatedVersionIsSentNext_data()
{
    QTest::addColumn<bool>("serverAhead");
    QTest::newRow("server not ahead") << false;
    QTest::newRow("server ahead") << true;
}

void LoreDriverTest::testPreparedButNotCreatedVersionIsSentNext()
{
    REQUIRE_BACKEND();
    QFETCH(bool, serverAhead);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Pair pair;
    QVERIFY2(setUpPair(dir.path(), &pair, {{"base.kra", "base"}}), "could not set up two copies of the project");

    // Cancelled once the change is being prepared: it is prepared, but no version is created. The
    // library then reports the file as prepared and no longer as changed on disk.
    QVERIFY(writeFile(pair.pathA, "base.kra", "changed, prepared, not a version yet"));
    KisVersioningJob *job = pair.a->synchronise(pair.pathA, "first");
    connect(job, &KisVersioningJob::progress, job, [job](qint64, qint64, const QString &label) {
        if (label == QLatin1String("Preparing your files")) {
            job->requestCancel();
        }
    });
    const Outcome cancelled = run(job);
    QVERIFY2(cancelled.done, "the job did not finish in time");
    QCOMPARE(cancelled.error, KisVersioningJob::Cancelled);
    // Still a change of this computer, in no version.
    const Outcome state = run(pair.a->status(pair.pathA));
    VERIFY_OK(state);
    QMap<QString, FileState> states;
    for (const FileStatus &file : state.result.value<ProjectStatus>().files) {
        states.insert(file.path, file.state);
    }
    QVERIFY(states.value("base.kra", FileState::Unchanged) == FileState::Modified);
    QCOMPARE(historyOf(pair.a.data(), pair.pathA, "base.kra").size(), 1);

    if (serverAhead) {
        QVERIFY(writeFile(pair.pathB, "outro.kra", "from the second"));
        VERIFY_OK(run(pair.b->synchronise(pair.pathB, "second")));
    }

    // The next synchronisation creates the version of what was prepared, and sends it.
    const Outcome again = run(pair.a->synchronise(pair.pathA, QString()));
    VERIFY_OK(again);
    const SyncResult result = again.result.value<SyncResult>();
    QVERIFY(result.divergences.isEmpty());
    QVERIFY(result.sent.contains("base.kra"));
    QVERIFY(!result.pendingUpload);
    if (serverAhead) {
        QVERIFY(result.received.contains("outro.kra"));
        QCOMPARE(readFile(QDir(pair.pathA).filePath("outro.kra")), QByteArray("from the second"));
    }

    VERIFY_OK(run(pair.b->synchronise(pair.pathB, QString())));
    QCOMPARE(readFile(QDir(pair.pathB).filePath("base.kra")), QByteArray("changed, prepared, not a version yet"));
}

void LoreDriverTest::testSetFoldersRefusesLocalChanges()
{
    REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Pair pair;
    QVERIFY2(setUpPair(dir.path(), &pair, {{"ui/a.kra", "ui base"}, {"concepts/b.kra", "concepts base"}}),
             "could not set up two copies of the project");

    QVERIFY(writeFile(pair.pathB, "ui/a.kra", "changed here"));
    const Outcome refused = run(pair.b->setFolders(pair.pathB, {"ui"}));
    QVERIFY2(refused.done, "the job did not finish in time");
    QCOMPARE(refused.error, KisVersioningJob::Failed);
    // Refused by the driver itself, before any call that receives.
    QCOMPARE(refused.errorText, QString("Synchronize the project before changing its folders."));

    // Nothing changed on disk, and no version was created.
    QCOMPARE(readFile(QDir(pair.pathB).filePath("ui/a.kra")), QByteArray("changed here"));
    QCOMPARE(readFile(QDir(pair.pathB).filePath("concepts/b.kra")), QByteArray("concepts base"));
    QCOMPARE(historyOf(pair.b.data(), pair.pathB, "ui/a.kra").size(), 1);
    const Outcome state = run(pair.b->status(pair.pathB));
    VERIFY_OK(state);
    bool modified = false;
    for (const FileStatus &file : state.result.value<ProjectStatus>().files) {
        modified = modified || (file.path == QLatin1String("ui/a.kra") && file.state == FileState::Modified);
    }
    QVERIFY(modified);
    QCOMPARE(state.result.value<ProjectStatus>().pendingUpload, 0);
}

void LoreDriverTest::testSetFoldersRefusesUnsentVersion_data()
{
    QTest::addColumn<QStringList>("folders");
    // The folders the second person already has: the library's sync then receives with a merge.
    QTest::newRow("same folders") << QStringList({"concepts", "ui"});
    QTest::newRow("fewer folders") << QStringList({"ui"});
}

void LoreDriverTest::testSetFoldersRefusesUnsentVersion()
{
    REQUIRE_BACKEND();
    QFETCH(QStringList, folders);
    const QByteArray base("line 1\nline 2\nline 3\nline 4\nline 5\nline 6\n");
    const QByteArray first("line 1 by the first\nline 2\nline 3\nline 4\nline 5\nline 6\n");
    const QByteArray second("line 1\nline 2\nline 3\nline 4\nline 5\nline 6 by the second\n");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Pair pair;
    QVERIFY2(setUpPair(dir.path(), &pair, {{"ui/notas.txt", base}, {"concepts/b.kra", "concepts base"}}),
             "could not set up two copies of the project");

    // On a clean project, up to date, choosing folders works.
    VERIFY_OK(run(pair.b->setFolders(pair.pathB, {"concepts", "ui"})));

    // The first person sends a change; the second makes a version of another line with no network.
    QVERIFY(writeFile(pair.pathA, "ui/notas.txt", first));
    VERIFY_OK(run(pair.a->synchronise(pair.pathA, "first")));
    setOffline(pair.b.data(), true);
    QVERIFY(writeFile(pair.pathB, "ui/notas.txt", second));
    const Outcome offline = run(pair.b->synchronise(pair.pathB, "second, offline"));
    VERIFY_OK(offline);
    QVERIFY(offline.result.value<SyncResult>().pendingUpload);
    setOffline(pair.b.data(), false);

    // Choosing folders would receive the first person's change over the unsent version.
    const Outcome refused = run(pair.b->setFolders(pair.pathB, folders));
    QVERIFY2(refused.done, "the job did not finish in time");
    QCOMPARE(refused.error, KisVersioningJob::Failed);
    // Refused by the driver itself, before any call that receives.
    QCOMPARE(refused.errorText, QString("Synchronize the project before changing its folders."));

    // The unsent version is still there, not merged with anything.
    QCOMPARE(readFile(QDir(pair.pathB).filePath("ui/notas.txt")), second);
    QCOMPARE(readFile(QDir(pair.pathB).filePath("concepts/b.kra")), QByteArray("concepts base"));
    const Outcome state = run(pair.b->status(pair.pathB));
    VERIFY_OK(state);
    QVERIFY(state.result.value<ProjectStatus>().pendingUpload >= 1);
    QVERIFY(state.result.value<ProjectStatus>().files.isEmpty());

    // Synchronising shows the two versions of the file, as for any divergence.
    const Outcome sync = run(pair.b->synchronise(pair.pathB, QString()));
    VERIFY_OK(sync);
    const SyncResult result = sync.result.value<SyncResult>();
    QCOMPARE(result.divergences.size(), 1);
    QCOMPARE(result.divergences[0].path, QString("ui/notas.txt"));
    QCOMPARE(readFile(QDir(pair.pathB).filePath("ui/notas.txt")), second);
}

void LoreDriverTest::testOperationsWithoutNetworkEndOffline()
{
    REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath("project");
    {
        LoreDriver driver;
        QVERIFY(createProject(&driver, project));
        QVERIFY(writeFile(project, "ui/a.kra", "a"));
        VERIFY_OK(run(driver.synchronise(project, "base")));
    }
    const ProjectInfo info = m_project;

    // Each operation with a driver of its own, so that none reuses another's answer.
    using Start = std::function<KisVersioningJob *(LoreDriver &)>;
    const QVector<QPair<const char *, Start>> operations = {
        {"whoIsEditing", [&](LoreDriver &d) { return d.whoIsEditing(project, {"ui/a.kra"}); }},
        {"announceEditing(true)", [&](LoreDriver &d) { return d.announceEditing(project, "ui/a.kra", true); }},
        {"announceEditing(false)", [&](LoreDriver &d) { return d.announceEditing(project, "ui/a.kra", false); }},
        {"listProjects", [&](LoreDriver &d) { return d.listProjects(server()); }},
        {"listFolders", [&](LoreDriver &d) { return d.listFolders(info); }},
        {"downloadProject", [&](LoreDriver &d) { return d.downloadProject(info, dir.filePath("second"), QStringList()); }},
        {"setFolders", [&](LoreDriver &d) { return d.setFolders(project, {"ui"}); }},
    };
    for (const auto &operation : operations) {
        LoreDriver driver;
        driver.setIdentityForTesting(QStringLiteral("A"));
        driver.setOfflineForTesting(true);
        QElapsedTimer timer;
        timer.start();
        const Outcome outcome = run(operation.second(driver));
        QVERIFY2(outcome.done, operation.first);
        QVERIFY2(outcome.error == KisVersioningJob::Offline,
                 qPrintable(QStringLiteral("%1: %2").arg(operation.first, outcome.errorText)));
        QVERIFY(!outcome.errorText.isEmpty());
        QVERIFY2(timer.elapsed() < 10000, operation.first);
    }
    // The download left nothing behind.
    QVERIFY(!QFileInfo::exists(dir.filePath("second")));
}


QVector<KisVersioningDriverConformance::Outcome> LoreDriverTest::runTogether(const QVector<KisVersioningJob *> &jobs,
                                                                             int timeoutMs)
{
    QVector<Outcome> outcomes(jobs.size());
    int left = jobs.size();
    QEventLoop loop;
    for (int i = 0; i < jobs.size(); ++i) {
        QObject::connect(jobs[i], &KisVersioningJob::finished, &loop, [&outcomes, &left, &loop, i](KisVersioningJob *job) {
            outcomes[i].done = true;
            outcomes[i].error = job->error();
            outcomes[i].errorText = job->errorText();
            outcomes[i].result = job->result<QVariant>();
            if (--left == 0) {
                loop.quit();
            }
        });
    }
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    if (left > 0) {
        loop.exec();
    }
    return outcomes;
}

void LoreDriverTest::testBackToBackOperationsShareTheServerCheck()
{
    REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath("first");
    const QString second = dir.filePath("second");
    ProjectInfo firstProject;
    ProjectInfo secondProject;
    {
        LoreDriver setUp;
        QVERIFY(createProject(&setUp, first));
        QVERIFY(writeFile(first, "ui/a.kra", "a"));
        VERIFY_OK(run(setUp.synchronise(first, "ui")));
        firstProject = m_project;
        QVERIFY(createProject(&setUp, second));
        QVERIFY(writeFile(second, "concepts/b.kra", "b"));
        VERIFY_OK(run(setUp.synchronise(second, "concepts")));
        secondProject = m_project;
    }

    // As the panel does: the folders of each project, one right after the other, on one driver.
    {
        LoreDriver driver;
        const QVector<Outcome> listed = runTogether({driver.listFolders(firstProject), driver.listFolders(secondProject)});
        VERIFY_OK(listed[0]);
        VERIFY_OK(listed[1]);
        QCOMPARE(listed[0].result.toStringList(), QStringList({"ui"}));
        QCOMPARE(listed[1].result.toStringList(), QStringList({"concepts"}));
    }
    // An editing notice withdrawn while the question who is editing is still under way.
    {
        LoreDriver driver;
        driver.setIdentityForTesting(QStringLiteral("A"));
        const QVector<Outcome> presence =
            runTogether({driver.whoIsEditing(first, {"ui/a.kra"}), driver.announceEditing(first, "ui/a.kra", false)});
        VERIFY_OK(presence[0]);
        VERIFY_OK(presence[1]);
    }
    // Without network, both end Offline.
    {
        LoreDriver driver;
        driver.setOfflineForTesting(true);
        const QVector<Outcome> listed = runTogether({driver.listFolders(firstProject), driver.listFolders(secondProject)});
        QCOMPARE(listed[0].error, KisVersioningJob::Offline);
        QCOMPARE(listed[1].error, KisVersioningJob::Offline);
        const QVector<Outcome> presence =
            runTogether({driver.whoIsEditing(first, {"ui/a.kra"}), driver.announceEditing(first, "ui/a.kra", false)});
        QCOMPARE(presence[0].error, KisVersioningJob::Offline);
        QCOMPARE(presence[1].error, KisVersioningJob::Offline);
    }
}

void LoreDriverTest::testLateAnswerDoesNotOverwriteNewerState()
{
    using Decision = LoreReachability::Decision;
    LoreReachability reachability;
    const QString key = QStringLiteral("lore://server");
    QVERIFY(reachability.decide(key) == Decision::Ask);

    // A check passes its limit: the server counts as not answering, and its call lingers.
    const quint64 first = reachability.begin(key);
    QVERIFY(reachability.decide(key) == Decision::Wait);
    reachability.settle(key, first, false, true);
    QVERIFY(reachability.decide(key) == Decision::Unreachable);
    QCOMPARE(reachability.lingering(key), 1);

    // A newer check is under way when the old call ends: the newer one is still waited for.
    const quint64 second = reachability.begin(key);
    reachability.lateEnd(key, first, false);
    QVERIFY(reachability.decide(key) == Decision::Wait);
    QCOMPARE(reachability.lingering(key), 0);
    reachability.settle(key, second, true, false);
    QVERIFY(reachability.decide(key) == Decision::Ask);

    // A late "no" from a timed-out check after a newer "yes" changes nothing.
    const quint64 third = reachability.begin(key);
    reachability.settle(key, third, false, true);
    const quint64 fourth = reachability.begin(key);
    reachability.settle(key, fourth, true, false);
    reachability.lateEnd(key, third, false);
    QVERIFY(reachability.decide(key) == Decision::Ask);

    // An older check that answers after a newer one changes nothing either.
    const quint64 fifth = reachability.begin(key);
    const quint64 sixth = reachability.begin(key);
    reachability.settle(key, sixth, true, false);
    reachability.settle(key, fifth, false, false);
    QVERIFY(reachability.decide(key) == Decision::Ask);

    // The answer of the newest check counts, and "no" is reused for a while.
    const quint64 seventh = reachability.begin(key);
    reachability.settle(key, seventh, false, false);
    QVERIFY(reachability.decide(key) == Decision::Unreachable);

    // Other servers are not affected.
    QVERIFY(reachability.decide(QStringLiteral("lore://other")) == Decision::Ask);
}

void LoreDriverTest::testSynchroniseAndStatusWithoutNetwork()
{
    REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LoreDriver driver;
    const QString project = dir.filePath("project");
    QVERIFY(createProject(&driver, project));
    QVERIFY(writeFile(project, "a.kra", "base"));
    VERIFY_OK(run(driver.synchronise(project, "base")));

    // No network: the version is created on this computer and waits.
    driver.setOfflineForTesting(true);
    QVERIFY(writeFile(project, "a.kra", "changed with no network"));
    const Outcome offline = run(driver.synchronise(project, "no network"));
    VERIFY_OK(offline);
    QVERIFY(offline.result.value<SyncResult>().pendingUpload);
    QVERIFY(offline.result.value<SyncResult>().divergences.isEmpty());

    // The state is read from this computer: nothing changed on disk, nothing counted on the server,
    // and the version that was not sent is counted.
    const Outcome state = run(driver.status(project));
    VERIFY_OK(state);
    QVERIFY(state.result.value<ProjectStatus>().files.isEmpty());
    QCOMPARE(state.result.value<ProjectStatus>().incoming, 0);
    QCOMPARE(state.result.value<ProjectStatus>().pendingUpload, 1);
    QVERIFY(state.result.value<ProjectStatus>().serverVersion.isEmpty());

    // The application restarts, still with no network: the driver remembers nothing, the project
    // does. Its state and a synchronisation with nothing new both say that work is waiting.
    {
        LoreDriver restarted;
        restarted.setIdentityForTesting(QStringLiteral("A"));
        restarted.setOfflineForTesting(true);
        const Outcome again = run(restarted.status(project));
        VERIFY_OK(again);
        QCOMPARE(again.result.value<ProjectStatus>().pendingUpload, 1);
        const Outcome nothingNew = run(restarted.synchronise(project, QString()));
        VERIFY_OK(nothingNew);
        QVERIFY(nothingNew.result.value<SyncResult>().pendingUpload);
    }

    // With the network back, the server does not have it yet; one synchronisation sends it.
    driver.setOfflineForTesting(false);
    const Outcome online = run(driver.status(project));
    VERIFY_OK(online);
    QVERIFY(online.result.value<ProjectStatus>().pendingUpload >= 1);
    // The server's version is known again; it changes when the version made here reaches it.
    const QString before = online.result.value<ProjectStatus>().serverVersion;
    QVERIFY(!before.isEmpty());
    const Outcome sent = run(driver.synchronise(project, QString()));
    VERIFY_OK(sent);
    QVERIFY(!sent.result.value<SyncResult>().pendingUpload);
    QCOMPARE(historyOf(&driver, project, "a.kra").size(), 2);
    const Outcome after = run(driver.status(project));
    VERIFY_OK(after);
    QCOMPARE(after.result.value<ProjectStatus>().pendingUpload, 0);
    QVERIFY(after.result.value<ProjectStatus>().serverVersion != before);

    // Sent, it is no longer counted with no network either.
    driver.setOfflineForTesting(true);
    const Outcome offlineAfter = run(driver.status(project));
    VERIFY_OK(offlineAfter);
    QCOMPARE(offlineAfter.result.value<ProjectStatus>().pendingUpload, 0);
}

void LoreDriverTest::testFolderMadeHereIsSentWhateverTheChoice()
{
    REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Pair pair;
    QVERIFY2(setUpPair(dir.path(), &pair, {{"ui/a.kra", "ui"}, {"concepts/b.kra", "concepts"}, {"raiz.kra", "root"}}),
             "could not set up two copies of the project");

    // The second person keeps only "ui": the driver says which folders of the server it left out.
    const Outcome chosen = run(pair.b->setFolders(pair.pathB, {"ui"}));
    VERIFY_OK(chosen);
    QCOMPARE(chosen.result.toStringList(), QStringList({"concepts"}));
    QVERIFY(!QFileInfo::exists(QDir(pair.pathB).filePath("concepts")));

    // A folder that exists only on this computer, and new files beside the chosen ones, are sent.
    QVERIFY(writeFile(pair.pathB, "icons/novo.kra", "icons"));
    QVERIFY(writeFile(pair.pathB, "ui/novo.kra", "ui new"));
    QVERIFY(writeFile(pair.pathB, "raiz2.kra", "root new"));
    const Outcome state = run(pair.b->status(pair.pathB));
    VERIFY_OK(state);
    QStringList listed;
    for (const FileStatus &file : state.result.value<ProjectStatus>().files) {
        listed << file.path;
    }
    QVERIFY2(listed.contains("icons/novo.kra"), qPrintable(listed.join(", ")));
    const Outcome sent = run(pair.b->synchronise(pair.pathB, "new folder"));
    VERIFY_OK(sent);
    const QStringList sentPaths = sent.result.value<SyncResult>().sent;
    QVERIFY2(sentPaths.contains("icons/novo.kra"), qPrintable(sentPaths.join(", ")));
    QVERIFY(sentPaths.contains("ui/novo.kra"));
    QVERIFY(sentPaths.contains("raiz2.kra"));
    QVERIFY(!sent.result.value<SyncResult>().pendingUpload);

    VERIFY_OK(run(pair.a->synchronise(pair.pathA, QString())));
    QCOMPARE(readFile(QDir(pair.pathA).filePath("icons/novo.kra")), QByteArray("icons"));
    QCOMPARE(readFile(QDir(pair.pathA).filePath("ui/novo.kra")), QByteArray("ui new"));

    // A folder of the server that was not chosen stays off this computer; a top-level folder another
    // person adds later arrives, since it was not on the server when the choice was made.
    QVERIFY(writeFile(pair.pathA, "concepts/b.kra", "concepts changed"));
    QVERIFY(writeFile(pair.pathA, "depois/c.kra", "added later"));
    VERIFY_OK(run(pair.a->synchronise(pair.pathA, "later")));
    const Outcome received = run(pair.b->synchronise(pair.pathB, QString()));
    VERIFY_OK(received);
    QVERIFY(!QFileInfo::exists(QDir(pair.pathB).filePath("concepts")));
    QCOMPARE(readFile(QDir(pair.pathB).filePath("depois/c.kra")), QByteArray("added later"));

    // A download with a choice leaves out every other folder of the server, the new ones included.
    LoreDriver third;
    third.setIdentityForTesting(QStringLiteral("C"));
    const QString pathC = dir.filePath("third");
    const Outcome downloaded = run(third.downloadProject(m_project, pathC, {"ui"}));
    VERIFY_OK(downloaded);
    QStringList leftOut = downloaded.result.toStringList();
    leftOut.sort();
    QCOMPARE(leftOut, QStringList({"concepts", "depois", "icons"}));
    QCOMPARE(readFile(QDir(pathC).filePath("ui/novo.kra")), QByteArray("ui new"));
    QVERIFY(QFileInfo::exists(QDir(pathC).filePath("raiz2.kra")));
    QVERIFY(!QFileInfo::exists(QDir(pathC).filePath("concepts")));
    QVERIFY(!QFileInfo::exists(QDir(pathC).filePath("icons")));

    // Back to the whole project: nothing is left out.
    const Outcome whole = run(pair.b->setFolders(pair.pathB, QStringList()));
    VERIFY_OK(whole);
    QVERIFY(whole.result.toStringList().isEmpty());
    QCOMPARE(readFile(QDir(pair.pathB).filePath("concepts/b.kra")), QByteArray("concepts changed"));
}

SIMPLE_TEST_MAIN(LoreDriverTest)
