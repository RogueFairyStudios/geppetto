/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGSETTINGS_H
#define KISVERSIONINGSETTINGS_H

#include <QString>
#include <QStringList>
#include <QVector>

#include <KSharedConfig>

#include "KisVersioningTypes.h"
#include "kritaversioning_export.h"

/**
 * The versioning settings and the list of downloaded project folders,
 * kept in the "Versioning" group of the application settings.
 */
class KRITAVERSIONING_EXPORT KisVersioningSettings
{
public:
    struct Project {
        QString driverId;
        KisVersioning::ProjectInfo info;
        QString localPath;
        QStringList folders;
    };

    explicit KisVersioningSettings(KSharedConfigPtr config = KSharedConfig::openConfig());

    QString serverAddress() const;
    void setServerAddress(const QString &address);

    QString driverId() const;
    void setDriverId(const QString &id);

    QString projectsRoot() const;
    void setProjectsRoot(const QString &path);

    /// Projects whose folder no longer exists stay listed; entries that cannot be read are skipped.
    QVector<Project> projects() const;
    /// A project with the same local path (compared after normalising it) is replaced.
    void addProject(const Project &project);
    void removeProject(const QString &localPath);
    void setFolders(const QString &localPath, const QStringList &folders);

private:
    void storeProjects(const QVector<Project> &projects);

    KSharedConfigPtr m_config;
};

#endif // KISVERSIONINGSETTINGS_H
