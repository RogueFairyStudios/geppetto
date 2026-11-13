/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGCONFLICTMARKERS_H
#define KISVERSIONINGCONFLICTMARKERS_H

#include <QDateTime>
#include <QString>
#include <QVector>

#include "kritaversioning_export.h"

/**
 * The "conflict to resolve" markers of a project: one small JSON file per
 * marker, in the project's .geppetto/conflicts folder. Paths are relative to
 * the project root, with '/' as the separator.
 */
class KRITAVERSIONING_EXPORT KisVersioningConflictMarkers
{
public:
    struct Marker {
        QString copyPath;      ///< the conflict copy
        QString originalPath;  ///< the file it is a copy of
        QString author;
        QDateTime date;
    };

    explicit KisVersioningConflictMarkers(const QString &projectPath);

    bool add(const Marker &marker);
    /// Unreadable marker files are skipped.
    QVector<Marker> list() const;
    bool remove(const QString &copyPath);

private:
    QString markerFilePath(const QString &copyPath) const;

    QString m_projectPath;
};

#endif // KISVERSIONINGCONFLICTMARKERS_H
