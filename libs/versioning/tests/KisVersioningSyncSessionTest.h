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
    void testKeepBothReloadsCleanOpenDocument();
    void testKeepBothDoesNotReloadModifiedDocument();
    void testBothModifiedKeepsBoth();
    void testConflictCopyNameAlreadyTaken();
    void testConflictCopyNeverOverwritesOtherCase();
    void testConflictCopyFailureLeavesFileUntouched();
    void testNameTakenAfterListingGetsNextName();
    void testMarkerFailureRemovesCopyAndStops();
    void testNewerVersionIsReloadedAfterKeepBoth();
    void testDestroyedDuringCopyIsSafe();
    void testWorkingFileChangedDuringCopyFails();
    void testSecondKeepBothOfSamePathIsReloaded();
    void testDocumentSavedWhileKeepingBothIsNotReloaded();
    void testDeleteVersusModifyKeepsTheChange();
    void testServerChangedRetriesUpToThreeTimes();
    void testOfflineIsPendingUpload();
    void testNotSignedInIsReported();
    void testResolveFailureEndsWithError();
    void testResolveFailureAfterTheFileChangedKeepsTheCopy();
    void testSaveFailureStopsBeforeDriver();
    void testSavesCompleteBeforeDriverIsCalled();
    void testSynchronousSaveCompletionIsAccepted();
    void testSecondSaveNotStartedAfterFailure();
    void testDestroyedWhileSavingIsSafe();
    void testDoneCalledTwiceIsIgnored();
    void testFinishedIsEmittedOnce();
};

#endif // KISVERSIONINGSYNCSESSIONTEST_H
