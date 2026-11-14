/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSPRESENCETEST_H
#define PROJECTSPRESENCETEST_H

#include <QObject>

class ProjectsPresenceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();

    void testFirstChangeAnnouncesOnce();
    void testFileOutsideProjectsIsIgnored();
    void testNeverSyncedFileIsIgnored();
    void testSyncWithdrawsNotice();
    void testCloseWithoutChangesWithdraws();
    void testCloseWithChangesKeepsNotice();
    void testOfflineIsSkippedSilently();
    void testRefusalIsSilentAndNextAnnouncementIsAttempted();
    void testDriverWithoutPresenceDoesNothing();
    void testActivationReportsOtherUser();
    void testOwnNoticeIsNotReported();
    void testActivationFailureIsSilent();
    void testDestroyedWithJobsPending();
    void testCloseKeepsNoticeWhileWorkWaits();
    void testChangeAfterCloseKeepsNotice();
    void testSyncWithdrawsNoticesLeftBefore();
    void testWithdrawalWaitsForTheAnnouncement();
};

#endif // PROJECTSPRESENCETEST_H
