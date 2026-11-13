/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningSettings.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <KConfigGroup>

namespace {

const char groupName[] = "Versioning";
const char serverAddressKey[] = "serverAddress";
const char driverIdKey[] = "driverId";
const char projectsRootKey[] = "projectsRoot";
const char projectsKey[] = "projects";

// Forward slashes, no trailing slash.
QString normalizedPath(const QString &path)
{
    QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    while (clean.size() > 1 && clean.endsWith('/') && !(clean.size() == 3 && clean[1] == ':')) {
        clean.chop(1);
    }
    return clean;
}

// The form used to compare two paths.
QString pathKey(const QString &path)
{
    const QString normalized = normalizedPath(path);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return normalized.toLower();
#else
    return normalized;
#endif
}

QJsonObject toJson(const KisVersioningSettings::Project &p)
{
    return QJsonObject{
        {"driverId", p.driverId},
        {"id", p.info.id},
        {"name", p.info.name},
        {"serverAddress", p.info.serverAddress},
        {"localPath", p.localPath},
        {"folders", QJsonArray::fromStringList(p.folders)},
    };
}

KisVersioningSettings::Project fromJson(const QJsonObject &o)
{
    KisVersioningSettings::Project p;
    p.driverId = o.value("driverId").toString();
    p.info.id = o.value("id").toString();
    p.info.name = o.value("name").toString();
    p.info.serverAddress = o.value("serverAddress").toString();
    p.localPath = o.value("localPath").toString();
    const QJsonValue folders = o.value("folders");
    if (folders.isArray()) {
        const QJsonArray array = folders.toArray();
        for (const QJsonValue &f : array) {
            if (f.isString()) {
                p.folders << f.toString();
            }
        }
    }
    return p;
}

}

KisVersioningSettings::KisVersioningSettings(KSharedConfigPtr config)
    : m_config(config)
{
}

QString KisVersioningSettings::serverAddress() const
{
    return KConfigGroup(m_config, groupName).readEntry(serverAddressKey, QString());
}

void KisVersioningSettings::setServerAddress(const QString &address)
{
    KConfigGroup group(m_config, groupName);
    group.writeEntry(serverAddressKey, address);
    group.sync();
}

QString KisVersioningSettings::driverId() const
{
    return KConfigGroup(m_config, groupName).readEntry(driverIdKey, QStringLiteral("lore"));
}

void KisVersioningSettings::setDriverId(const QString &id)
{
    KConfigGroup group(m_config, groupName);
    group.writeEntry(driverIdKey, id);
    group.sync();
}

QString KisVersioningSettings::projectsRoot() const
{
    const QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Geppetto");
    return KConfigGroup(m_config, groupName).readEntry(projectsRootKey, fallback);
}

void KisVersioningSettings::setProjectsRoot(const QString &path)
{
    KConfigGroup group(m_config, groupName);
    group.writeEntry(projectsRootKey, path);
    group.sync();
}

QVector<KisVersioningSettings::Project> KisVersioningSettings::projects() const
{
    QVector<Project> result;
    const QByteArray raw = KConfigGroup(m_config, groupName).readEntry(projectsKey, QString()).toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray()) {
        return result;
    }
    const QJsonArray array = doc.array();
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;
        }
        Project p = fromJson(value.toObject());
        if (p.localPath.isEmpty()) {
            continue;
        }
        result << p;
    }
    return result;
}

void KisVersioningSettings::storeProjects(const QVector<Project> &projects)
{
    QJsonArray array;
    for (const Project &p : projects) {
        array.append(toJson(p));
    }
    KConfigGroup group(m_config, groupName);
    group.writeEntry(projectsKey, QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    group.sync();
}

void KisVersioningSettings::addProject(const Project &project)
{
    Project stored = project;
    stored.localPath = normalizedPath(project.localPath);
    const QString key = pathKey(stored.localPath);

    QVector<Project> list = projects();
    bool replaced = false;
    for (Project &existing : list) {
        if (pathKey(existing.localPath) == key) {
            existing = stored;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        list << stored;
    }
    storeProjects(list);
}

void KisVersioningSettings::removeProject(const QString &localPath)
{
    const QString key = pathKey(localPath);
    QVector<Project> list = projects();
    for (int i = 0; i < list.size(); ++i) {
        if (pathKey(list[i].localPath) == key) {
            list.removeAt(i);
            storeProjects(list);
            return;
        }
    }
}

void KisVersioningSettings::setFolders(const QString &localPath, const QStringList &folders)
{
    const QString key = pathKey(localPath);
    QVector<Project> list = projects();
    for (Project &p : list) {
        if (pathKey(p.localPath) == key) {
            p.folders = folders;
            storeProjects(list);
            return;
        }
    }
}
