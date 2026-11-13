/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningConflictMarkers.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
const char conflictsFolder[] = ".geppetto/conflicts";
}

KisVersioningConflictMarkers::KisVersioningConflictMarkers(const QString &projectPath)
    : m_projectPath(projectPath)
{
}

QString KisVersioningConflictMarkers::markerFilePath(const QString &copyPath) const
{
    const QByteArray hash = QCryptographicHash::hash(copyPath.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QDir(m_projectPath).filePath(QLatin1String(conflictsFolder) + QLatin1Char('/')
                                        + QString::fromLatin1(hash) + QStringLiteral(".json"));
}

bool KisVersioningConflictMarkers::add(const Marker &marker)
{
    if (!QDir(m_projectPath).mkpath(QLatin1String(conflictsFolder))) {
        return false;
    }
    QJsonObject object;
    object.insert(QStringLiteral("copyPath"), marker.copyPath);
    object.insert(QStringLiteral("originalPath"), marker.originalPath);
    object.insert(QStringLiteral("author"), marker.author);
    object.insert(QStringLiteral("date"), marker.date.toString(Qt::ISODateWithMs));

    QSaveFile file(markerFilePath(marker.copyPath));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson());
    return file.commit();
}

QVector<KisVersioningConflictMarkers::Marker> KisVersioningConflictMarkers::list() const
{
    QVector<Marker> markers;
    const QDir dir(QDir(m_projectPath).filePath(QLatin1String(conflictsFolder)));
    const QStringList names = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &name : names) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) {
            continue;
        }
        const QJsonObject object = document.object();
        Marker marker;
        marker.copyPath = object.value(QStringLiteral("copyPath")).toString();
        if (marker.copyPath.isEmpty()) {
            continue;
        }
        marker.originalPath = object.value(QStringLiteral("originalPath")).toString();
        marker.author = object.value(QStringLiteral("author")).toString();
        marker.date = QDateTime::fromString(object.value(QStringLiteral("date")).toString(), Qt::ISODateWithMs);
        markers.append(marker);
    }
    return markers;
}

bool KisVersioningConflictMarkers::remove(const QString &copyPath)
{
    return QFile::remove(markerFilePath(copyPath));
}
