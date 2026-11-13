/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsPresence.h"

#include <QDir>
#include <QStringList>
#include <QVector>

#include <KisVersioningDriver.h>
#include <KisVersioningFileRules.h>
#include <KisVersioningJob.h>
#include <KisVersioningSettings.h>

#include "ProjectsDocumentHost.h"

using namespace KisVersioning;

ProjectsPresence::ProjectsPresence(KisVersioningDriver *driver, KisVersioningSettings *settings, QObject *parent)
    : QObject(parent)
    , m_driver(driver)
    , m_settings(settings)
{
}

ProjectsPresence::~ProjectsPresence() = default;

bool ProjectsPresence::locate(const QString &absolutePath, QString *projectPath, QString *relativePath) const
{
    if (!m_driver->capabilities().testFlag(Presence) || absolutePath.isEmpty()) {
        return false;
    }
    const QString file = QDir::cleanPath(QDir::fromNativeSeparators(absolutePath));
    const QVector<KisVersioningSettings::Project> projects = m_settings->projects();
    for (const KisVersioningSettings::Project &project : projects) {
        if (project.driverId != m_driver->id()) {
            continue;
        }
        const QString root = QDir::cleanPath(QDir::fromNativeSeparators(project.localPath));
        if (!ProjectsDocumentHost::isInside(file, root)
            || ProjectsDocumentHost::pathKey(file) == ProjectsDocumentHost::pathKey(root)) {
            continue;
        }
        const QString relative = file.mid(root.endsWith(QLatin1Char('/')) ? root.size() : root.size() + 1);
        if (isNeverSynced(relative)) {
            return false;
        }
        *projectPath = project.localPath;
        *relativePath = relative;
        return true;
    }
    return false;
}

void ProjectsPresence::documentModified(const QString &absolutePath)
{
    QString projectPath;
    QString relativePath;
    if (!locate(absolutePath, &projectPath, &relativePath)) {
        return;
    }
    const QString key = ProjectsDocumentHost::pathKey(absolutePath);
    if (m_announced.contains(key)) {
        return;
    }
    Entry entry;
    entry.projectPath = projectPath;
    entry.relativePath = relativePath;
    entry.serial = m_nextSerial++;
    m_announced.insert(key, entry);

    KisVersioningJob *job = m_driver->announceEditing(projectPath, relativePath, true);
    // The job deletes itself; its result is read here. Safe if this object is destroyed first.
    connect(job, &KisVersioningJob::finished, this, [this, key, entry](KisVersioningJob *finishedJob) {
        const bool failed = finishedJob->error() != KisVersioningJob::NoError;
        const auto it = m_announced.constFind(key);
        const bool stillWanted = it != m_announced.constEnd() && it->serial == entry.serial;
        if (failed) {
            // Offline, or another person's notice is registered: nothing to show. The next change tries again.
            if (stillWanted) {
                m_announced.remove(key);
            }
        } else if (!stillWanted) {
            // The notice was withdrawn while it was being sent.
            withdraw(entry.projectPath, entry.relativePath);
        }
    });
}

void ProjectsPresence::withdraw(const QString &projectPath, const QString &relativePath)
{
    // The outcome is of no interest: a notice that cannot be withdrawn is not the artist's concern.
    m_driver->announceEditing(projectPath, relativePath, false);
}

void ProjectsPresence::documentClosed(const QString &absolutePath, bool hadChanges)
{
    if (hadChanges || !m_driver->capabilities().testFlag(Presence)) {
        return;
    }
    const QString key = ProjectsDocumentHost::pathKey(absolutePath);
    const auto it = m_announced.find(key);
    if (it == m_announced.end()) {
        return;
    }
    const Entry entry = *it;
    m_announced.erase(it);
    withdraw(entry.projectPath, entry.relativePath);
}

void ProjectsPresence::projectSynchronised(const QString &projectPath)
{
    if (!m_driver->capabilities().testFlag(Presence)) {
        return;
    }
    const QString projectKey = ProjectsDocumentHost::pathKey(projectPath);
    for (auto it = m_announced.begin(); it != m_announced.end();) {
        if (ProjectsDocumentHost::pathKey(it->projectPath) == projectKey) {
            const Entry entry = *it;
            it = m_announced.erase(it);
            withdraw(entry.projectPath, entry.relativePath);
        } else {
            ++it;
        }
    }
}

void ProjectsPresence::documentActivated(const QString &absolutePath)
{
    QString projectPath;
    QString relativePath;
    if (!locate(absolutePath, &projectPath, &relativePath)) {
        return;
    }
    KisVersioningJob *job = m_driver->whoIsEditing(projectPath, {relativePath});
    connect(job, &KisVersioningJob::finished, this, [this, absolutePath, relativePath](KisVersioningJob *finishedJob) {
        if (finishedJob->error() != KisVersioningJob::NoError) {
            return;
        }
        const QString me = m_driver->currentUser();
        const QVector<EditingInfo> editing = finishedJob->result<QVector<EditingInfo>>();
        for (const EditingInfo &info : editing) {
            if (info.path == relativePath && !info.user.isEmpty() && info.user != me) {
                Q_EMIT someoneElseIsEditing(absolutePath, info.user);
                return;
            }
        }
    });
}
