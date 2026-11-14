/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsPresence.h"

#include <QDir>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <KisVersioningDriver.h>
#include <KisVersioningFileRules.h>
#include <KisVersioningJob.h>
#include <KisVersioningSettings.h>

#include "ProjectsDocumentHost.h"

using namespace KisVersioning;

namespace {

/// True when @p status shows work on @p relativePath that is not on the server: a change on
/// disk, or versions not sent (which may hold it).
bool hasWorkToSend(const ProjectStatus &status, const QString &relativePath)
{
    if (status.pendingUpload > 0) {
        return true;
    }
    const QString key = ProjectsDocumentHost::pathKey(relativePath);
    for (const FileStatus &file : status.files) {
        if (file.state != FileState::Unchanged && ProjectsDocumentHost::pathKey(file.path) == key) {
            return true;
        }
    }
    return false;
}

} // namespace

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
        if (isNeverSynced(relative, configuredBackupSuffix())) {
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
    // Changed again after it was closed: the close no longer decides anything.
    m_closing.remove(key);
    if (m_announced.contains(key)) {
        return;
    }
    Entry entry;
    entry.projectPath = projectPath;
    entry.relativePath = relativePath;
    entry.serial = m_nextSerial++;
    m_announced.insert(key, entry);
    announce(key, entry);
}

void ProjectsPresence::announce(const QString &key, const Entry &entry)
{
    callDriver([this, key, entry]() {
        KisVersioningJob *job = m_driver->announceEditing(entry.projectPath, entry.relativePath, true);
        // The job deletes itself; its result is read here. Safe if this object is destroyed first.
        connect(job, &KisVersioningJob::finished, this, [this, key, entry](KisVersioningJob *finishedJob) {
            const bool registered = finishedJob->error() == KisVersioningJob::NoError;
            const auto current = m_announced.find(key);
            if (current != m_announced.end() && current->serial == entry.serial) {
                if (registered) {
                    current->inFlight = false;
                } else {
                    // Offline, or another person's notice is registered: nothing to show. The next
                    // change tries again.
                    m_announced.erase(current);
                }
            }
            // Withdrawn while it was being sent: withdrawn now, if there is anything to withdraw.
            const auto waiting = m_withdrawWhenAnnounced.find(entry.serial);
            if (waiting != m_withdrawWhenAnnounced.end()) {
                const Entry withdrawn = *waiting;
                m_withdrawWhenAnnounced.erase(waiting);
                if (registered) {
                    sendWithdrawal(withdrawn.projectPath, withdrawn.relativePath);
                }
            }
        });
    });
}

void ProjectsPresence::withdraw(const QString &key)
{
    const auto it = m_announced.find(key);
    if (it == m_announced.end()) {
        return;
    }
    const Entry entry = *it;
    m_announced.erase(it);
    if (entry.inFlight) {
        // Sent now, it could reach the server before the announcement and leave it in place.
        m_withdrawWhenAnnounced.insert(entry.serial, entry);
    } else {
        sendWithdrawal(entry.projectPath, entry.relativePath);
    }
}

void ProjectsPresence::sendWithdrawal(const QString &projectPath, const QString &relativePath)
{
    // The outcome is of no interest: a notice that cannot be withdrawn is not the artist's concern.
    callDriver([this, projectPath, relativePath]() { m_driver->announceEditing(projectPath, relativePath, false); });
}

bool ProjectsPresence::isAnnounced(const QString &projectPath, const QString &relativePath) const
{
    const QString projectKey = ProjectsDocumentHost::pathKey(projectPath);
    const QString fileKey = ProjectsDocumentHost::pathKey(relativePath);
    for (const Entry &entry : m_announced) {
        if (ProjectsDocumentHost::pathKey(entry.projectPath) == projectKey
            && ProjectsDocumentHost::pathKey(entry.relativePath) == fileKey) {
            return true;
        }
    }
    return false;
}

void ProjectsPresence::callDriver(const std::function<void()> &call)
{
    if (m_held) {
        m_waiting.append(call);
    } else {
        call();
    }
}

void ProjectsPresence::setHeld(bool held)
{
    m_held = held;
    while (!m_held && !m_waiting.isEmpty()) {
        m_waiting.takeFirst()();
    }
}

void ProjectsPresence::documentClosed(const QString &absolutePath)
{
    QString projectPath;
    QString relativePath;
    if (!locate(absolutePath, &projectPath, &relativePath)) {
        return;
    }
    const QString key = ProjectsDocumentHost::pathKey(absolutePath);
    if (!m_announced.contains(key)) {
        return; // not announced in this run of the application: a synchronisation takes care of it
    }
    // Whether the file still holds work that is not on the server is asked now: the document's own
    // state is gone with it, and a save clears it anyway.
    const quint64 serial = m_nextSerial++;
    m_closing.insert(key, serial);
    callDriver([this, key, serial, projectPath, relativePath]() {
        KisVersioningJob *job = m_driver->status(projectPath);
        connect(job, &KisVersioningJob::finished, this, [this, key, serial, relativePath](KisVersioningJob *finishedJob) {
            if (m_closing.value(key) != serial) {
                return; // changed again since, or closed again
            }
            m_closing.remove(key);
            if (finishedJob->error() != KisVersioningJob::NoError
                || hasWorkToSend(finishedJob->result<ProjectStatus>(), relativePath)) {
                return; // the notice stays until the work is sent
            }
            withdraw(key);
        });
    });
}

void ProjectsPresence::projectSynchronised(const QString &projectPath)
{
    if (!m_driver->capabilities().testFlag(Presence)) {
        return;
    }
    const QString projectKey = ProjectsDocumentHost::pathKey(projectPath);
    QStringList keys;
    QSet<QString> withdrawn;
    for (auto it = m_announced.constBegin(); it != m_announced.constEnd(); ++it) {
        if (ProjectsDocumentHost::pathKey(it->projectPath) == projectKey) {
            keys << it.key();
            withdrawn.insert(ProjectsDocumentHost::pathKey(it->relativePath));
        }
    }
    for (const QString &key : std::as_const(keys)) {
        m_closing.remove(key);
        withdraw(key);
    }

    // Notices this person left in the project before (an earlier run of the application that
    // quit, failed or had no network) are withdrawn too. Without a name nothing can be told apart.
    const QString me = m_driver->currentUser();
    if (me.isEmpty()) {
        return;
    }
    callDriver([this, projectPath, me, withdrawn]() {
        KisVersioningJob *job = m_driver->whoIsEditing(projectPath, QStringList());
        connect(job, &KisVersioningJob::finished, this, [this, projectPath, me, withdrawn](KisVersioningJob *finishedJob) {
            if (finishedJob->error() != KisVersioningJob::NoError) {
                return;
            }
            const QVector<EditingInfo> editing = finishedJob->result<QVector<EditingInfo>>();
            for (const EditingInfo &info : editing) {
                // Announced again since the run ended: that notice is current.
                if (info.user != me || withdrawn.contains(ProjectsDocumentHost::pathKey(info.path))
                    || isAnnounced(projectPath, info.path)) {
                    continue;
                }
                sendWithdrawal(projectPath, info.path);
            }
        });
    });
}

void ProjectsPresence::documentActivated(const QString &absolutePath)
{
    QString projectPath;
    QString relativePath;
    if (!locate(absolutePath, &projectPath, &relativePath)) {
        return;
    }
    callDriver([this, projectPath, absolutePath, relativePath]() {
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
    });
}
