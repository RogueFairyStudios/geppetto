/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSMODELTEST_H
#define PROJECTSMODELTEST_H

#include <simpletest.h>
#include <QObject>

class ProjectsModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();

    void testFoldersAndFilesAppear();
    void testStatesComeFromStatus();
    void testEditingUserIsShown();
    void testConflictCopyIsFlagged();
    void testSummaryText();
    void testNeverSyncedFilesAreHidden();
    void testMissingProjectFolderShowsError();
    void testNotSignedInKeepsProjectsListed();

    void testOtherDriversProjectsAreHidden();
    void testRemovedFileIsListed();
    void testOwnEditingIsNotShown();
    void testAddFolderAddsProject();
    void testTickingFolderDownloadsProject();
    void testUntickingFolderNarrowsTheChoice();
    void testUntickingFolderWithUnsentWorkIsRefused();
    void testConflictResolvedRemovesOnlyTheMarker();
    void testDestroyedWhileRefreshingIsSafe();

    void testRunnerWaitsForEachProject();
    void testRunnerGoesOnAfterAFailure();

    void testPathKeyIgnoresSeparatorsAndCase();
    void testPluginRegistersThePanel();
    void testTitleIsTranslated();
};

#endif // PROJECTSMODELTEST_H
