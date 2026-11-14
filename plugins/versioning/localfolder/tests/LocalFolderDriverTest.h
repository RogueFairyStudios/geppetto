/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LOCALFOLDERDRIVERTEST_H
#define LOCALFOLDERDRIVERTEST_H

#include <KisVersioningDriverConformance.h>

class LocalFolderDriverTest : public KisVersioningDriverConformance
{
    Q_OBJECT

protected:
    bool backendAvailable(QString *reason) const override;
    KisVersioningDriver *createDriver(QObject *parent) override;
    bool createProject(KisVersioningDriver *driver, const QString &localPath) override;
    bool cloneProject(KisVersioningDriver *driver, const QString &localPath) override;
    void setOffline(KisVersioningDriver *driver, bool offline) override;

private Q_SLOTS:
    void testRegistryFindsThePlugin();
    void testDeclaresOnlyNotes();
    void testDownloadCreatesVersionsFolder();
    void testVersionsStoreIsNotVersionedButConflictMarkersAre();
    void testDeletedFileGetsAVersion();
    void testFetchOfDeletedFileFails();
    void testUnsupportedOperationsFail();
    void testListProjectsIsEmpty();
    void testDriverMayBeDestroyedWhileWorking();
    void testTwoDriversOnOneFolderKeepEveryVersion();
    void testLockedStoreFailsWithoutWriting();
    void testDefaultNotesAreStoredNotTranslatedOnRead();
    void testWorkerDoesNotNeedTheJobThreadToProduceDefaultNotes();
    void testBackupsWithTheConfiguredSuffixAreNotSynchronised();
    void testUnchangedFilesAreNotReadAgain();
};

#endif // LOCALFOLDERDRIVERTEST_H
