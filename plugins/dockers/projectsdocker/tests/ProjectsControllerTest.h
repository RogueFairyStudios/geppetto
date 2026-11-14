/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSCONTROLLERTEST_H
#define PROJECTSCONTROLLERTEST_H

#include <QObject>

/// What the Projects panel decides, with the fake driver and fake documents, without a window.
class ProjectsControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init();

    void testPendingUploadFromARunStaysUntilSent();
    void testNoticesStayWhileWorkIsPending();
    void testNothingElseReachesTheDriverDuringARun();
    void testRunWaitsForTheListBeingRead();
    void testOneRunAtATime();
    void testMessagesAfterARun();
    void testDocumentNotReloadedIsWarned();
    void testSignedOutStopsRunsAndChoices();
    void testWhoIsSignedInIsAskedAtStart();
    void testNobodySignedInStopsRunsAndChoices();
    void testNoNetworkAtStartStillAllowsWork();
    void testSignInShowsWhatItAsks();
};

#endif // PROJECTSCONTROLLERTEST_H
