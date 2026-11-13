/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningSettingsTest.h"

#include <QDir>
#include <QTemporaryDir>

#include <KConfigGroup>
#include <KSharedConfig>

#include <KisVersioningSettings.h>

namespace {

KSharedConfigPtr openConfig(const QTemporaryDir &dir)
{
    return KSharedConfig::openConfig(dir.filePath("settings"), KConfig::SimpleConfig);
}

KisVersioningSettings::Project makeProject(const QString &path)
{
    KisVersioningSettings::Project p;
    p.driverId = "lore";
    p.info.id = "p1";
    p.info.name = "Game";
    p.info.serverAddress = "server.example:1234";
    p.localPath = path;
    p.folders = QStringList{"ui", "art/hero"};
    return p;
}

}

void KisVersioningSettingsTest::testDefaults()
{
    QTemporaryDir dir;
    KisVersioningSettings s(openConfig(dir));
    QVERIFY(s.serverAddress().isEmpty());
    QCOMPARE(s.driverId(), QString("lore"));
    QVERIFY(s.projects().isEmpty());
    QVERIFY(s.projectsRoot().endsWith("/Geppetto"));
}

void KisVersioningSettingsTest::testScalarRoundTrip()
{
    QTemporaryDir dir;
    {
        KisVersioningSettings s(openConfig(dir));
        s.setServerAddress("server.example:1234");
        s.setDriverId("other");
        s.setProjectsRoot("/work/projects");
    }
    KisVersioningSettings s(KSharedConfig::openConfig(dir.filePath("settings"), KConfig::SimpleConfig));
    QCOMPARE(s.serverAddress(), QString("server.example:1234"));
    QCOMPARE(s.driverId(), QString("other"));
    QCOMPARE(s.projectsRoot(), QString("/work/projects"));
}

void KisVersioningSettingsTest::testProjectsRoundTrip()
{
    QTemporaryDir dir;
    {
        KisVersioningSettings s(openConfig(dir));
        s.addProject(makeProject(dir.filePath("game")));
    }
    // A config object over the same file, read from disk.
    KisVersioningSettings s(KSharedConfig::openConfig(dir.filePath("settings"), KConfig::SimpleConfig));
    const auto list = s.projects();
    QCOMPARE(list.size(), 1);
    const auto expected = makeProject(dir.filePath("game"));
    QCOMPARE(list[0].driverId, expected.driverId);
    QCOMPARE(list[0].info.id, expected.info.id);
    QCOMPARE(list[0].info.name, expected.info.name);
    QCOMPARE(list[0].info.serverAddress, expected.info.serverAddress);
    QCOMPARE(list[0].localPath, expected.localPath);
    QCOMPARE(list[0].folders, expected.folders);
}

void KisVersioningSettingsTest::testRemoveProject()
{
    QTemporaryDir dir;
    KisVersioningSettings s(openConfig(dir));
    s.addProject(makeProject(dir.filePath("a")));
    s.addProject(makeProject(dir.filePath("b")));
    s.removeProject(dir.filePath("a"));
    QCOMPARE(s.projects().size(), 1);
    QCOMPARE(s.projects()[0].localPath, dir.filePath("b"));
    s.removeProject(dir.filePath("unknown"));
    QCOMPARE(s.projects().size(), 1);
}

void KisVersioningSettingsTest::testSetFolders()
{
    QTemporaryDir dir;
    KisVersioningSettings s(openConfig(dir));
    s.addProject(makeProject(dir.filePath("a")));
    s.addProject(makeProject(dir.filePath("b")));
    s.setFolders(dir.filePath("a"), {"sound"});
    QCOMPARE(s.projects()[0].folders, QStringList{"sound"});
    QCOMPARE(s.projects()[1].folders, (QStringList{"ui", "art/hero"}));
}

void KisVersioningSettingsTest::testMissingFolderStaysListed()
{
    QTemporaryDir dir;
    const QString gone = dir.filePath("does/not/exist");
    QVERIFY(!QDir(gone).exists());
    KisVersioningSettings s(openConfig(dir));
    s.addProject(makeProject(gone));
    QCOMPARE(s.projects().size(), 1);
    KisVersioningSettings again(openConfig(dir));
    QCOMPARE(again.projects().size(), 1);
}

void KisVersioningSettingsTest::testCorruptProjectsEntryIsIgnored()
{
    QTemporaryDir dir;
    auto config = openConfig(dir);
    KConfigGroup group(config, "Versioning");
    group.writeEntry("projects", QString("{not json"));
    KisVersioningSettings s(config);
    QVERIFY(s.projects().isEmpty());

    group.writeEntry("projects", QString("42"));
    QVERIFY(s.projects().isEmpty());

    // The list is usable again after being overwritten.
    s.addProject(makeProject(dir.filePath("a")));
    QCOMPARE(s.projects().size(), 1);
}

void KisVersioningSettingsTest::testEntriesWithoutLocalPathAreSkipped()
{
    QTemporaryDir dir;
    auto config = openConfig(dir);
    KConfigGroup group(config, "Versioning");
    group.writeEntry("projects",
                     QString("[{\"driverId\":\"lore\"}, 7, null, {\"localPath\":\"\"},"
                             " {\"localPath\":\"/ok/path\",\"folders\":\"notalist\",\"info\":5}]"));
    KisVersioningSettings s(config);
    const auto list = s.projects();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list[0].localPath, QString("/ok/path"));
    QVERIFY(list[0].folders.isEmpty());
}

void KisVersioningSettingsTest::testTrailingSlash()
{
    QTemporaryDir dir;
    KisVersioningSettings s(openConfig(dir));
    s.addProject(makeProject(dir.filePath("game") + "/"));
    QCOMPARE(s.projects()[0].localPath, dir.filePath("game"));
    s.setFolders(dir.filePath("game") + "/", {"x"});
    QCOMPARE(s.projects()[0].folders, QStringList{"x"});
    s.removeProject(dir.filePath("game") + "/");
    QVERIFY(s.projects().isEmpty());
}

void KisVersioningSettingsTest::testNativeSeparators()
{
    QTemporaryDir dir;
    KisVersioningSettings s(openConfig(dir));
    s.addProject(makeProject(dir.filePath("sub/game")));
    const QString native = QDir::toNativeSeparators(dir.filePath("sub/game"));
    s.setFolders(native, {"y"});
    QCOMPARE(s.projects()[0].folders, QStringList{"y"});
    s.removeProject(native);
    QVERIFY(s.projects().isEmpty());

    s.addProject(makeProject(native));
    const QString stored = s.projects()[0].localPath;
    QVERIFY(!stored.contains(QChar(0x5c)));
}

void KisVersioningSettingsTest::testAddSamePathTwice()
{
    QTemporaryDir dir;
    KisVersioningSettings s(openConfig(dir));
    auto first = makeProject(dir.filePath("game"));
    s.addProject(first);
    auto second = makeProject(dir.filePath("game") + "/");
    second.info.name = "Renamed";
    s.addProject(second);
    QCOMPARE(s.projects().size(), 1);
    QCOMPARE(s.projects()[0].info.name, QString("Renamed"));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    auto third = makeProject(dir.filePath("game").toUpper());
    third.info.name = "Again";
    s.addProject(third);
    QCOMPARE(s.projects().size(), 1);
    QCOMPARE(s.projects()[0].info.name, QString("Again"));
#endif
}

SIMPLE_TEST_MAIN(KisVersioningSettingsTest)
