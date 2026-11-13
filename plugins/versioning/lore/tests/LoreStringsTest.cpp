/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreStringsTest.h"

#include <simpletest.h>

#include "LoreStrings.h"

void LoreStringsTest::testEmptyIsNullPointer()
{
    const lore_string_t empty = toLore(QByteArray());
    QVERIFY(empty.string == nullptr);
    QCOMPARE(empty.length, uintptr_t(0));

    // An empty but allocated buffer is empty too.
    const QByteArray allocated("");
    const lore_string_t alsoEmpty = toLore(allocated);
    QVERIFY(alsoEmpty.string == nullptr);
    QCOMPARE(alsoEmpty.length, uintptr_t(0));
}

void LoreStringsTest::testFromLoreUsesLength()
{
    const char buffer[] = "abcdef";
    lore_string_t value;
    std::memset(&value, 0, sizeof value);
    value.string = buffer;
    value.length = 3;
    QCOMPARE(fromLore(value), QString("abc"));
}

void LoreStringsTest::testFromLoreNull()
{
    lore_string_t value;
    std::memset(&value, 0, sizeof value);
    QVERIFY(fromLore(value).isNull());
}

void LoreStringsTest::testUtf8RoundTrip()
{
    const QString text = QString::fromUtf8("ícone novo");
    const QByteArray utf8 = text.toUtf8();
    const lore_string_t value = toLore(utf8);
    QCOMPARE(value.length, uintptr_t(utf8.size()));
    QVERIFY(value.length > uintptr_t(text.size())); // the accent takes two bytes
    QCOMPARE(fromLore(value), text);
}

SIMPLE_TEST_MAIN(LoreStringsTest)
