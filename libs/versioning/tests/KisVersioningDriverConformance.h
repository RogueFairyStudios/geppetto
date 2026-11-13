/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGDRIVERCONFORMANCE_H
#define KISVERSIONINGDRIVERCONFORMANCE_H

#include <simpletest.h>

#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QObject>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>

#include <klocalizedstring.h>

#include <KisVersioningDriver.h>
#include <KisVersioningFileRules.h>
#include <KisVersioningTypes.h>

// Every test starts with one of these: they skip it when the backend or the feature is missing.
#define REQUIRE_BACKEND()                                                           do {                                                                                QString skipReason_;                                                            if (!backendAvailable(&skipReason_)) {                                              QSKIP(qPrintable(skipReason_));                                             }                                                                           } while (false)

#define REQUIRE_FEATURE(CAPABILITY)                                                 do {                                                                                REQUIRE_BACKEND();                                                              if (!driverHas(CAPABILITY)) {                                                       QSKIP("The driver does not declare the feature " #CAPABILITY);              }                                                                           } while (false)

// Fails the test when the job did not finish in time or ended with an error.
#define VERIFY_OK(OUTCOME)                                                          do {                                                                                const Outcome verifyOutcome_ = (OUTCOME);                                       QVERIFY2(verifyOutcome_.done, "the job did not finish in time");                QVERIFY2(verifyOutcome_.error == KisVersioningJob::NoError,                              qPrintable(verifyOutcome_.errorText));                             } while (false)

/**
 * The tests every versioning driver must pass. A driver's test inherits this
 * class, implements the hooks below and gets all the tests as slots. A test
 * that needs a feature (RemoteProjects, PartialDownload, Presence) skips
 * itself when the backend is not available or the driver does not declare it.
 *
 * moc has to run on this header for the derived test: list it among that
 * test's sources and add this directory to its include path.
 *
 * Tests that need two people create two drivers and two local paths:
 * createProject() makes the project for the first one, cloneProject() brings
 * the same project to the second one.
 */
class KisVersioningDriverConformance : public QObject
{
    Q_OBJECT

protected:
    /// False, with the reason in @p reason, when the backend cannot be reached; every test then skips.
    virtual bool backendAvailable(QString *reason) const = 0;
    virtual KisVersioningDriver *createDriver(QObject *parent) = 0;
    /// Creates an empty project on the backend and downloads it to localPath.
    virtual bool createProject(KisVersioningDriver *driver, const QString &localPath) = 0;
    /// Downloads the same project to another path, for drivers with RemoteProjects.
    virtual bool cloneProject(KisVersioningDriver *driver, const QString &localPath) = 0;
    virtual void setOffline(KisVersioningDriver *driver, bool offline) = 0;

private Q_SLOTS:
    void initTestCase()
    {
        // The assertions use the English source strings, whatever the machine's language.
        KLocalizedString::setLanguages({QStringLiteral("en_US")});
    }

    void testSyncCreatesVersionWithNotes()
    {
        REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<KisVersioningDriver> driver(createDriver(nullptr));
        const QString project = dir.filePath("project");
        QVERIFY(createProject(driver.data(), project));

        QVERIFY(writeFile(project, "fundo.kra", "bytes"));
        const Outcome sync = run(driver->synchronise(project, "ajuste de contraste no fundo"));
        VERIFY_OK(sync);
        QVERIFY(sync.result.value<KisVersioning::SyncResult>().sent.contains("fundo.kra"));

        const Outcome historyOutcome = run(driver->fileHistory(project, "fundo.kra", 0, 50));
        VERIFY_OK(historyOutcome);
        const QVector<KisVersioning::VersionInfo> history = historyOutcome.result.value<QVector<KisVersioning::VersionInfo>>();
        QCOMPARE(history.size(), 1);
        QCOMPARE(history[0].number, 1);
        QCOMPARE(history[0].notes, QString("ajuste de contraste no fundo"));
        QVERIFY(!history[0].author.isEmpty());
    }

    void testEmptyNotesGetDefaultSummary()
    {
        REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<KisVersioningDriver> driver(createDriver(nullptr));
        const QString project = dir.filePath("project");
        QVERIFY(createProject(driver.data(), project));

        QVERIFY(writeFile(project, "a.kra", "a"));
        QVERIFY(writeFile(project, "b.kra", "b"));
        QVERIFY(writeFile(project, "c.kra", "c"));
        VERIFY_OK(run(driver->synchronise(project, QString())));

        const QVector<KisVersioning::VersionInfo> history = historyOf(driver.data(), project, "a.kra");
        QCOMPARE(history.size(), 1);
        QCOMPARE(history[0].notes, QString("3 files changed"));
    }

    void testNothingChangedCreatesNoVersion()
    {
        REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<KisVersioningDriver> driver(createDriver(nullptr));
        const QString project = dir.filePath("project");
        QVERIFY(createProject(driver.data(), project));

        QVERIFY(writeFile(project, "a.kra", "a"));
        VERIFY_OK(run(driver->synchronise(project, "first")));

        const Outcome again = run(driver->synchronise(project, "second"));
        VERIFY_OK(again);
        QVERIFY(again.result.value<KisVersioning::SyncResult>().sent.isEmpty());
        QCOMPARE(historyOf(driver.data(), project, "a.kra").size(), 1);
    }

    void testNeverSyncedFilesAreIgnored()
    {
        REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<KisVersioningDriver> driver(createDriver(nullptr));
        const QString project = dir.filePath("project");
        QVERIFY(createProject(driver.data(), project));

        QVERIFY(writeFile(project, "ui/botao.kra", "real"));
        QVERIFY(writeFile(project, "ui/botao.kra~", "backup"));
        QVERIFY(writeFile(project, "ui/.botao.kra-autosave.kra", "autosave"));

        const Outcome sync = run(driver->synchronise(project, "x"));
        VERIFY_OK(sync);
        const KisVersioning::SyncResult result = sync.result.value<KisVersioning::SyncResult>();
        QVERIFY(result.sent.contains("ui/botao.kra"));
        QVERIFY(!result.sent.contains("ui/botao.kra~"));
        QVERIFY(!result.sent.contains("ui/.botao.kra-autosave.kra"));

        // They do not show up as changes either.
        const Outcome statusOutcome = run(driver->status(project));
        VERIFY_OK(statusOutcome);
        for (const KisVersioning::FileStatus &file : statusOutcome.result.value<KisVersioning::ProjectStatus>().files) {
            QVERIFY(file.state == KisVersioning::FileState::Unchanged
                    || file.path == "ui/botao.kra");
        }
    }

    void testStatusReportsModifiedAddedDeleted()
    {
        REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<KisVersioningDriver> driver(createDriver(nullptr));
        const QString project = dir.filePath("project");
        QVERIFY(createProject(driver.data(), project));

        QVERIFY(writeFile(project, "a.kra", "a"));
        QVERIFY(writeFile(project, "b.kra", "b"));
        QVERIFY(writeFile(project, "c.kra", "c"));
        VERIFY_OK(run(driver->synchronise(project, "base")));

        QVERIFY(writeFile(project, "a.kra", "a changed"));
        QVERIFY(QFile::remove(QDir(project).filePath("b.kra")));
        QVERIFY(writeFile(project, "d.kra", "d"));

        const Outcome statusOutcome = run(driver->status(project));
        VERIFY_OK(statusOutcome);
        QMap<QString, KisVersioning::FileState> states;
        for (const KisVersioning::FileStatus &file : statusOutcome.result.value<KisVersioning::ProjectStatus>().files) {
            states.insert(file.path, file.state);
        }
        QVERIFY(states.value("a.kra", KisVersioning::FileState::Unchanged) == KisVersioning::FileState::Modified);
        QVERIFY(states.value("b.kra", KisVersioning::FileState::Unchanged) == KisVersioning::FileState::Deleted);
        QVERIFY(states.value("d.kra", KisVersioning::FileState::Unchanged) == KisVersioning::FileState::Added);
        QVERIFY(states.value("c.kra", KisVersioning::FileState::Unchanged) == KisVersioning::FileState::Unchanged);
    }

    void testHistoryIsOldestFirst()
    {
        REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<KisVersioningDriver> driver(createDriver(nullptr));
        const QString project = dir.filePath("project");
        QVERIFY(createProject(driver.data(), project));

        const QStringList notes = {"primeira", "segunda", "terceira"};
        for (int i = 0; i < notes.size(); ++i) {
            QVERIFY(writeFile(project, "a.kra", QByteArray("content ") + QByteArray::number(i)));
            VERIFY_OK(run(driver->synchronise(project, notes[i])));
        }

        const QVector<KisVersioning::VersionInfo> history = historyOf(driver.data(), project, "a.kra");
        QCOMPARE(history.size(), 3);
        for (int i = 0; i < notes.size(); ++i) {
            QCOMPARE(history[i].notes, notes[i]);
            if (i > 0) {
                QVERIFY(history[i].number > history[i - 1].number);
            }
        }

        // A page of the history keeps the same order.
        const Outcome page = run(driver->fileHistory(project, "a.kra", 1, 1));
        VERIFY_OK(page);
        const QVector<KisVersioning::VersionInfo> paged = page.result.value<QVector<KisVersioning::VersionInfo>>();
        QCOMPARE(paged.size(), 1);
        QCOMPARE(paged[0].notes, QString("segunda"));
    }

    void testFetchFileAtVersionLeavesWorkingFile()
    {
        REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<KisVersioningDriver> driver(createDriver(nullptr));
        const QString project = dir.filePath("project");
        QVERIFY(createProject(driver.data(), project));

        QVERIFY(writeFile(project, "a.kra", "bytes of version 1"));
        VERIFY_OK(run(driver->synchronise(project, "one")));
        QVERIFY(writeFile(project, "a.kra", "bytes of version 2"));
        VERIFY_OK(run(driver->synchronise(project, "two")));

        const QVector<KisVersioning::VersionInfo> history = historyOf(driver.data(), project, "a.kra");
        QCOMPARE(history.size(), 2);

        const QString output = dir.filePath("restored/a-v1.kra");
        VERIFY_OK(run(driver->fetchFileAtVersion(project, "a.kra", history[0].id, output)));
        QCOMPARE(readFile(output), QByteArray("bytes of version 1"));
        QCOMPARE(readFile(QDir(project).filePath("a.kra")), QByteArray("bytes of version 2"));
    }

    void testUnicodeAndSpacesInPaths()
    {
        REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<KisVersioningDriver> driver(createDriver(nullptr));
        const QString project = dir.filePath(QString::fromUtf8("projeto ação"));
        QVERIFY(createProject(driver.data(), project));

        const QString path = QString::fromUtf8("concepts/ícone novo (v2).kra");
        QVERIFY(writeFile(project, path, "unicode bytes"));
        const Outcome sync = run(driver->synchronise(project, QString::fromUtf8("ícone")));
        VERIFY_OK(sync);
        QVERIFY(sync.result.value<KisVersioning::SyncResult>().sent.contains(path));

        const QVector<KisVersioning::VersionInfo> history = historyOf(driver.data(), project, path);
        QCOMPARE(history.size(), 1);
        const QString output = dir.filePath(QString::fromUtf8("saída/ícone novo.kra"));
        VERIFY_OK(run(driver->fetchFileAtVersion(project, path, history[0].id, output)));
        QCOMPARE(readFile(output), QByteArray("unicode bytes"));

        // The name comes back unchanged to another person, too.
        if (driverHas(KisVersioning::RemoteProjects)) {
            QScopedPointer<KisVersioningDriver> other(createDriver(nullptr));
            const QString otherProject = dir.filePath("other");
            QVERIFY(cloneProject(other.data(), otherProject));
            QCOMPARE(readFile(QDir(otherProject).filePath(path)), QByteArray("unicode bytes"));
        }
    }

    void testMissingProjectFolderFails()
    {
        REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<KisVersioningDriver> driver(createDriver(nullptr));
        const QString project = dir.filePath("project");
        QVERIFY(createProject(driver.data(), project));
        QVERIFY(QDir(project).removeRecursively());

        const Outcome statusOutcome = run(driver->status(project));
        QVERIFY2(statusOutcome.done, "the job did not finish in time");
        QCOMPARE(statusOutcome.error, KisVersioningJob::Failed);
    }

    // RemoteProjects ------------------------------------------------------

    void testSecondCloneReceivesChanges()
    {
        REQUIRE_FEATURE(KisVersioning::RemoteProjects);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Pair pair;
        QVERIFY2(setUpPair(dir.path(), &pair, {{"base.kra", "base"}}), "could not set up two copies of the project");

        QVERIFY(writeFile(pair.pathA, "novo.kra", "new file"));
        VERIFY_OK(run(pair.a->synchronise(pair.pathA, "one")));

        const Outcome received = run(pair.b->synchronise(pair.pathB, QString()));
        VERIFY_OK(received);
        QVERIFY(received.result.value<KisVersioning::SyncResult>().received.contains("novo.kra"));
        QCOMPARE(readFile(QDir(pair.pathB).filePath("novo.kra")), QByteArray("new file"));

        // A change to a file the second copy already has arrives as well.
        QVERIFY(writeFile(pair.pathA, "base.kra", "base changed"));
        VERIFY_OK(run(pair.a->synchronise(pair.pathA, "two")));
        VERIFY_OK(run(pair.b->synchronise(pair.pathB, QString())));
        QCOMPARE(readFile(QDir(pair.pathB).filePath("base.kra")), QByteArray("base changed"));
    }

    // The most common case: each person changes a different file, and both synchronise.
    void testChangesToDifferentFilesReachBoth()
    {
        REQUIRE_FEATURE(KisVersioning::RemoteProjects);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Pair pair;
        QVERIFY2(setUpPair(dir.path(), &pair, {{"x.kra", "x base"}, {"y.kra", "y base"}}),
                 "could not set up two copies of the project");

        QVERIFY(writeFile(pair.pathA, "x.kra", "x changed by the first"));
        QVERIFY(writeFile(pair.pathB, "y.kra", "y changed by the second"));
        VERIFY_OK(run(pair.a->synchronise(pair.pathA, "first")));

        const Outcome second = run(pair.b->synchronise(pair.pathB, "second"));
        VERIFY_OK(second);
        const KisVersioning::SyncResult result = second.result.value<KisVersioning::SyncResult>();
        QVERIFY(result.divergences.isEmpty());
        QVERIFY(result.received.contains("x.kra"));
        QVERIFY(!result.received.contains("y.kra"));
        QVERIFY(result.sent.contains("y.kra"));
        QVERIFY(!result.pendingUpload);
        QCOMPARE(readFile(QDir(pair.pathB).filePath("x.kra")), QByteArray("x changed by the first"));
        QCOMPARE(readFile(QDir(pair.pathB).filePath("y.kra")), QByteArray("y changed by the second"));

        const Outcome first = run(pair.a->synchronise(pair.pathA, QString()));
        VERIFY_OK(first);
        QVERIFY(first.result.value<KisVersioning::SyncResult>().divergences.isEmpty());
        QVERIFY(first.result.value<KisVersioning::SyncResult>().received.contains("y.kra"));
        QCOMPARE(readFile(QDir(pair.pathA).filePath("x.kra")), QByteArray("x changed by the first"));
        QCOMPARE(readFile(QDir(pair.pathA).filePath("y.kra")), QByteArray("y changed by the second"));
    }

    // Both people change one file. Each content kind must end the same way: a backend may treat
    // text differently from binary content (merge it line by line), but the artist's file is never
    // changed without being told.
    void testDivergenceReportedAndMineIntact_data()
    {
        addDivergenceContents();
    }

    void testDivergenceReportedAndMineIntact()
    {
        REQUIRE_FEATURE(KisVersioning::RemoteProjects);
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

        const Outcome sync = run(pair.b->synchronise(pair.pathB, "second"));
        VERIFY_OK(sync);
        const KisVersioning::SyncResult result = sync.result.value<KisVersioning::SyncResult>();
        QCOMPARE(result.divergences.size(), 1);
        QCOMPARE(result.divergences[0].path, QString("a.kra"));
        QVERIFY(result.divergences[0].kind == KisVersioning::DivergenceKind::BothModified);
        // Byte for byte what the second person saved.
        QCOMPARE(readFile(QDir(pair.pathB).filePath("a.kra")), second);
    }

    void testResolveTheirsThenSyncIsAccepted_data()
    {
        addDivergenceContents();
    }

    void testResolveTheirsThenSyncIsAccepted()
    {
        REQUIRE_FEATURE(KisVersioning::RemoteProjects);
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
        QCOMPARE(diverged.result.value<KisVersioning::SyncResult>().divergences.size(), 1);
        QCOMPARE(readFile(QDir(pair.pathB).filePath("a.kra")), second);

        // Byte for byte what the first person saved.
        VERIFY_OK(run(pair.b->resolve(pair.pathB, "a.kra", KisVersioning::Resolution::Theirs, QString())));
        QCOMPARE(readFile(QDir(pair.pathB).filePath("a.kra")), first);

        const Outcome accepted = run(pair.b->synchronise(pair.pathB, "after resolving"));
        VERIFY_OK(accepted);
        QVERIFY(accepted.result.value<KisVersioning::SyncResult>().divergences.isEmpty());
        QVERIFY(!accepted.result.value<KisVersioning::SyncResult>().pendingUpload);
        QCOMPARE(readFile(QDir(pair.pathB).filePath("a.kra")), first);

        // The first person still finds their own version at the path.
        VERIFY_OK(run(pair.a->synchronise(pair.pathA, QString())));
        QCOMPARE(readFile(QDir(pair.pathA).filePath("a.kra")), first);
    }

    void testDeleteVersusModifyIsReported()
    {
        REQUIRE_FEATURE(KisVersioning::RemoteProjects);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Pair pair;
        QVERIFY2(setUpPair(dir.path(), &pair, {{"a.kra", "base a"}, {"b.kra", "base b"}}),
                 "could not set up two copies of the project");

        // The first person deletes a.kra and changes b.kra; the second does the opposite.
        QVERIFY(QFile::remove(QDir(pair.pathA).filePath("a.kra")));
        QVERIFY(writeFile(pair.pathA, "b.kra", "b changed by the first"));
        QVERIFY(writeFile(pair.pathB, "a.kra", "a changed by the second"));
        QVERIFY(QFile::remove(QDir(pair.pathB).filePath("b.kra")));
        VERIFY_OK(run(pair.a->synchronise(pair.pathA, "first")));

        const Outcome sync = run(pair.b->synchronise(pair.pathB, "second"));
        VERIFY_OK(sync);
        QMap<QString, KisVersioning::DivergenceKind> kinds;
        for (const KisVersioning::Divergence &divergence : sync.result.value<KisVersioning::SyncResult>().divergences) {
            kinds.insert(divergence.path, divergence.kind);
        }
        QCOMPARE(kinds.size(), 2);
        QVERIFY(kinds.value("a.kra") == KisVersioning::DivergenceKind::ModifiedHereDeletedThere);
        QVERIFY(kinds.value("b.kra") == KisVersioning::DivergenceKind::DeletedHereModifiedThere);
    }

    void testOfflineSyncIsPendingThenSentOnce()
    {
        REQUIRE_FEATURE(KisVersioning::RemoteProjects);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Pair pair;
        QVERIFY2(setUpPair(dir.path(), &pair, {{"base.kra", "base"}}), "could not set up two copies of the project");

        setOffline(pair.a.data(), true);
        QVERIFY(writeFile(pair.pathA, "novo.kra", "made offline"));
        const Outcome offline = run(pair.a->synchronise(pair.pathA, "offline work"));
        VERIFY_OK(offline);
        QVERIFY(offline.result.value<KisVersioning::SyncResult>().pendingUpload);

        setOffline(pair.a.data(), false);
        const Outcome online = run(pair.a->synchronise(pair.pathA, QString()));
        VERIFY_OK(online);
        QVERIFY(!online.result.value<KisVersioning::SyncResult>().pendingUpload);

        VERIFY_OK(run(pair.b->synchronise(pair.pathB, QString())));
        QCOMPARE(readFile(QDir(pair.pathB).filePath("novo.kra")), QByteArray("made offline"));
        QCOMPARE(historyOf(pair.b.data(), pair.pathB, "novo.kra").size(), 1);
    }

    void testReadOnlyFileOnReceiveFailsCleanly()
    {
        REQUIRE_FEATURE(KisVersioning::RemoteProjects);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Pair pair;
        QVERIFY2(setUpPair(dir.path(), &pair, {{"a.kra", "base"}}), "could not set up two copies of the project");

        QVERIFY(writeFile(pair.pathA, "a.kra", "changed by the first"));
        VERIFY_OK(run(pair.a->synchronise(pair.pathA, "first")));

        const QString lockedPath = QDir(pair.pathB).filePath("a.kra");
        QVERIFY(QFile::setPermissions(lockedPath, QFileDevice::ReadOwner));
        const Outcome sync = run(pair.b->synchronise(pair.pathB, QString()));
        // Let the file be removed with the temporary folder whatever happened.
        QFile::setPermissions(lockedPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        QVERIFY2(sync.done, "the job did not finish in time");
        QCOMPARE(sync.error, KisVersioningJob::Failed);
        QCOMPARE(readFile(lockedPath), QByteArray("base"));
    }

    // PartialDownload -----------------------------------------------------

    void testPartialDownloadBringsOnlyChosen()
    {
        REQUIRE_FEATURE(KisVersioning::PartialDownload);
        REQUIRE_FEATURE(KisVersioning::RemoteProjects);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Pair pair;
        QVERIFY2(setUpPair(dir.path(), &pair, {{"ui/a.kra", "ui base"}, {"concepts/b.kra", "concepts base"}}),
                 "could not set up two copies of the project");

        // The second person keeps only the ui folder.
        VERIFY_OK(run(pair.b->setFolders(pair.pathB, {"ui"})));

        QVERIFY(writeFile(pair.pathA, "ui/a.kra", "ui changed"));
        QVERIFY(writeFile(pair.pathA, "concepts/b.kra", "concepts changed"));
        VERIFY_OK(run(pair.a->synchronise(pair.pathA, "both folders")));

        const Outcome sync = run(pair.b->synchronise(pair.pathB, QString()));
        VERIFY_OK(sync);
        const KisVersioning::SyncResult result = sync.result.value<KisVersioning::SyncResult>();
        QVERIFY(result.received.contains("ui/a.kra"));
        QVERIFY(!result.received.contains("concepts/b.kra"));
        QCOMPARE(readFile(QDir(pair.pathB).filePath("ui/a.kra")), QByteArray("ui changed"));
        QVERIFY(readFile(QDir(pair.pathB).filePath("concepts/b.kra")) != QByteArray("concepts changed"));
    }

    // Presence ------------------------------------------------------------

    void testEditingNoticeVisibleToOther()
    {
        REQUIRE_FEATURE(KisVersioning::Presence);
        REQUIRE_FEATURE(KisVersioning::RemoteProjects);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Pair pair;
        QVERIFY2(setUpPair(dir.path(), &pair, {{"a.kra", "base"}}), "could not set up two copies of the project");

        VERIFY_OK(run(pair.a->announceEditing(pair.pathA, "a.kra", true)));
        const Outcome seen = run(pair.b->whoIsEditing(pair.pathB, {"a.kra"}));
        VERIFY_OK(seen);
        const QVector<KisVersioning::EditingInfo> editing = seen.result.value<QVector<KisVersioning::EditingInfo>>();
        QCOMPARE(editing.size(), 1);
        QCOMPARE(editing[0].path, QString("a.kra"));
        QCOMPARE(editing[0].user, pair.a->currentUser());

        VERIFY_OK(run(pair.a->announceEditing(pair.pathA, "a.kra", false)));
        const Outcome gone = run(pair.b->whoIsEditing(pair.pathB, {"a.kra"}));
        VERIFY_OK(gone);
        QVERIFY(gone.result.value<QVector<KisVersioning::EditingInfo>>().isEmpty());
    }

protected:
    /// What a finished job reported. Read inside the slot connected to finished(), because the job then deletes itself.
    struct Outcome {
        bool done = false;   ///< false when the job did not finish in time
        KisVersioningJob::Error error = KisVersioningJob::NoError;
        QString errorText;
        QVariant result;
    };

    /// Waits for @p job, with a limit so that a hung driver fails the test instead of hanging it.
    static Outcome run(KisVersioningJob *job, int timeoutMs = 30000)
    {
        Outcome outcome;
        if (!job) {
            return outcome;
        }
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(job, &KisVersioningJob::finished, &loop, [&outcome, &loop](KisVersioningJob *finished) {
            outcome.done = true;
            outcome.error = finished->error();
            outcome.errorText = finished->errorText();
            outcome.result = finished->result<QVariant>();
            loop.quit();
        });
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();
        return outcome;
    }

    static bool writeFile(const QString &root, const QString &relativePath, const QByteArray &bytes)
    {
        const QString absolute = QDir(root).filePath(relativePath);
        if (!QDir().mkpath(QFileInfo(absolute).absolutePath())) {
            return false;
        }
        QFile file(absolute);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        const bool ok = file.write(bytes) == bytes.size();
        file.close();
        return ok;
    }

    static QByteArray readFile(const QString &absolutePath)
    {
        QFile file(absolutePath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    QVector<KisVersioning::VersionInfo> historyOf(KisVersioningDriver *driver, const QString &project, const QString &path)
    {
        const Outcome outcome = run(driver->fileHistory(project, path, 0, 100));
        return outcome.done && outcome.error == KisVersioningJob::NoError
            ? outcome.result.value<QVector<KisVersioning::VersionInfo>>()
            : QVector<KisVersioning::VersionInfo>();
    }

    /// Bytes that no tool takes for text: a zip signature, as a real .kra starts, a marker and
    /// filler with zero and high bytes.
    static QByteArray binaryContent(const QByteArray &marker)
    {
        QByteArray bytes("PK\x03\x04\x14\x00\x00\x00", 8);
        bytes += marker;
        quint32 x = 2166136261u;
        for (char c : marker) {
            x = (x ^ quint8(c)) * 16777619u;
        }
        for (int i = 0; i < 4096; ++i) {
            x = x * 1103515245u + 12345u;
            bytes += char(x >> 16);
        }
        return bytes;
    }

    /// Rows "base", "first" and "second" for the divergence tests: binary content, text where both
    /// change the same line, and text where each changes a different line.
    static void addDivergenceContents()
    {
        QTest::addColumn<QByteArray>("base");
        QTest::addColumn<QByteArray>("first");
        QTest::addColumn<QByteArray>("second");
        const QByteArray lines("line 1\nline 2\nline 3\nline 4\nline 5\nline 6\n");
        QTest::newRow("binary") << binaryContent("base") << binaryContent("first") << binaryContent("second");
        QTest::newRow("text, same line") << lines
                                         << QByteArray("line 1 by the first\nline 2\nline 3\nline 4\nline 5\nline 6\n")
                                         << QByteArray("line 1 by the second\nline 2\nline 3\nline 4\nline 5\nline 6\n");
        QTest::newRow("text, different lines") << lines
                                               << QByteArray("line 1 by the first\nline 2\nline 3\nline 4\nline 5\nline 6\n")
                                               << QByteArray("line 1\nline 2\nline 3\nline 4\nline 5\nline 6 by the second\n");
    }

    bool driverHas(KisVersioning::Capability capability)
    {
        QScopedPointer<KisVersioningDriver> probe(createDriver(nullptr));
        return probe && probe->capabilities().testFlag(capability);
    }

    /// Two people working on one project, each with a driver and a local path.
    struct Pair {
        QScopedPointer<KisVersioningDriver> a;
        QScopedPointer<KisVersioningDriver> b;
        QString pathA;
        QString pathB;
    };

    /// Creates the project for the first person with @p files already synchronised, then clones it for the second.
    bool setUpPair(const QString &root, Pair *pair, const QMap<QString, QByteArray> &files)
    {
        pair->a.reset(createDriver(nullptr));
        pair->b.reset(createDriver(nullptr));
        if (!pair->a || !pair->b) {
            return false;
        }
        pair->pathA = QDir(root).filePath("first");
        pair->pathB = QDir(root).filePath("second");
        if (!createProject(pair->a.data(), pair->pathA)) {
            return false;
        }
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            if (!writeFile(pair->pathA, it.key(), it.value())) {
                return false;
            }
        }
        const Outcome base = run(pair->a->synchronise(pair->pathA, QStringLiteral("base")));
        if (!base.done || base.error != KisVersioningJob::NoError) {
            return false;
        }
        if (!cloneProject(pair->b.data(), pair->pathB)) {
            return false;
        }
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            if (readFile(QDir(pair->pathB).filePath(it.key())) != it.value()) {
                return false;
            }
        }
        return true;
    }
};


#endif // KISVERSIONINGDRIVERCONFORMANCE_H
