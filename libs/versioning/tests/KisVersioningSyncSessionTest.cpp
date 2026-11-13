/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningSyncSessionTest.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <klocalizedstring.h>

#include <KisVersioningConflictMarkers.h>
#include <KisVersioningSyncSession.h>

#include "KisVersioningFakeDriver.h"

using namespace KisVersioning;

namespace {

class FakeHost : public KisVersioningDocumentHost
{
public:
    QStringList modified;
    QStringList openAndUnmodified;  ///< open documents without changes; a saved document joins them
    QStringList modifiedAgain;      ///< open documents the artist changed again after the save
    bool saveSucceeds = true;
    QStringList reloaded;
    QStringList *sharedLog = nullptr;

    QStringList modifiedOpenDocuments(const QString &) const override { return modified; }
    bool saveDocument(const QString &absolutePath) override
    {
        if (sharedLog) {
            sharedLog->append(QStringLiteral("host:saveDocument ") + absolutePath);
        }
        if (saveSucceeds && !openAndUnmodified.contains(absolutePath)) {
            openAndUnmodified << absolutePath;
        }
        return saveSucceeds;
    }
    bool isOpenAndUnmodified(const QString &absolutePath) const override
    {
        return openAndUnmodified.contains(absolutePath) && !modifiedAgain.contains(absolutePath);
    }
    void reloadDocument(const QString &absolutePath) override
    {
        reloaded << absolutePath;
        if (sharedLog) {
            sharedLog->append(QStringLiteral("host:reloadDocument ") + absolutePath);
        }
    }
};

struct Fixture {
    QTemporaryDir dir;
    KisVersioningFakeDriver driver;
    FakeHost host;
    QStringList log;

    Fixture()
    {
        driver.setSharedLog(&log);
        host.sharedLog = &log;
    }

    QString path(const QString &relative) const { return QDir(dir.path()).filePath(relative); }

    void write(const QString &relative, const QByteArray &content)
    {
        QDir().mkpath(QFileInfo(path(relative)).absolutePath());
        QFile file(path(relative));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
    }

    QByteArray read(const QString &relative) const
    {
        QFile file(path(relative));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
    }

    KisVersioningSyncSession::Report run(const QString &notes = QStringLiteral("notes"))
    {
        KisVersioningSyncSession session(&driver, &host);
        QSignalSpy spy(&session, &KisVersioningSyncSession::finished);
        session.start(dir.path(), notes);
        if (spy.isEmpty()) {
            spy.wait(5000);
        }
        if (spy.count() != 1) {
            return failureReport();
        }
        return spy.first().first().value<KisVersioningSyncSession::Report>();
    }

    static KisVersioningSyncSession::Report failureReport()
    {
        KisVersioningSyncSession::Report report;
        report.errorText = QStringLiteral("finished was not emitted once");
        return report;
    }
};

SyncResult resultWithDivergence(const QString &path, DivergenceKind kind)
{
    SyncResult result;
    Divergence divergence;
    divergence.path = path;
    divergence.kind = kind;
    result.divergences.append(divergence);
    return result;
}

SyncResult serverChangedResult()
{
    SyncResult result;
    result.serverChanged = true;
    return result;
}

} // namespace

void KisVersioningSyncSessionTest::initTestCase()
{
    KLocalizedString::setLanguages({QStringLiteral("en_US")});
}

void KisVersioningSyncSessionTest::testOnlyMineIsSent()
{
    Fixture f;
    SyncResult result;
    result.sent = QStringList{"ui/botao.kra"};
    f.driver.queueSyncResult(result);

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::NoError);
    QCOMPARE(report.sent, QStringList({"ui/botao.kra"}));
    QVERIFY(report.received.isEmpty());
    QVERIFY(!report.pendingUpload);
    QCOMPARE(f.driver.callCount("synchronise"), 1);
    QCOMPARE(f.driver.callCount("resolve"), 0);
    QCOMPARE(f.driver.callsNamed("synchronise").first().args, QStringList({f.dir.path(), "notes"}));
}

void KisVersioningSyncSessionTest::testOnlyTheirsReloadsCleanDoc()
{
    Fixture f;
    SyncResult result;
    result.received = QStringList{"ui/botao.kra", "ui/other.kra"};
    f.driver.queueSyncResult(result);
    f.host.openAndUnmodified = QStringList{f.path("ui/botao.kra")};

    const auto report = f.run();

    QCOMPARE(report.received, QStringList({"ui/botao.kra", "ui/other.kra"}));
    QCOMPARE(f.host.reloaded, QStringList({f.path("ui/botao.kra")}));
}

void KisVersioningSyncSessionTest::testModifiedOpenDocIsSavedFirst()
{
    Fixture f;
    f.host.modified = QStringList{f.path("ui/botao.kra")};
    // The artist keeps working while the synchronisation runs: unsaved changes are never reloaded over.
    f.host.modifiedAgain = QStringList{f.path("ui/botao.kra")};
    SyncResult result;
    result.sent = QStringList{"ui/botao.kra"};
    result.received = QStringList{"ui/botao.kra"};
    f.driver.queueSyncResult(result);

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::NoError);
    QCOMPARE(f.log.size(), 2);
    QVERIFY(f.log.at(0).startsWith("host:saveDocument "));
    QVERIFY(f.log.at(1).startsWith("driver:synchronise "));
    QVERIFY(f.host.reloaded.isEmpty());
}

void KisVersioningSyncSessionTest::testKeepBothReloadsCleanOpenDocument()
{
    Fixture f;
    f.driver.setCurrentUser("Bruno");
    f.write("ui/botao.kra", "mine");
    f.driver.setTheirContent("ui/botao.kra", "theirs");
    f.host.modified = QStringList{f.path("ui/botao.kra")};  // saved first, so unmodified afterwards
    f.driver.queueSyncResult(resultWithDivergence("ui/botao.kra", DivergenceKind::BothModified));

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::NoError);
    QCOMPARE(f.host.reloaded, QStringList({f.path("ui/botao.kra")}));
    QCOMPARE(f.read("ui/botao (Bruno).kra"), QByteArray("mine"));
    int resolveAt = -1, reloadAt = -1;
    for (int i = 0; i < f.log.size(); ++i) {
        if (f.log.at(i).startsWith("driver:resolve ")) resolveAt = i;
        if (f.log.at(i).startsWith("host:reloadDocument ")) reloadAt = i;
    }
    QVERIFY(resolveAt >= 0);
    QVERIFY(reloadAt > resolveAt);
}

void KisVersioningSyncSessionTest::testKeepBothDoesNotReloadModifiedDocument()
{
    Fixture f;
    f.driver.setCurrentUser("Bruno");
    f.write("ui/botao.kra", "mine");
    f.host.openAndUnmodified = QStringList{f.path("ui/botao.kra")};
    f.host.modifiedAgain = QStringList{f.path("ui/botao.kra")};
    f.driver.queueSyncResult(resultWithDivergence("ui/botao.kra", DivergenceKind::BothModified));

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::NoError);
    QVERIFY(f.host.reloaded.isEmpty());
    QCOMPARE(f.read("ui/botao (Bruno).kra"), QByteArray("mine"));
}

void KisVersioningSyncSessionTest::testBothModifiedKeepsBoth()
{
    Fixture f;
    f.driver.setCurrentUser("Bruno");
    f.write("ui/botao.kra", "mine");
    f.driver.setTheirContent("ui/botao.kra", "theirs");
    f.driver.queueSyncResult(resultWithDivergence("ui/botao.kra", DivergenceKind::BothModified));
    SyncResult second;
    second.sent = QStringList{"ui/botao.kra", "ui/botao (Bruno).kra"};
    f.driver.queueSyncResult(second);

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::NoError);
    QCOMPARE(f.read("ui/botao (Bruno).kra"), QByteArray("mine"));
    QCOMPARE(f.read("ui/botao.kra"), QByteArray("theirs"));

    const auto resolves = f.driver.callsNamed("resolve");
    QCOMPARE(resolves.size(), 1);
    QCOMPARE(resolves.first().args.mid(1, 2), QStringList({"ui/botao.kra", "Theirs"}));

    const auto markers = KisVersioningConflictMarkers(f.dir.path()).list();
    QCOMPARE(markers.size(), 1);
    QCOMPARE(markers.first().originalPath, QString("ui/botao.kra"));
    QCOMPARE(markers.first().copyPath, QString("ui/botao (Bruno).kra"));
    QCOMPARE(markers.first().author, QString("Bruno"));

    QCOMPARE(f.driver.callCount("synchronise"), 2);
    QCOMPARE(report.keptBoth, QStringList({"ui/botao.kra"}));
    QCOMPARE(report.conflictCopies, QStringList({"ui/botao (Bruno).kra"}));
    QVERIFY(!report.pendingUpload);
}

void KisVersioningSyncSessionTest::testConflictCopyNameAlreadyTaken()
{
    Fixture f;
    f.driver.setCurrentUser("Bruno");
    f.write("ui/botao.kra", "mine");
    f.write("ui/botao (Bruno).kra", "an earlier copy");
    f.driver.queueSyncResult(resultWithDivergence("ui/botao.kra", DivergenceKind::BothModified));

    const auto report = f.run();

    QCOMPARE(report.conflictCopies, QStringList({"ui/botao (Bruno) 2.kra"}));
    QCOMPARE(f.read("ui/botao (Bruno) 2.kra"), QByteArray("mine"));
    QCOMPARE(f.read("ui/botao (Bruno).kra"), QByteArray("an earlier copy"));
}

void KisVersioningSyncSessionTest::testConflictCopyNeverOverwritesOtherCase()
{
    // A file whose name differs only in case is the same file on Windows and macOS.
    Fixture f;
    f.driver.setCurrentUser("Bruno");
    f.write("ui/botao.kra", "mine");
    f.write("ui/Botao (Bruno).kra", "an earlier copy");
    f.driver.queueSyncResult(resultWithDivergence("ui/botao.kra", DivergenceKind::BothModified));

    const auto report = f.run();

    QCOMPARE(report.conflictCopies, QStringList({"ui/botao (Bruno) 2.kra"}));
    QCOMPARE(f.read("ui/Botao (Bruno).kra"), QByteArray("an earlier copy"));
}

void KisVersioningSyncSessionTest::testConflictCopyFailureLeavesFileUntouched()
{
    Fixture f;
    // A copy name longer than any file system allows: the copy cannot be written.
    f.driver.setCurrentUser(QString(300, QLatin1Char('x')));
    f.write("ui/botao.kra", "mine");
    f.driver.setTheirContent("ui/botao.kra", "theirs");
    f.driver.queueSyncResult(resultWithDivergence("ui/botao.kra", DivergenceKind::BothModified));

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::Failed);
    QVERIFY(!report.errorText.isEmpty());
    QCOMPARE(f.driver.callCount("resolve"), 0);
    QCOMPARE(f.read("ui/botao.kra"), QByteArray("mine"));
    QVERIFY(report.keptBoth.isEmpty());
    QVERIFY(report.conflictCopies.isEmpty());
    QCOMPARE(f.driver.callCount("synchronise"), 1);
}

void KisVersioningSyncSessionTest::testDeleteVersusModifyKeepsTheChange()
{
    {
        // I deleted it, the other person changed it: take theirs.
        Fixture f;
        f.driver.setCurrentUser("Bruno");
        f.host.openAndUnmodified = QStringList{f.path("ui/botao.kra")};
        f.driver.queueSyncResult(resultWithDivergence("ui/botao.kra", DivergenceKind::DeletedHereModifiedThere));

        const auto report = f.run();
        QCOMPARE(f.host.reloaded, QStringList({f.path("ui/botao.kra")}));

        QCOMPARE(report.error, KisVersioningJob::NoError);
        const auto resolves = f.driver.callsNamed("resolve");
        QCOMPARE(resolves.size(), 1);
        QCOMPARE(resolves.first().args.mid(1, 2), QStringList({"ui/botao.kra", "Theirs"}));
        QVERIFY(report.conflictCopies.isEmpty());
        QCOMPARE(QDir(f.dir.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
    {
        // I changed it, the other person deleted it: keep mine.
        Fixture f;
        f.driver.setCurrentUser("Bruno");
        f.write("ui/botao.kra", "mine");
        f.host.openAndUnmodified = QStringList{f.path("ui/botao.kra")};
        f.driver.queueSyncResult(resultWithDivergence("ui/botao.kra", DivergenceKind::ModifiedHereDeletedThere));

        const auto report = f.run();
        QVERIFY(f.host.reloaded.isEmpty());  // my file stays: nothing to show again

        QCOMPARE(report.error, KisVersioningJob::NoError);
        const auto resolves = f.driver.callsNamed("resolve");
        QCOMPARE(resolves.size(), 1);
        QCOMPARE(resolves.first().args.mid(1, 2), QStringList({"ui/botao.kra", "Mine"}));
        QVERIFY(report.conflictCopies.isEmpty());
        QCOMPARE(f.read("ui/botao.kra"), QByteArray("mine"));
        QCOMPARE(QDir(f.path("ui")).entryList(QDir::Files).size(), 1);
    }
}

void KisVersioningSyncSessionTest::testServerChangedRetriesUpToThreeTimes()
{
    Fixture f;
    for (int i = 0; i < 5; ++i) {
        f.driver.queueSyncResult(serverChangedResult());
    }

    const auto report = f.run();

    QCOMPARE(f.driver.callCount("synchronise"), KisVersioningSyncSession::MaxAttempts);
    QCOMPARE(f.driver.callCount("synchronise"), 3);
    QVERIFY(report.pendingUpload);
    QCOMPARE(report.error, KisVersioningJob::NoError);
}

void KisVersioningSyncSessionTest::testOfflineIsPendingUpload()
{
    Fixture f;
    f.write("ui/botao.kra", "mine");
    f.driver.queueSyncError(KisVersioningJob::Offline, "no network");

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::Offline);
    QCOMPARE(report.errorText, QString("no network"));
    QVERIFY(report.pendingUpload);
    QCOMPARE(f.driver.callCount("resolve"), 0);
    QCOMPARE(f.read("ui/botao.kra"), QByteArray("mine"));
}

void KisVersioningSyncSessionTest::testNotSignedInIsReported()
{
    Fixture f;
    f.driver.queueSyncError(KisVersioningJob::NotSignedIn, "sign in first");

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::NotSignedIn);
    QVERIFY(report.pendingUpload);
}

void KisVersioningSyncSessionTest::testResolveFailureEndsWithError()
{
    Fixture f;
    f.driver.setCurrentUser("Bruno");
    f.write("ui/botao.kra", "mine");
    f.driver.setResolveError(KisVersioningJob::Failed, "server refused");
    f.driver.queueSyncResult(resultWithDivergence("ui/botao.kra", DivergenceKind::BothModified));

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::Failed);
    QCOMPARE(report.errorText, QString("server refused"));
    QCOMPARE(f.driver.callCount("synchronise"), 1);
    // The copy made before the failed resolve stays: it holds my content.
    QCOMPARE(f.read("ui/botao (Bruno).kra"), QByteArray("mine"));
}

void KisVersioningSyncSessionTest::testSaveFailureStopsBeforeDriver()
{
    Fixture f;
    f.host.modified = QStringList{f.path("ui/botao.kra")};
    f.host.saveSucceeds = false;

    const auto report = f.run();

    QCOMPARE(report.error, KisVersioningJob::Failed);
    QVERIFY(!report.errorText.isEmpty());
    QVERIFY(f.driver.calls().isEmpty());
}

void KisVersioningSyncSessionTest::testFinishedIsEmittedOnce()
{
    Fixture f;
    f.driver.queueSyncResult(serverChangedResult());
    KisVersioningSyncSession session(&f.driver, &f.host);
    QSignalSpy spy(&session, &KisVersioningSyncSession::finished);
    session.start(f.dir.path(), "notes");
    QVERIFY(spy.wait(5000));
    QTest::qWait(100);
    QCOMPARE(spy.count(), 1);
}

SIMPLE_TEST_MAIN(KisVersioningSyncSessionTest)
