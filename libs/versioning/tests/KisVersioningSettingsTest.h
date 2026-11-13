/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGSETTINGSTEST_H
#define KISVERSIONINGSETTINGSTEST_H

#include <simpletest.h>
#include <QObject>

class KisVersioningSettingsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testDefaults();
    void testScalarRoundTrip();
    void testProjectsRoundTrip();
    void testRemoveProject();
    void testSetFolders();
    void testMissingFolderStaysListed();
    void testCorruptProjectsEntryIsIgnored();
    void testEntriesWithoutLocalPathAreSkipped();
    void testTrailingSlash();
    void testNativeSeparators();
    void testAddSamePathTwice();
};

#endif // KISVERSIONINGSETTINGSTEST_H
