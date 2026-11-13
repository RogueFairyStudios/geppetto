/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LOREDRIVERTEST_H
#define LOREDRIVERTEST_H

#include <QTemporaryDir>

#include <KisVersioningDriverConformance.h>

/**
 * The conformance tests against a Lore server, whose address comes only from the environment
 * variable KRITA_LORE_TEST_SERVER (for example lore://127.0.0.1:41337). Without it every test
 * that needs the server skips. The presence test needs a server that takes each caller's claimed
 * identity (trust_unsigned_identity), since an unauthenticated server cannot tell people apart.
 *
 * Lore's per-user data goes to a temporary folder, never to the user profile.
 */
class LoreDriverTest : public KisVersioningDriverConformance
{
    Q_OBJECT
public:
    LoreDriverTest();

protected:
    bool backendAvailable(QString *reason) const override;
    KisVersioningDriver *createDriver(QObject *parent) override;
    bool createProject(KisVersioningDriver *driver, const QString &localPath) override;
    bool cloneProject(KisVersioningDriver *driver, const QString &localPath) override;
    void setOffline(KisVersioningDriver *driver, bool offline) override;

private Q_SLOTS:
    void testRegistryFindsThePlugin();
    void testDeclaresItsCapabilities();
    void testExpiredSessionIsNotSignedIn();
    void testRawLoreErrorNeverReachesErrorText();
    void testLargeFileReportsProgress();
    void testSessionKeepsBothVersionsOfALineMergedFile();
    void testListsProjectsAndFoldersWithoutDownloading();
    void testSignInWithoutAuthenticationUsesTheSystemUser();
    void testResolveMineKeepsMyBytes_data();
    void testResolveMineKeepsMyBytes();
    void testUnsettledDivergenceIsReportedAgain_data();
    void testUnsettledDivergenceIsReportedAgain();

private:
    QString server() const;

    QTemporaryDir m_loreData;
    KisVersioning::ProjectInfo m_project;   ///< the last project createProject() made
};

#endif // LOREDRIVERTEST_H
