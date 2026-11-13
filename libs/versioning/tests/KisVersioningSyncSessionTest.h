/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGSYNCSESSIONTEST_H
#define KISVERSIONINGSYNCSESSIONTEST_H

#include <simpletest.h>
#include <QObject>

class KisVersioningSyncSessionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testOnlyMineIsSent();
    void testOnlyTheirsReloadsCleanDoc();
    void testModifiedOpenDocIsSavedFirst();
    void testBothModifiedKeepsBoth();
    void testConflictCopyNameAlreadyTaken();
    void testConflictCopyNeverOverwritesOtherCase();
    void testConflictCopyFailureLeavesFileUntouched();
    void testDeleteVersusModifyKeepsTheChange();
    void testServerChangedRetriesUpToThreeTimes();
    void testOfflineIsPendingUpload();
    void testNotSignedInIsReported();
    void testResolveFailureEndsWithError();
    void testSaveFailureStopsBeforeDriver();
    void testFinishedIsEmittedOnce();
};

#endif // KISVERSIONINGSYNCSESSIONTEST_H
