/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningFileRulesTest.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <klocalizedstring.h>

#include <KisVersioningConflictMarkers.h>
#include <KisVersioningFileRules.h>

void KisVersioningFileRulesTest::initTestCase()
{
    // The assertions use the English source strings, whatever the machine's language.
    KLocalizedString::setLanguages({QStringLiteral("en_US")});
}

void KisVersioningFileRulesTest::testNeverSynced()
{
    using namespace KisVersioning;
    QVERIFY(isNeverSynced(".botao.kra-autosave.kra"));
    QVERIFY(isNeverSynced("ui/botao.kra-autosave.kra"));
    QVERIFY(isNeverSynced("ui/botao.kra~"));
    QVERIFY(isNeverSynced("ui/botao.kra.bak", ".bak"));
    QVERIFY(!isNeverSynced("ui/botao.kra"));
    QVERIFY(!isNeverSynced("ui/autosave-notas.txt"));
    QVERIFY(!isNeverSynced(".geppetto/conflicts/ab12.json"));
}

void KisVersioningFileRulesTest::testConflictCopyName()
{
    using namespace KisVersioning;
    QCOMPARE(conflictCopyName("botao.kra", "Bruno", {}), QString("botao (Bruno).kra"));
    QCOMPARE(conflictCopyName("botao.kra", "Bruno", {"botao (Bruno).kra"}), QString("botao (Bruno) 2.kra"));
    QCOMPARE(conflictCopyName("botao.kra", "Bruno", {"Botao (Bruno).kra"}), QString("botao (Bruno) 2.kra"));
    QCOMPARE(conflictCopyName("botao.kra", "Bruno", {"botao (Bruno).kra", "botao (Bruno) 2.kra"}),
             QString("botao (Bruno) 3.kra"));
    QCOMPARE(conflictCopyName(QString::fromUtf8("ícone novo.kra"), QString::fromUtf8("Ana Lúcia"), {}),
             QString::fromUtf8("ícone novo (Ana Lúcia).kra"));
    QCOMPARE(conflictCopyName("LEIAME", "Ana", {}), QString("LEIAME (Ana)"));
    QCOMPARE(conflictCopyName("a.tar.gz", "Ana", {}), QString("a.tar (Ana).gz"));
    QCOMPARE(conflictCopyName("a.kra", "A/B:C", {}), QString("a (A_B_C).kra"));
    QCOMPARE(conflictCopyName("a.kra", "A\\B", {}), QString("a (A_B).kra"));
    QCOMPARE(conflictCopyName("a.kra", "", {}), QString("a (2).kra"));
}

void KisVersioningFileRulesTest::testDefaultNotes()
{
    QCOMPARE(KisVersioning::defaultNotes(1), QString("1 file changed"));
    QCOMPARE(KisVersioning::defaultNotes(3), QString("3 files changed"));
}

void KisVersioningFileRulesTest::testMarkersRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir root(dir.path());

    // A folder with no .geppetto has no markers.
    KisVersioningConflictMarkers markers(dir.path());
    QVERIFY(markers.list().isEmpty());

    using Marker = KisVersioningConflictMarkers::Marker;
    const QDateTime date = QDateTime(QDate(2026, 11, 13), QTime(10, 30, 15, 250), Qt::UTC);
    const Marker first{QString::fromUtf8("ui/ícone novo (Ana Lúcia).kra"), QString::fromUtf8("ui/ícone novo.kra"),
                       QString::fromUtf8("Ana Lúcia"), date};
    const Marker second{"ui/botao (Bruno).kra", "ui/botao.kra", "Bruno", date.addDays(1)};
    QVERIFY(markers.add(first));
    QVERIFY(markers.add(second));

    // Each marker is a file of its own.
    const QStringList files = QDir(root.filePath(".geppetto/conflicts")).entryList({"*.json"}, QDir::Files);
    QCOMPARE(files.size(), 2);

    QVector<Marker> listed = markers.list();
    QCOMPARE(listed.size(), 2);
    for (const Marker &m : listed) {
        const Marker &expected = (m.copyPath == first.copyPath) ? first : second;
        QCOMPARE(m.copyPath, expected.copyPath);
        QCOMPARE(m.originalPath, expected.originalPath);
        QCOMPARE(m.author, expected.author);
        QCOMPARE(m.date, expected.date);
    }

    // A corrupt marker file is skipped.
    QFile corrupt(root.filePath(".geppetto/conflicts/0000.json"));
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("{ not json");
    corrupt.close();
    QCOMPARE(markers.list().size(), 2);

    // Removing one leaves the other.
    QVERIFY(markers.remove(first.copyPath));
    listed = markers.list();
    QCOMPARE(listed.size(), 1);
    QCOMPARE(listed.first().copyPath, second.copyPath);
}

SIMPLE_TEST_MAIN(KisVersioningFileRulesTest)
