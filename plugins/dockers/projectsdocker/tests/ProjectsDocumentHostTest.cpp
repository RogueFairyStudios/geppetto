/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsDocumentHostTest.h"

// A real KisDocument needs the application's resources: this test uses the heavier test main.
#include <testui.h>

#include <memory>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KisDocument.h>
#include <KisPart.h>
#include <kis_config.h>

#include "ProjectsDocumentHost.h"

namespace {

/// The documents a test adds to the application; cleanup() takes them away.
QList<QPointer<KisDocument>> s_documents;

/// A document open in the application at @p path. With an image, it can be saved, as a .kra and
/// without any dialog.
KisDocument *openDocument(const QString &path, bool modified, bool withImage = false)
{
    KisDocument *document = KisPart::instance()->createDocument();
    if (withImage) {
        document->newImage(QStringLiteral("test"), 32, 32, KoColorSpaceRegistry::instance()->rgb8(), KoColor(),
                           KisConfig::RASTER_LAYER, 1, QString(), 96);
        document->setMimeType(KisDocument::nativeFormatMimeType());
        document->setFileBatchMode(true);
    }
    document->setPath(path);
    document->setModified(modified);
    KisPart::instance()->addDocument(document);
    s_documents << document;
    return document;
}

/// What a save reported.
struct SaveOutcome {
    int calls = 0;
    bool ok = false;
    bool fileWritten = false;     ///< the file had content when done() arrived
    bool documentClean = false;   ///< the document was marked as saved when done() arrived
};

std::function<void(bool)> recordInto(const std::shared_ptr<SaveOutcome> &outcome, const QString &path,
                                     KisDocument *document)
{
    const QPointer<KisDocument> watched(document);
    return [outcome, path, watched](bool ok) {
        ++outcome->calls;
        outcome->ok = ok;
        outcome->fileWritten = QFileInfo(path).size() > 0;
        outcome->documentClean = watched && !watched->isModified();
    };
}

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

} // namespace

void ProjectsDocumentHostTest::initTestCase()
{
    // The first document starts the application's tools, some of which ask for translations
    // without a domain; that warning belongs to them, not to this test.
    QLoggingCategory::setFilterRules(QStringLiteral("kf.i18n.warning=false"));
}

void ProjectsDocumentHostTest::cleanup()
{
    for (const QPointer<KisDocument> &document : std::as_const(s_documents)) {
        if (document) {
            KisPart::instance()->removeDocument(document);
        }
    }
    s_documents.clear();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void ProjectsDocumentHostTest::testPathKeyIgnoresSeparatorsAndCase()
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

void ProjectsDocumentHostTest::testModifiedDocumentsOfAProject()
{
    QTemporaryDir dir;
    const QString project = dir.filePath(QStringLiteral("game"));
    // Kept with the other separators, as the application may keep a path.
    openDocument(QDir::toNativeSeparators(dir.filePath(QStringLiteral("game/ui/a.kra"))), true);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // Kept with another case: the same folder on these systems.
    openDocument(dir.filePath(QStringLiteral("GAME/ui/B.kra")), true);
#endif
    openDocument(dir.filePath(QStringLiteral("game/ui/clean.kra")), false);
    openDocument(dir.filePath(QStringLiteral("game/ui/a.kra~")), true);       // a backup, never synchronised
    openDocument(dir.filePath(QStringLiteral("other/c.kra")), true);          // outside the project
    openDocument(dir.filePath(QStringLiteral("game2/d.kra")), true);          // a folder that only starts alike

    ProjectsDocumentHost host;
    QStringList expected{dir.filePath(QStringLiteral("game/ui/a.kra"))};
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    expected << dir.filePath(QStringLiteral("GAME/ui/B.kra"));
#endif
    expected.sort();

    QStringList found = host.modifiedOpenDocuments(project);
    found.sort();
    QCOMPARE(found, expected);

    // The project folder given with the other separators and a final separator finds the same.
    found = host.modifiedOpenDocuments(QDir::toNativeSeparators(project) + QDir::separator());
    found.sort();
    QCOMPARE(found, expected);
}

void ProjectsDocumentHostTest::testOpenAndModifiedState()
{
    QTemporaryDir dir;
    const QString modifiedPath = dir.filePath(QStringLiteral("game/a.kra"));
    const QString cleanPath = dir.filePath(QStringLiteral("game/b.kra"));
    openDocument(modifiedPath, true);
    openDocument(QDir::toNativeSeparators(cleanPath), false);

    ProjectsDocumentHost host;
    QVERIFY(host.isOpenAndModified(modifiedPath));
    QVERIFY(host.isOpenAndModified(QDir::toNativeSeparators(modifiedPath)));
    QVERIFY(!host.isOpenAndUnmodified(modifiedPath));

    QVERIFY(host.isOpenAndUnmodified(cleanPath));
    QVERIFY(!host.isOpenAndModified(cleanPath));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    QVERIFY(host.isOpenAndUnmodified(cleanPath.toUpper()));
#endif

    const QString notOpen = dir.filePath(QStringLiteral("game/c.kra"));
    QVERIFY(!host.isOpenAndModified(notOpen));
    QVERIFY(!host.isOpenAndUnmodified(notOpen));
}

void ProjectsDocumentHostTest::testSaveOfUnmodifiedDocumentSucceedsAtOnce()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("game/a.kra"));
    KisDocument *document = openDocument(path, false);

    // Saved by the artist since the session asked: the file on disk is current.
    ProjectsDocumentHost host;
    auto outcome = std::make_shared<SaveOutcome>();
    host.saveDocument(path, recordInto(outcome, path, document));
    QCOMPARE(outcome->calls, 1);
    QVERIFY(outcome->ok);
    QTest::qWait(50);
    QCOMPARE(outcome->calls, 1);
}

void ProjectsDocumentHostTest::testSaveOfDocumentNotOpenFails()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("game/a.kra"));
    openDocument(dir.filePath(QStringLiteral("game/other.kra")), true);

    ProjectsDocumentHost host;
    auto outcome = std::make_shared<SaveOutcome>();
    host.saveDocument(path, recordInto(outcome, path, nullptr));
    QCOMPARE(outcome->calls, 1);
    QVERIFY(!outcome->ok);
    QTest::qWait(50);
    QCOMPARE(outcome->calls, 1);
}

void ProjectsDocumentHostTest::testSaveWritesTheFileThenReportsOnce()
{
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("game"))));
    const QString path = dir.filePath(QStringLiteral("game/a.kra"));
    KisDocument *document = openDocument(path, true, true);
    QVERIFY(!QFileInfo::exists(path));

    ProjectsDocumentHost host;
    auto outcome = std::make_shared<SaveOutcome>();
    host.saveDocument(QDir::toNativeSeparators(path), recordInto(outcome, path, document));
    QTRY_COMPARE_WITH_TIMEOUT(outcome->calls, 1, 60000);
    QVERIFY(outcome->ok);
    QVERIFY(outcome->fileWritten);
    QVERIFY(outcome->documentClean);

    // Nothing else arrives afterwards.
    QTest::qWait(300);
    QCOMPARE(outcome->calls, 1);
    QVERIFY(!document->isSaving());
}

void ProjectsDocumentHostTest::testSaveIntoReadOnlyFileFails()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("game/locked.kra"));
    writeFile(path, "earlier content");
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser));
    // The permissions go back before the folder is removed, whatever happens below.
    const std::shared_ptr<void> restore(nullptr, [path](void *) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser
                                        | QFileDevice::WriteUser);
    });
    QVERIFY(!QFileInfo(path).isWritable());
    KisDocument *document = openDocument(path, true, true);

    ProjectsDocumentHost host;
    auto outcome = std::make_shared<SaveOutcome>();
    host.saveDocument(path, recordInto(outcome, path, document));
    QTRY_COMPARE_WITH_TIMEOUT(outcome->calls, 1, 60000);
    QVERIFY(!outcome->ok);
    QTest::qWait(300);
    QCOMPARE(outcome->calls, 1);
    QVERIFY(document->isModified());

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("earlier content"));
}

void ProjectsDocumentHostTest::testSaveIntoMissingFolderFails()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("missing/a.kra"));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("missing"))));
    KisDocument *document = openDocument(path, true, true);

    ProjectsDocumentHost host;
    auto outcome = std::make_shared<SaveOutcome>();
    host.saveDocument(path, recordInto(outcome, path, document));
    QTRY_COMPARE_WITH_TIMEOUT(outcome->calls, 1, 60000);
    QVERIFY(!outcome->ok);
    QTest::qWait(300);
    QCOMPARE(outcome->calls, 1);
    QVERIFY(document->isModified());
    QVERIFY(!document->isSaving());
}

void ProjectsDocumentHostTest::testReloadDecision()
{
    QTemporaryDir dir;
    const QString modifiedPath = dir.filePath(QStringLiteral("game/modified.kra"));
    const QString cleanPath = dir.filePath(QStringLiteral("game/clean.kra"));
    const QString sharedPath = dir.filePath(QStringLiteral("game/shared.kra"));
    openDocument(modifiedPath, true);
    KisDocument *clean = openDocument(QDir::toNativeSeparators(cleanPath), false);
    // The same file open twice, one copy with unsaved changes: neither copy may be replaced.
    openDocument(sharedPath, false);
    openDocument(sharedPath, true);

    ProjectsDocumentHost host;
    QVERIFY(host.documentsToReload(modifiedPath).isEmpty());
    const QList<KisDocument *> toReload = host.documentsToReload(cleanPath);
    QCOMPARE(toReload.size(), 1);
    QCOMPARE(toReload.first(), clean);
    QVERIFY(host.documentsToReload(sharedPath).isEmpty());
    QVERIFY(host.documentsToReload(dir.filePath(QStringLiteral("game/not-open.kra"))).isEmpty());
}

void ProjectsDocumentHostTest::testReloadLeavesModifiedDocumentAlone()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("game/a.kra"));
    writeFile(path, "content on disk");
    KisDocument *document = openDocument(QDir::toNativeSeparators(path), true);
    const QList<QPointer<KisDocument>> before = KisPart::instance()->documents();

    ProjectsDocumentHost host;
    host.reloadDocument(path);
    QTest::qWait(200);

    QVERIFY(KisPart::instance()->documents() == before);
    QVERIFY(KisPart::instance()->documents().contains(document));
    QVERIFY(document->isModified());
    QCOMPARE(document->path(), QDir::toNativeSeparators(path));
}

KISTEST_MAIN(ProjectsDocumentHostTest)
