/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningFakeDriver.h"

#include <QDir>
#include <QFile>

using namespace KisVersioning;

KisVersioningFakeDriver::KisVersioningFakeDriver(QObject *parent)
    : KisVersioningDriver(parent)
{
    m_status.result = QVariant::fromValue(ProjectStatus());
    m_editing.result = QVariant::fromValue(QVector<EditingInfo>());
    m_listProjects.result = QVariant::fromValue(QVector<ProjectInfo>());
}

KisVersioningFakeDriver::Outcome KisVersioningFakeDriver::failure(KisVersioningJob::Error error, const QString &text)
{
    Outcome outcome;
    outcome.error = error;
    outcome.text = text;
    return outcome;
}

void KisVersioningFakeDriver::setCurrentUser(const QString &user) { m_user = user; }
void KisVersioningFakeDriver::setCapabilities(Capabilities capabilities) { m_capabilities = capabilities; }

void KisVersioningFakeDriver::queueSyncResult(const SyncResult &result)
{
    Outcome outcome;
    outcome.result = QVariant::fromValue(result);
    m_syncQueue.append(outcome);
}

void KisVersioningFakeDriver::queueSyncError(KisVersioningJob::Error error, const QString &text)
{
    m_syncQueue.append(failure(error, text));
}

void KisVersioningFakeDriver::setStatus(const ProjectStatus &status)
{
    m_status = Outcome();
    m_status.result = QVariant::fromValue(status);
}

void KisVersioningFakeDriver::setStatusError(KisVersioningJob::Error error, const QString &text)
{
    m_status = failure(error, text);
}

void KisVersioningFakeDriver::setEditing(const QVector<EditingInfo> &editing)
{
    m_editing = Outcome();
    m_editing.result = QVariant::fromValue(editing);
}

void KisVersioningFakeDriver::setWhoIsEditingError(KisVersioningJob::Error error, const QString &text)
{
    m_editing = failure(error, text);
}

void KisVersioningFakeDriver::setAnnounceOutcome(KisVersioningJob::Error error, const QString &text)
{
    m_announce = failure(error, text);
}

void KisVersioningFakeDriver::setResolveError(KisVersioningJob::Error error, const QString &text)
{
    m_resolve = failure(error, text);
}

void KisVersioningFakeDriver::setTheirContent(const QString &path, const QByteArray &content)
{
    m_theirContent.insert(path, content);
}

void KisVersioningFakeDriver::setSharedLog(QStringList *log) { m_sharedLog = log; }

void KisVersioningFakeDriver::setSignInError(KisVersioningJob::Error error, const QString &text)
{
    m_signIn = failure(error, text);
}

void KisVersioningFakeDriver::setProjects(const QVector<ProjectInfo> &projects)
{
    m_listProjects = Outcome();
    m_listProjects.result = QVariant::fromValue(projects);
}

void KisVersioningFakeDriver::setListProjectsError(KisVersioningJob::Error error, const QString &text)
{
    m_listProjects = failure(error, text);
}

void KisVersioningFakeDriver::setProjectFolders(const QString &projectId, const QStringList &folders)
{
    m_projectFolders.insert(projectId, folders);
}

void KisVersioningFakeDriver::setDownloadError(KisVersioningJob::Error error, const QString &text)
{
    m_download = failure(error, text);
}

void KisVersioningFakeDriver::setSetFoldersError(KisVersioningJob::Error error, const QString &text)
{
    m_setFolders = failure(error, text);
}

const QList<KisVersioningFakeDriver::Call> &KisVersioningFakeDriver::calls() const { return m_calls; }

int KisVersioningFakeDriver::callCount(const QString &name) const
{
    return callsNamed(name).size();
}

QList<KisVersioningFakeDriver::Call> KisVersioningFakeDriver::callsNamed(const QString &name) const
{
    QList<Call> named;
    for (const Call &call : m_calls) {
        if (call.name == name) {
            named.append(call);
        }
    }
    return named;
}

void KisVersioningFakeDriver::record(const QString &name, const QStringList &args)
{
    m_calls.append({name, args});
    if (m_sharedLog) {
        m_sharedLog->append(QStringLiteral("driver:%1 %2").arg(name, args.join(QLatin1Char(' '))));
    }
}

KisVersioningJob *KisVersioningFakeDriver::makeJob(const Outcome &outcome)
{
    // The job delivers finished() later, on its own, and deletes itself.
    KisVersioningJob *job = new KisVersioningJob(this);
    if (outcome.error == KisVersioningJob::NoError) {
        job->finish(outcome.result);
    } else {
        job->fail(outcome.error, outcome.text);
    }
    return job;
}

QString KisVersioningFakeDriver::id() const { return QStringLiteral("fake"); }
Capabilities KisVersioningFakeDriver::capabilities() const { return m_capabilities; }
QString KisVersioningFakeDriver::currentUser() const
{
    if (m_userHook) {
        m_userHook();
    }
    return m_user;
}

void KisVersioningFakeDriver::setCurrentUserHook(const std::function<void()> &hook) { m_userHook = hook; }

KisVersioningJob *KisVersioningFakeDriver::signIn(const QString &serverAddress)
{
    record(QStringLiteral("signIn"), {serverAddress});
    return makeJob(m_signIn);
}

KisVersioningJob *KisVersioningFakeDriver::signOut()
{
    record(QStringLiteral("signOut"), {});
    return makeJob(Outcome());
}

KisVersioningJob *KisVersioningFakeDriver::listProjects(const QString &serverAddress)
{
    record(QStringLiteral("listProjects"), {serverAddress});
    return makeJob(m_listProjects);
}

KisVersioningJob *KisVersioningFakeDriver::listFolders(const ProjectInfo &project)
{
    record(QStringLiteral("listFolders"), {project.id});
    Outcome outcome;
    outcome.result = QVariant::fromValue(m_projectFolders.value(project.id));
    return makeJob(outcome);
}

KisVersioningJob *KisVersioningFakeDriver::downloadProject(const ProjectInfo &project, const QString &localPath,
                                                           const QStringList &folders)
{
    record(QStringLiteral("downloadProject"), {project.id, localPath, folders.join(QLatin1Char(','))});
    if (m_download.error == KisVersioningJob::NoError) {
        QDir().mkpath(localPath);
        for (const QString &folder : folders) {
            QDir(localPath).mkpath(folder);
        }
    }
    return makeJob(m_download);
}

KisVersioningJob *KisVersioningFakeDriver::setFolders(const QString &localPath, const QStringList &folders)
{
    record(QStringLiteral("setFolders"), {localPath, folders.join(QLatin1Char(','))});
    return makeJob(m_setFolders);
}

KisVersioningJob *KisVersioningFakeDriver::status(const QString &localPath)
{
    record(QStringLiteral("status"), {localPath});
    return makeJob(m_status);
}

KisVersioningJob *KisVersioningFakeDriver::synchronise(const QString &localPath, const QString &notes)
{
    record(QStringLiteral("synchronise"), {localPath, notes});
    if (m_syncQueue.isEmpty()) {
        Outcome outcome;
        outcome.result = QVariant::fromValue(SyncResult());
        return makeJob(outcome);
    }
    return makeJob(m_syncQueue.takeFirst());
}

KisVersioningJob *KisVersioningFakeDriver::resolve(const QString &localPath, const QString &path,
                                                   Resolution resolution, const QString &mergedFile)
{
    static const char *names[] = {"Mine", "Theirs", "MergedFile"};
    record(QStringLiteral("resolve"), {localPath, path, QString::fromLatin1(names[int(resolution)]), mergedFile});

    if (m_resolve.error != KisVersioningJob::NoError) {
        return makeJob(m_resolve);
    }

    const QString working = QDir(localPath).filePath(path);
    if (resolution == Resolution::Theirs && m_theirContent.contains(path)) {
        QFile file(working);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(m_theirContent.value(path));
        }
    } else if (resolution == Resolution::MergedFile) {
        QFile::remove(working);
        QFile::copy(mergedFile, working);
    }
    return makeJob(Outcome());
}

KisVersioningJob *KisVersioningFakeDriver::fileHistory(const QString &localPath, const QString &path,
                                                       int offset, int limit)
{
    record(QStringLiteral("fileHistory"), {localPath, path, QString::number(offset), QString::number(limit)});
    Outcome outcome;
    outcome.result = QVariant::fromValue(QVector<VersionInfo>());
    return makeJob(outcome);
}

KisVersioningJob *KisVersioningFakeDriver::fetchFileAtVersion(const QString &localPath, const QString &path,
                                                              const QString &versionId, const QString &outputPath)
{
    record(QStringLiteral("fetchFileAtVersion"), {localPath, path, versionId, outputPath});
    return makeJob(Outcome());
}

KisVersioningJob *KisVersioningFakeDriver::announceEditing(const QString &localPath, const QString &path, bool editing)
{
    record(QStringLiteral("announceEditing"),
           {localPath, path, editing ? QStringLiteral("true") : QStringLiteral("false")});
    return makeJob(m_announce);
}

KisVersioningJob *KisVersioningFakeDriver::whoIsEditing(const QString &localPath, const QStringList &paths)
{
    record(QStringLiteral("whoIsEditing"), {localPath, paths.join(QLatin1Char(','))});
    return makeJob(m_editing);
}
