/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningJobTest.h"

#include <QSignalSpy>
#include <QThread>
#include <QDateTime>
#include <QVariant>

#include <KisVersioningJob.h>
#include <KisVersioningDriver.h>
#include <KisVersioningDriverRegistry.h>
#include <KisVersioningTypes.h>

namespace {
class FakeFactory : public KisVersioningDriverFactory
{
public:
    QString id() const override { return QStringLiteral("fake"); }
    QString name() const override { return QStringLiteral("Fake"); }
    KisVersioningDriver *create(QObject *) const override { return nullptr; }
};
}

void KisVersioningJobTest::testFinishDeliversResultOnce()
{
    KisVersioningJob *job = new KisVersioningJob;
    QSignalSpy spy(job, &KisVersioningJob::finished);
    job->finish(QVariant::fromValue(QStringList{"ui", "concepts"}));
    QCOMPARE(spy.count(), 0);                       // never synchronously
    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(job->error(), KisVersioningJob::NoError);
    QCOMPARE(job->result<QStringList>(), (QStringList{"ui", "concepts"}));
    job->finish();                                  // the second call is ignored
    QTest::qWait(50);
    QCOMPARE(spy.count(), 1);
}

void KisVersioningJobTest::testFailCarriesErrorAndText()
{
    KisVersioningJob *job = new KisVersioningJob;
    QSignalSpy spy(job, &KisVersioningJob::finished);
    QVERIFY(!job->isFinished());
    job->fail(KisVersioningJob::Offline, "x");
    QCOMPARE(spy.count(), 0);
    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(job->error(), KisVersioningJob::Offline);
    QCOMPARE(job->errorText(), QString("x"));
    QVERIFY(job->isFinished());
    job->finish();                                  // already failed: ignored
    QCOMPARE(job->error(), KisVersioningJob::Offline);
}

void KisVersioningJobTest::testFinishFromAnotherThread()
{
    KisVersioningJob *job = new KisVersioningJob;
    QThread *mainThread = QThread::currentThread();
    QThread *deliveredOn = nullptr;
    connect(job, &KisVersioningJob::finished, this, [&](KisVersioningJob *) {
        deliveredOn = QThread::currentThread();
    });
    QSignalSpy spy(job, &KisVersioningJob::finished);

    QThread *worker = QThread::create([job]() { job->finish(QVariant(42)); });
    worker->start();
    QVERIFY(spy.wait(2000));
    worker->wait();
    delete worker;

    QCOMPARE(deliveredOn, mainThread);
    QCOMPARE(job->result<int>(), 42);
}

void KisVersioningJobTest::testCancelIsOnlyARequest()
{
    KisVersioningJob *job = new KisVersioningJob;
    QSignalSpy spy(job, &KisVersioningJob::finished);
    QVERIFY(!job->cancelRequested());
    job->requestCancel();
    QVERIFY(job->cancelRequested());
    QTest::qWait(50);
    QCOMPARE(spy.count(), 0);
    QVERIFY(!job->isFinished());

    job->fail(KisVersioningJob::Cancelled, "stopped");
    QVERIFY(spy.wait(1000));
    QCOMPARE(job->error(), KisVersioningJob::Cancelled);
}

void KisVersioningJobTest::testRegistryFindsAddedFactory()
{
    KisVersioningDriverRegistry *registry = KisVersioningDriverRegistry::instance();
    QVERIFY(registry);
    FakeFactory *f = new FakeFactory;
    registry->add(f);
    QCOMPARE(registry->value("fake"), static_cast<KisVersioningDriverFactory *>(f));
}

void KisVersioningJobTest::testTypesTravelInVariant()
{
    using namespace KisVersioning;

    SyncResult sync;
    Divergence d;
    d.path = "a.kra";
    d.kind = DivergenceKind::DeletedHereModifiedThere;
    d.otherAuthor = "someone";
    sync.divergences.append(d);
    sync.received = QStringList{"b.kra"};
    sync.sent = QStringList{"c.kra"};
    sync.pendingUpload = true;
    sync.serverChanged = true;

    QVariant v = QVariant::fromValue(sync);
    const SyncResult back = v.value<SyncResult>();
    QCOMPARE(back.divergences.size(), 1);
    QCOMPARE(back.divergences[0].path, QString("a.kra"));
    QCOMPARE(back.divergences[0].kind, DivergenceKind::DeletedHereModifiedThere);
    QCOMPARE(back.divergences[0].otherAuthor, QString("someone"));
    QCOMPARE(back.received, QStringList{"b.kra"});
    QCOMPARE(back.sent, QStringList{"c.kra"});
    QVERIFY(back.pendingUpload);
    QVERIFY(back.serverChanged);

    VersionInfo info;
    info.id = "abc";
    info.number = 3;
    info.author = "me";
    info.date = QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5), Qt::UTC);
    info.notes = "n";
    info.isMerge = true;
    QVector<VersionInfo> history{info};

    KisVersioningJob job;
    job.finish(QVariant::fromValue(history));
    const QVector<VersionInfo> got = job.result<QVector<VersionInfo>>();
    QCOMPARE(got.size(), 1);
    QCOMPARE(got[0].id, QString("abc"));
    QCOMPARE(got[0].number, 3);
    QCOMPARE(got[0].date, info.date);
    QVERIFY(got[0].isMerge);

    QVariant fs = QVariant::fromValue(FileStatus{"p", FileState::Modified});
    QCOMPARE(fs.value<FileStatus>().state, FileState::Modified);
    QVERIFY(QVariant::fromValue(ProjectStatus()).isValid());
    QVERIFY(QVariant::fromValue(ProjectInfo()).isValid());
    QVERIFY(QVariant::fromValue(EditingInfo()).isValid());
}

SIMPLE_TEST_MAIN(KisVersioningJobTest)
