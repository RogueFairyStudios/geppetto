/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LORESTRINGS_H
#define LORESTRINGS_H

#include <cstring>

#include <QByteArray>
#include <QString>

// lore.h is a C header without C++ linkage guards.
extern "C" {
#include <lore.h>
}

/**
 * A string for the library, pointing into @p utf8. The library copies it before the call that
 * takes it returns, so @p utf8 only has to live until then. An empty string is {nullptr, 0}, as
 * the library requires.
 */
inline lore_string_t toLore(const QByteArray &utf8)
{
    lore_string_t value;
    std::memset(&value, 0, sizeof value);
    if (!utf8.isEmpty()) {
        value.string = utf8.constData();
        value.length = uintptr_t(utf8.size());
    }
    return value;
}

/**
 * A string from the library. Reads exactly @p value.length bytes: the library's strings are not
 * guaranteed to end where a terminator would say. {nullptr, 0} gives a null QString.
 */
inline QString fromLore(const lore_string_t &value)
{
    if (!value.string || value.length == 0) {
        return QString();
    }
    return QString::fromUtf8(value.string, int(value.length));
}

#endif // LORESTRINGS_H
