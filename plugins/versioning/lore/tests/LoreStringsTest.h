/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LORESTRINGSTEST_H
#define LORESTRINGSTEST_H

#include <QObject>

class LoreStringsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testEmptyIsNullPointer();
    void testFromLoreUsesLength();
    void testFromLoreNull();
    void testUtf8RoundTrip();
};

#endif // LORESTRINGSTEST_H
