/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSDOCUMENTHOSTTEST_H
#define PROJECTSDOCUMENTHOSTTEST_H

#include <QObject>

class ProjectsDocumentHostTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();

    void testPathKeyIgnoresSeparatorsAndCase();
    void testModifiedDocumentsOfAProject();
    void testOpenAndModifiedState();
    void testSaveOfUnmodifiedDocumentSucceedsAtOnce();
    void testSaveOfDocumentNotOpenFails();
    void testSaveWritesTheFileThenReportsOnce();
    void testSaveIntoReadOnlyFileFails();
    void testSaveIntoMissingFolderFails();
    void testReloadDecision();
    void testReloadLeavesModifiedDocumentAlone();
};

#endif // PROJECTSDOCUMENTHOSTTEST_H
