/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGJOBTEST_H
#define KISVERSIONINGJOBTEST_H

#include <simpletest.h>
#include <QObject>

class KisVersioningJobTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFinishDeliversResultOnce();
    void testFailCarriesErrorAndText();
    void testFinishFromAnotherThread();
    void testCancelIsOnlyARequest();
    void testRegistryFindsAddedFactory();
    void testTypesTravelInVariant();
};

#endif // KISVERSIONINGJOBTEST_H
