/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGFILERULESTEST_H
#define KISVERSIONINGFILERULESTEST_H

#include <simpletest.h>
#include <QObject>

class KisVersioningFileRulesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testNeverSynced();
    void testConflictCopyName();
    void testDefaultNotes();
    void testMarkersRoundTrip();
};

#endif // KISVERSIONINGFILERULESTEST_H
