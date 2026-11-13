/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsPresenceTest.h"

#include <simpletest.h>

#include <QCoreApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KisVersioningSettings.h>

#include "KisVersioningFakeDriver.h"
#include "ProjectsPresence.h"

using namespace KisVersioning;

namespace {

struct Fixture {
    QTemporaryDir dir;
    KSharedConfigPtr config;
    KisVersioningSettings settings;
    KisVersioningFakeDriver driver;
    ProjectsPresence presence;

    Fixture()
        : config(KSharedConfig::openConfig(dir.filePath(QStringLiteral("settings")), KConfig::SimpleConfig))
        , settings(config)
        , presence(&driver, &settings)
    {
        KisVersioningSettings::Project project;
        project.driverId = QStringLiteral("fake");
        project.info.id = QStringLiteral("game");
        project.info.name = QStringLiteral("game");
        project.localPath = dir.filePath(QStringLiteral("game"));
        settings.addProject(project);
        driver.setCapabilities(Presence);
        driver.setCurrentUser(QStringLiteral("Ana"));
    }

    QString project() const { return dir.filePath(QStringLiteral("game")); }
    QString file(const QString &relative) const { return dir.filePath(QStringLiteral("game/") + relative); }

    /// Lets the jobs of the fake driver deliver their results.
    static void settle()
    {
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
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

} // namespace

void ProjectsPresenceTest::init()
{
    // The assertions use the English source strings, whatever the machine's language.
    KLocalizedString::setLanguages({QStringLiteral("en_US")});
}

void ProjectsPresenceTest::testFirstChangeAnnouncesOnce()
{
    Fixture f;
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();

    QCOMPARE(f.driver.callCount(QStringLiteral("announceEditing")), 1);
    const KisVersioningFakeDriver::Call call = f.driver.callsNamed(QStringLiteral("announceEditing")).first();
    QCOMPARE(call.args.at(0), f.project());
    QCOMPARE(call.args.at(1), QStringLiteral("ui/botao.kra"));
    QCOMPARE(call.args.at(2), QStringLiteral("true"));
}

void ProjectsPresenceTest::testFileOutsideProjectsIsIgnored()
{
    Fixture f;
    QTemporaryDir other;
    f.presence.documentModified(other.filePath(QStringLiteral("loose.kra")));
    f.presence.documentModified(f.project() + QStringLiteral("-copy/botao.kra"));
    f.presence.documentActivated(other.filePath(QStringLiteral("loose.kra")));
    f.presence.documentClosed(other.filePath(QStringLiteral("loose.kra")), false);
    f.presence.documentModified(QString());
    Fixture::settle();

    QVERIFY(f.driver.calls().isEmpty());
}

void ProjectsPresenceTest::testNeverSyncedFileIsIgnored()
{
    Fixture f;
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra~")));
    f.presence.documentModified(f.file(QStringLiteral("ui/.botao.kra-autosave.kra")));
    Fixture::settle();

    QVERIFY(f.driver.calls().isEmpty());
}

void ProjectsPresenceTest::testSyncWithdrawsNotice()
{
    Fixture f;
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    f.presence.documentModified(f.file(QStringLiteral("art/heroi.kra")));
    Fixture::settle();

    f.presence.projectSynchronised(f.project());
    Fixture::settle();

    // Each file is withdrawn once; the order among files is not specified.
    QStringList sent = f.announcements();
    QCOMPARE(sent.mid(0, 2), (QStringList{QStringLiteral("ui/botao.kra:true"), QStringLiteral("art/heroi.kra:true")}));
    QStringList withdrawn = sent.mid(2);
    withdrawn.sort();
    QCOMPARE(withdrawn, (QStringList{QStringLiteral("art/heroi.kra:false"), QStringLiteral("ui/botao.kra:false")}));

    // Nothing is left to withdraw, and the file can be announced again.
    f.presence.projectSynchronised(f.project());
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();
    QCOMPARE(f.driver.callCount(QStringLiteral("announceEditing")), 5);
}

void ProjectsPresenceTest::testCloseWithoutChangesWithdraws()
{
    Fixture f;
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();
    f.presence.documentClosed(f.file(QStringLiteral("ui/botao.kra")), false);
    Fixture::settle();

    QCOMPARE(f.announcements(), (QStringList{QStringLiteral("ui/botao.kra:true"), QStringLiteral("ui/botao.kra:false")}));

    // A file that was never announced is not withdrawn.
    f.presence.documentClosed(f.file(QStringLiteral("ui/menu.kra")), false);
    QCOMPARE(f.driver.callCount(QStringLiteral("announceEditing")), 2);
}

void ProjectsPresenceTest::testCloseWithChangesKeepsNotice()
{
    Fixture f;
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();
    f.presence.documentClosed(f.file(QStringLiteral("ui/botao.kra")), true);
    Fixture::settle();

    QCOMPARE(f.announcements(), (QStringList{QStringLiteral("ui/botao.kra:true")}));
    // The notice is still held, so a synchronisation withdraws it.
    f.presence.projectSynchronised(f.project());
    QCOMPARE(f.driver.callCount(QStringLiteral("announceEditing")), 2);
}

void ProjectsPresenceTest::testOfflineIsSkippedSilently()
{
    Fixture f;
    f.driver.setAnnounceOutcome(KisVersioningJob::Offline, QStringLiteral("No connection"));
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();
    QCOMPARE(f.driver.callCount(QStringLiteral("announceEditing")), 1);

    // The connection is back: the next change is announced.
    f.driver.setAnnounceOutcome(KisVersioningJob::NoError);
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();
    QCOMPARE(f.driver.callCount(QStringLiteral("announceEditing")), 2);

    // A notice that was never registered is not withdrawn at the close; a registered one is.
    f.presence.documentClosed(f.file(QStringLiteral("ui/botao.kra")), false);
    QCOMPARE(f.driver.callCount(QStringLiteral("announceEditing")), 3);
}

void ProjectsPresenceTest::testRefusalIsSilentAndNextAnnouncementIsAttempted()
{
    Fixture f;
    QSignalSpy spy(&f.presence, &ProjectsPresence::someoneElseIsEditing);
    f.driver.setAnnounceOutcome(KisVersioningJob::Failed, QStringLiteral("Refused"));
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();
    f.presence.documentClosed(f.file(QStringLiteral("ui/botao.kra")), false);
    f.presence.projectSynchronised(f.project());
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();

    // Two announcements, no withdrawal of the notice of another person, nothing reported.
    QCOMPARE(f.announcements(), (QStringList{QStringLiteral("ui/botao.kra:true"), QStringLiteral("ui/botao.kra:true")}));
    QCOMPARE(spy.count(), 0);
}

void ProjectsPresenceTest::testDriverWithoutPresenceDoesNothing()
{
    Fixture f;
    f.driver.setCapabilities(Capabilities());
    f.presence.documentModified(f.file(QStringLiteral("ui/botao.kra")));
    f.presence.documentActivated(f.file(QStringLiteral("ui/botao.kra")));
    f.presence.documentClosed(f.file(QStringLiteral("ui/botao.kra")), false);
    f.presence.projectSynchronised(f.project());
    Fixture::settle();

    QVERIFY(f.driver.calls().isEmpty());
}

void ProjectsPresenceTest::testActivationReportsOtherUser()
{
    Fixture f;
    f.driver.setEditing({EditingInfo{QStringLiteral("ui/botao.kra"), QStringLiteral("Bruno")}});
    QSignalSpy spy(&f.presence, &ProjectsPresence::someoneElseIsEditing);
    f.presence.documentActivated(f.file(QStringLiteral("ui/botao.kra")));
    QVERIFY(spy.wait(2000));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toString(), f.file(QStringLiteral("ui/botao.kra")));
    QCOMPARE(spy.first().at(1).toString(), QStringLiteral("Bruno"));
    const KisVersioningFakeDriver::Call call = f.driver.callsNamed(QStringLiteral("whoIsEditing")).first();
    QCOMPARE(call.args.at(0), f.project());
    QCOMPARE(call.args.at(1), QStringLiteral("ui/botao.kra"));
}

void ProjectsPresenceTest::testOwnNoticeIsNotReported()
{
    Fixture f;
    f.driver.setEditing({EditingInfo{QStringLiteral("ui/botao.kra"), QStringLiteral("Ana")}});
    QSignalSpy spy(&f.presence, &ProjectsPresence::someoneElseIsEditing);
    f.presence.documentActivated(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();
    QTest::qWait(50);

    QCOMPARE(f.driver.callCount(QStringLiteral("whoIsEditing")), 1);
    QCOMPARE(spy.count(), 0);
}

void ProjectsPresenceTest::testActivationFailureIsSilent()
{
    Fixture f;
    f.driver.setWhoIsEditingError(KisVersioningJob::Offline, QStringLiteral("No connection"));
    QSignalSpy spy(&f.presence, &ProjectsPresence::someoneElseIsEditing);
    f.presence.documentActivated(f.file(QStringLiteral("ui/botao.kra")));
    Fixture::settle();
    QTest::qWait(50);

    QCOMPARE(spy.count(), 0);
}

void ProjectsPresenceTest::testDestroyedWithJobsPending()
{
    Fixture f;
    f.driver.setEditing({EditingInfo{QStringLiteral("ui/botao.kra"), QStringLiteral("Bruno")}});
    {
        ProjectsPresence shortLived(&f.driver, &f.settings);
        shortLived.documentModified(f.file(QStringLiteral("ui/botao.kra")));
        shortLived.documentActivated(f.file(QStringLiteral("ui/botao.kra")));
    }
    // The jobs end after the helper is gone: nothing may touch it.
    Fixture::settle();
    QTest::qWait(50);
    QCOMPARE(f.driver.callCount(QStringLiteral("announceEditing")), 1);
}

SIMPLE_TEST_MAIN(ProjectsPresenceTest)
