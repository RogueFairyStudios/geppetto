/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsControllerTest.h"

#include <simpletest.h>

#include <memory>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KisVersioningConflictMarkers.h>
#include <KisVersioningSettings.h>

#include "KisVersioningFakeDriver.h"
#include "ProjectsController.h"
#include "ProjectsDocumentHost.h"
#include "ProjectsDocuments.h"
#include "ProjectsModel.h"

using namespace KisVersioning;

namespace {

/// The documents open in a pretend application. Saves end on the next turn of the event loop.
class FakeDocuments : public ProjectsDocuments
{
public:
    QHash<QString, QStringList> modified;   ///< modified documents per project folder
    QStringList open;                       ///< every open document
    QStringList foreign;                    ///< modified documents not in Krita's own format

    QStringList modifiedOpenDocuments(const QString &projectPath) const override
    {
        return modified.value(projectPath);
    }
    void saveDocument(const QString &, std::function<void(bool)> done) override
    {
        QTimer::singleShot(0, [done]() { done(true); });
    }
    bool isOpenAndUnmodified(const QString &path) const override
    {
        return open.contains(path);
    }
    void reloadDocument(const QString &) override
    {
    }
    bool savedSince(const QString &, const QDateTime &) const override
    {
        return false;
    }
    QStringList openDocumentsIn(const QString &folder) const override
    {
        QStringList inside;
        for (const QString &path : open) {
            if (ProjectsDocumentHost::isInside(path, folder)) {
                inside << path;
            }
        }
        return inside;
    }
    QStringList unsavedForeignDocuments(const QString &projectPath) const override
    {
        QStringList inside;
        for (const QString &path : foreign) {
            if (ProjectsDocumentHost::isInside(path, projectPath)) {
                inside << path;
            }
        }
        return inside;
    }
};

struct Fixture {
    QTemporaryDir dir;
    KSharedConfigPtr config;
    KisVersioningSettings settings;
    KisVersioningFakeDriver driver;
    FakeDocuments documents;
    std::unique_ptr<ProjectsController> controller;   // last: it goes first

    Fixture()
        : config(KSharedConfig::openConfig(dir.filePath(QStringLiteral("settings")), KConfig::SimpleConfig))
        , settings(config)
    {
        settings.setProjectsRoot(dir.filePath(QStringLiteral("projects")));
    }

    QString path(const QString &relative) const
    {
        return dir.filePath(relative);
    }

    void write(const QString &relative, const QByteArray &content = "content")
    {
        QDir().mkpath(QFileInfo(path(relative)).absolutePath());
        QFile file(path(relative));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
    }

    /// "game", downloaded, with the folders "ui" and "art" on disk.
    void addGame(const QStringList &folders = {})
    {
        write(QStringLiteral("game/ui/botao.kra"));
        write(QStringLiteral("game/art/heroi.kra"));
        KisVersioningSettings::Project project;
        project.driverId = QStringLiteral("fake");
        project.info.id = QStringLiteral("game");
        project.info.name = QStringLiteral("game");
        project.localPath = path(QStringLiteral("game"));
        project.folders = folders;
        settings.addProject(project);
    }

    ProjectsController &make()
    {
        controller.reset(new ProjectsController(&driver, &documents, &settings));
        controller->setSaveRefreshDelay(10);
        return *controller;
    }

    bool startAndWait()
    {
        QSignalSpy spy(controller->model(), &ProjectsModel::refreshed);
        controller->start();
        return spy.wait(5000);
    }

    /// Runs a synchronisation to its end, and the refresh that follows it.
    bool runAndWait(const QString &notes = QString())
    {
        QSignalSpy finished(controller.get(), &ProjectsController::runFinished);
        QSignalSpy refreshed(controller->model(), &ProjectsModel::refreshed);
        controller->synchronise(notes);
        return (!finished.isEmpty() || finished.wait(5000)) && (!refreshed.isEmpty() || refreshed.wait(5000));
    }

    /// The announcements received so far, as "path:true" or "path:false".
    QStringList announcements() const
    {
        QStringList list;
        const QList<KisVersioningFakeDriver::Call> calls = driver.callsNamed(QStringLiteral("announceEditing"));
        for (const KisVersioningFakeDriver::Call &call : calls) {
            list << call.args.at(1) + QLatin1Char(':') + call.args.at(2);
        }
        return list;
    }
};

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

SyncResult pendingResult()
{
    SyncResult result;
    result.pendingUpload = true;
    return result;
}

} // namespace

void ProjectsControllerTest::init()
{
    // The assertions use the English source strings, whatever the machine's language.
    KLocalizedString::setLanguages({QStringLiteral("en_US")});
}

void ProjectsControllerTest::testPendingUploadFromARunStaysUntilSent()
{
    Fixture f;
    f.addGame();
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());
    QVERIFY(!controller.pendingUpload());

    // No network: the version stays on this computer. The state the driver gives knows nothing of
    // it, the run does.
    f.driver.queueSyncResult(pendingResult());
    QVERIFY(f.runAndWait());
    QCOMPARE(controller.model()->pendingUploadCount(), 0);
    QVERIFY(controller.pendingUpload());
    QVERIFY(controller.messages().contains(
        QStringLiteral("Your work is kept on this computer and will be sent the next time you synchronize.")));

    // A run that fails proves nothing: the work is still shown as waiting.
    f.driver.queueSyncError(KisVersioningJob::Failed, QStringLiteral("could not"));
    QVERIFY(f.runAndWait());
    QVERIFY(controller.pendingUpload());
    QCOMPARE(controller.messages(), QStringList{QStringLiteral("could not")});

    // With no network the driver says Offline: still waiting.
    f.driver.queueSyncError(KisVersioningJob::Offline, QStringLiteral("no network"));
    QVERIFY(f.runAndWait());
    QVERIFY(controller.pendingUpload());

    // A run that sends everything clears it.
    f.driver.queueSyncResult(SyncResult());
    QVERIFY(f.runAndWait());
    QVERIFY(!controller.pendingUpload());
    QVERIFY(controller.messages().isEmpty());

    // The state of the project alone also shows it.
    ProjectStatus status;
    status.pendingUpload = 2;
    f.driver.setStatus(status);
    controller.requestRefresh();
    QTRY_VERIFY(controller.pendingUpload());
}

void ProjectsControllerTest::testNoticesStayWhileWorkIsPending()
{
    Fixture f;
    f.addGame();
    f.driver.setCapabilities(Presence);
    f.driver.setCurrentUser(QStringLiteral("Ana"));
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());

    controller.documentModified(f.path(QStringLiteral("game/ui/botao.kra")));
    QTRY_COMPARE(f.announcements(), QStringList{QStringLiteral("ui/botao.kra:true")});

    // The work was kept on this computer, or did not go at all: the notice stays.
    f.driver.queueSyncResult(pendingResult());
    QVERIFY(f.runAndWait());
    f.driver.queueSyncError(KisVersioningJob::Offline, QStringLiteral("no network"));
    QVERIFY(f.runAndWait());
    QTest::qWait(50);
    QCOMPARE(f.announcements(), QStringList{QStringLiteral("ui/botao.kra:true")});

    // Everything was sent: it goes.
    f.driver.queueSyncResult(SyncResult());
    QVERIFY(f.runAndWait());
    QTRY_VERIFY(f.announcements().contains(QStringLiteral("ui/botao.kra:false")));
}

void ProjectsControllerTest::testNothingElseReachesTheDriverDuringARun()
{
    Fixture f;
    f.addGame({QStringLiteral("ui"), QStringLiteral("art")});
    f.driver.setCapabilities(RemoteProjects | PartialDownload | Presence);
    f.driver.setCurrentUser(QStringLiteral("Ana"));
    f.driver.setProjects({ProjectInfo{QStringLiteral("game"), QStringLiteral("game"), QString()}});
    f.driver.setProjectFolders(QStringLiteral("game"), {QStringLiteral("art"), QStringLiteral("ui")});
    f.write(QStringLiteral("game/ui/botao (Bruno).kra"));
    KisVersioningConflictMarkers markers(f.path(QStringLiteral("game")));
    KisVersioningConflictMarkers::Marker marker;
    marker.copyPath = QStringLiteral("ui/botao (Bruno).kra");
    marker.originalPath = QStringLiteral("ui/botao.kra");
    marker.date = QDateTime::currentDateTime();
    QVERIFY(markers.add(marker));
    QDir().mkpath(f.path(QStringLiteral("other")));
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());
    QVERIFY(controller.canSynchronise());
    QVERIFY(controller.canChooseFolders());
    ProjectsModel *model = controller.model();
    const QModelIndex art = findPath(*model, f.path(QStringLiteral("game/art")));
    QVERIFY(model->flags(art) & Qt::ItemIsUserCheckable);

    f.driver.setHeld(QStringLiteral("synchronise"), true);
    QSignalSpy finished(&controller, &ProjectsController::runFinished);
    controller.synchronise(QString());
    QVERIFY(controller.isSynchronising());
    QTRY_COMPARE(f.driver.heldCount(QStringLiteral("synchronise")), 1);
    const int callsBefore = f.driver.calls().size();

    // Everything that would reach the driver waits, or is refused.
    QVERIFY(!controller.canSynchronise());
    QVERIFY(!controller.canChooseFolders());
    controller.requestRefresh();
    controller.documentSaved(f.path(QStringLiteral("game/ui/botao.kra")));
    controller.documentModified(f.path(QStringLiteral("game/ui/botao.kra")));
    controller.documentActivated(f.path(QStringLiteral("game/art/heroi.kra")));
    QVERIFY(!(model->flags(art) & Qt::ItemIsUserCheckable));
    QVERIFY(!model->setData(art, Qt::Unchecked, Qt::CheckStateRole));
    controller.addFolder(f.path(QStringLiteral("other")));
    controller.markConflictResolved(findPath(*model, f.path(QStringLiteral("game/ui/botao (Bruno).kra"))));
    controller.synchronise(QString());
    QTest::qWait(100);
    QCOMPARE(f.driver.calls().size(), callsBefore);
    QCOMPARE(markers.list().size(), 1);
    QCOMPARE(f.settings.projects().size(), 1);

    // The run ends: the list is read once, and what waited is sent.
    const int statusBefore = f.driver.callCount(QStringLiteral("status"));
    QSignalSpy refreshed(model, &ProjectsModel::refreshed);
    f.driver.releaseHeld(QStringLiteral("synchronise"));
    QVERIFY(finished.wait(5000));
    QVERIFY(refreshed.wait(5000));
    QTest::qWait(100);
    QCOMPARE(f.driver.callCount(QStringLiteral("status")), statusBefore + 1);
    QCOMPARE(refreshed.size(), 1);
    QCOMPARE(f.driver.callCount(QStringLiteral("synchronise")), 1);
    QCOMPARE(f.announcements().first(), QStringLiteral("ui/botao.kra:true"));
    QVERIFY(controller.canSynchronise());
    QVERIFY(controller.canChooseFolders());
    QVERIFY(model->flags(findPath(*model, f.path(QStringLiteral("game/art")))) & Qt::ItemIsUserCheckable);
}

void ProjectsControllerTest::testRunWaitsForTheListBeingRead()
{
    Fixture f;
    f.addGame();
    ProjectsController &controller = f.make();
    f.driver.setHeld(QStringLiteral("status"), true);
    controller.start();
    QTRY_COMPARE(f.driver.heldCount(QStringLiteral("status")), 1);
    QVERIFY(controller.model()->isRefreshing());

    QSignalSpy finished(&controller, &ProjectsController::runFinished);
    controller.synchronise(QString());
    QVERIFY(controller.isSynchronising());
    QTest::qWait(100);
    QCOMPARE(f.driver.callCount(QStringLiteral("synchronise")), 0);

    // The list arrives: the run starts.
    f.driver.setHeld(QStringLiteral("status"), false);
    f.driver.releaseHeld(QStringLiteral("status"));
    QVERIFY(finished.wait(5000));
    QCOMPARE(f.driver.callCount(QStringLiteral("synchronise")), 1);
}

void ProjectsControllerTest::testOneRunAtATime()
{
    Fixture f;
    f.addGame();
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());
    f.driver.setHeld(QStringLiteral("synchronise"), true);
    controller.synchronise(QStringLiteral("first"));
    controller.synchronise(QStringLiteral("second"));
    QTRY_COMPARE(f.driver.heldCount(QStringLiteral("synchronise")), 1);
    QTest::qWait(50);
    QCOMPARE(f.driver.callCount(QStringLiteral("synchronise")), 1);
    QCOMPARE(f.driver.callsNamed(QStringLiteral("synchronise")).first().args.at(1), QStringLiteral("first"));

    QSignalSpy finished(&controller, &ProjectsController::runFinished);
    f.driver.releaseHeld(QStringLiteral("synchronise"));
    QVERIFY(finished.wait(5000));
    QCOMPARE(finished.size(), 1);
}

void ProjectsControllerTest::testMessagesAfterARun()
{
    Fixture f;
    f.addGame();
    f.driver.setCurrentUser(QStringLiteral("Bruno"));
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());

    SyncResult diverged;
    Divergence divergence;
    divergence.path = QStringLiteral("ui/botao.kra");
    divergence.kind = DivergenceKind::BothModified;
    diverged.divergences.append(divergence);
    f.driver.queueSyncResult(diverged);
    QSignalSpy finished(&controller, &ProjectsController::runFinished);
    QVERIFY(f.runAndWait());
    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.first().first().toBool(), true);
    QCOMPARE(controller.messages(), QStringList{QStringLiteral("Both versions of botao.kra were kept")});

    // A run that fails says why, and that it did not go well.
    f.driver.queueSyncError(KisVersioningJob::Failed, QStringLiteral("the folder is gone"));
    QVERIFY(f.runAndWait());
    QCOMPARE(finished.last().first().toBool(), false);
    QCOMPARE(controller.messages(), QStringList{QStringLiteral("the folder is gone")});
}

void ProjectsControllerTest::testDocumentNotReloadedIsWarned()
{
    Fixture f;
    f.addGame();
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());
    const QString warning = QStringLiteral("botao.kra was replaced by the other version while you kept working. "
                                           "Save your work under another name.");

    controller.documentNotReloaded(f.path(QStringLiteral("game/ui/botao.kra")));
    QCOMPARE(controller.messages(), QStringList{warning});

    // During a run, it comes with the run's outcome.
    f.driver.setHeld(QStringLiteral("synchronise"), true);
    QSignalSpy finished(&controller, &ProjectsController::runFinished);
    controller.synchronise(QString());
    QTRY_COMPARE(f.driver.heldCount(QStringLiteral("synchronise")), 1);
    controller.documentNotReloaded(f.path(QStringLiteral("game/ui/botao.kra")));
    f.driver.releaseHeld(QStringLiteral("synchronise"));
    QVERIFY(finished.wait(5000));
    QCOMPARE(controller.messages(), QStringList{warning});
}

void ProjectsControllerTest::testSignedOutStopsRunsAndChoices()
{
    Fixture f;
    f.addGame();
    f.driver.setCapabilities(SignIn | RemoteProjects);
    f.driver.setCurrentUser(QStringLiteral("Ana"));
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());
    QCOMPARE(controller.account(), ProjectsController::Account::SignedIn);
    QVERIFY(controller.canSynchronise());

    // The session expired during a run: back to signed out, with nothing more allowed.
    f.driver.queueSyncError(KisVersioningJob::NotSignedIn, QStringLiteral("expired"));
    QVERIFY(f.runAndWait());
    QCOMPARE(controller.account(), ProjectsController::Account::SignedOut);
    QCOMPARE(controller.messages(), QStringList{QStringLiteral("Sign in again to send your work")});
    QVERIFY(!controller.canSynchronise());
    QVERIFY(!controller.canChooseFolders());
    QVERIFY(controller.pendingUpload());

    // Signing in again allows them again.
    QSignalSpy refreshed(controller.model(), &ProjectsModel::refreshed);
    controller.signIn(QStringLiteral("server.example"));
    QVERIFY(refreshed.wait(5000));
    QCOMPARE(controller.account(), ProjectsController::Account::SignedIn);
    QVERIFY(controller.canSynchronise());
    QVERIFY(controller.canChooseFolders());
    QCOMPARE(f.settings.serverAddress(), QStringLiteral("server.example"));
}

void ProjectsControllerTest::testWhoIsSignedInIsAskedAtStart()
{
    Fixture f;
    f.addGame();
    f.driver.setCapabilities(SignIn | RemoteProjects);
    f.settings.setServerAddress(QStringLiteral("server.example"));
    // The application starts: the driver does not know anyone yet, its backend kept a session.
    f.driver.setResumedUser(QStringLiteral("Ana"));
    ProjectsController &controller = f.make();
    QCOMPARE(controller.account(), ProjectsController::Account::SignedOut);
    QVERIFY(f.startAndWait());

    QTRY_COMPARE(controller.account(), ProjectsController::Account::SignedIn);
    QCOMPARE(controller.user(), QStringLiteral("Ana"));
    QCOMPARE(f.driver.callsNamed(QStringLiteral("resumeSession")).first().args, QStringList{QStringLiteral("server.example")});
    // Asked without the artist: no sign-in, no browser.
    QCOMPARE(f.driver.callCount(QStringLiteral("signIn")), 0);
    QVERIFY(controller.canSynchronise());
    QVERIFY(controller.canChooseFolders());

    // While the answer is awaited, nothing starts.
    Fixture g;
    g.addGame();
    g.driver.setCapabilities(SignIn | RemoteProjects);
    g.settings.setServerAddress(QStringLiteral("server.example"));
    g.driver.setHeld(QStringLiteral("resumeSession"), true);
    ProjectsController &waiting = g.make();
    QVERIFY(g.startAndWait());
    QCOMPARE(waiting.account(), ProjectsController::Account::Checking);
    QVERIFY(!waiting.canSynchronise());
    QVERIFY(!waiting.canChooseFolders());

    // A driver without sign-in is never asked.
    Fixture h;
    h.addGame();
    h.settings.setServerAddress(QStringLiteral("server.example"));
    ProjectsController &none = h.make();
    QVERIFY(h.startAndWait());
    QCOMPARE(none.account(), ProjectsController::Account::NotNeeded);
    QCOMPARE(h.driver.callCount(QStringLiteral("resumeSession")), 0);
}

void ProjectsControllerTest::testNobodySignedInStopsRunsAndChoices()
{
    Fixture f;
    f.addGame();
    f.driver.setCapabilities(SignIn | RemoteProjects);
    f.settings.setServerAddress(QStringLiteral("server.example"));
    f.driver.setResumeSessionError(KisVersioningJob::NotSignedIn, QStringLiteral("nobody"));
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());
    QTRY_COMPARE(controller.account(), ProjectsController::Account::SignedOut);

    QVERIFY(!controller.canSynchronise());
    QVERIFY(!controller.canChooseFolders());
    controller.synchronise(QString());
    QTest::qWait(50);
    QCOMPARE(f.driver.callCount(QStringLiteral("synchronise")), 0);
    QVERIFY(!controller.isSynchronising());

    // No server known yet: nobody can be signed in, and nothing is asked.
    Fixture g;
    g.addGame();
    g.driver.setCapabilities(SignIn | RemoteProjects);
    ProjectsController &fresh = g.make();
    QVERIFY(g.startAndWait());
    QCOMPARE(fresh.account(), ProjectsController::Account::SignedOut);
    QCOMPARE(g.driver.callCount(QStringLiteral("resumeSession")), 0);
}

void ProjectsControllerTest::testNoNetworkAtStartStillAllowsWork()
{
    Fixture f;
    f.addGame();
    f.driver.setCapabilities(SignIn | RemoteProjects);
    f.settings.setServerAddress(QStringLiteral("server.example"));
    f.driver.setResumeSessionError(KisVersioningJob::Offline, QStringLiteral("no network"));
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());
    QTRY_COMPARE(controller.account(), ProjectsController::Account::Unknown);

    // The work can still be kept on this computer.
    QVERIFY(controller.canSynchronise());

    // The next reading of the list asks again; this time the session is found.
    f.driver.setResumedUser(QStringLiteral("Ana"));
    controller.requestRefresh();
    QTRY_COMPARE(controller.account(), ProjectsController::Account::SignedIn);
    QCOMPARE(controller.user(), QStringLiteral("Ana"));
}

void ProjectsControllerTest::testSignInShowsWhatItAsks()
{
    Fixture f;
    f.addGame();
    f.driver.setCapabilities(SignIn | RemoteProjects);
    f.driver.setSignInProgress(QStringLiteral("Finish signing in in the browser. The code to confirm is WDJB-MJHT."));
    f.driver.setHeld(QStringLiteral("signIn"), true);
    ProjectsController &controller = f.make();
    QVERIFY(f.startAndWait());

    controller.signIn(QStringLiteral("server.example"));
    QVERIFY(controller.isSigningIn());
    QTRY_COMPARE(controller.signInProgress(),
                 QStringLiteral("Finish signing in in the browser. The code to confirm is WDJB-MJHT."));

    QSignalSpy refreshed(controller.model(), &ProjectsModel::refreshed);
    f.driver.releaseHeld(QStringLiteral("signIn"));
    QVERIFY(refreshed.wait(5000));
    QVERIFY(!controller.isSigningIn());
    QVERIFY(controller.signInProgress().isEmpty());
    QCOMPARE(controller.account(), ProjectsController::Account::SignedIn);
}

SIMPLE_TEST_MAIN(ProjectsControllerTest)
