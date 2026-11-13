/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LocalFolderDriver.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGlobalStatic>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QSaveFile>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>

#include <KUser>
#include <klocalizedstring.h>

#include <KisVersioningFileRules.h>

using namespace KisVersioning;

namespace {

// Everything this driver keeps lives under this folder of the project; it is never itself versioned.
const char versionsFolder[] = ".geppetto/versions";
const char indexFileName[] = "index.json";
const int indexFormat = 1;
const qint64 chunkSize = 1 << 20;

/// What went wrong, without text: the text is made on the interface thread, where translating is safe.
enum class Problem {
    None,
    Cancelled,
    FolderMissing,
    IndexUnreadable,
    IndexUnwritable,
    CannotRead,
    CannotCopy,
    CannotWrite,
    NoSuchVersion,
    NotInVersion,
    Locked,
};

/// What a worker hands back to the interface thread.
struct Outcome {
    Problem problem = Problem::None;
    QString subject;                 ///< the file, folder or version the problem is about
    QVariant result;
};

Outcome problem(Problem kind, const QString &subject = QString())
{
    Outcome outcome;
    outcome.problem = kind;
    outcome.subject = subject;
    return outcome;
}

struct FileEntry {
    QString path;
    QString sha256;
    bool deleted = false;
};

struct Version {
    int number = 0;
    QString author;
    QDateTime date;
    QString notes;
    QVector<FileEntry> files;
};

using Cancelled = std::function<bool()>;
/// Makes the default summary of a version; it runs on the job's thread, where translating is done.
using Summary = std::function<QString(int)>;

const char lockFileName[] = "lock";
std::atomic<int> lockTimeoutMs {10000};

/// One operation at a time touches the index, whatever driver asked for it.
Q_GLOBAL_STATIC(QMutex, storeMutex)

bool isProjectFolder(const QString &path)
{
    return !path.isEmpty() && QFileInfo(path).isDir();
}

QString storePath(const QString &root)
{
    return QDir(root).filePath(QString::fromLatin1(versionsFolder));
}

QString indexPath(const QString &root)
{
    return QDir(storePath(root)).filePath(QString::fromLatin1(indexFileName));
}

QString versionFolder(const QString &root, int number)
{
    return QDir(storePath(root)).filePath(QString::number(number));
}

/// A missing index is a project with no versions yet.
bool loadIndex(const QString &root, QVector<Version> *versions)
{
    versions->clear();
    QFile file(indexPath(root));
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject top = document.object();
    if (top.value(QLatin1String("format")).toInt() != indexFormat) {
        return false;
    }
    const QJsonArray array = top.value(QLatin1String("versions")).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        Version version;
        version.number = object.value(QLatin1String("number")).toInt();
        if (version.number <= 0 || (!versions->isEmpty() && version.number <= versions->last().number)) {
            return false;
        }
        version.author = object.value(QLatin1String("author")).toString();
        version.date = QDateTime::fromString(object.value(QLatin1String("date")).toString(), Qt::ISODateWithMs);
        version.date.setTimeSpec(Qt::UTC);
        version.notes = object.value(QLatin1String("notes")).toString();
        const QJsonArray files = object.value(QLatin1String("files")).toArray();
        for (const QJsonValue &fileValue : files) {
            const QJsonObject fileObject = fileValue.toObject();
            FileEntry entry;
            entry.path = fileObject.value(QLatin1String("path")).toString();
            entry.deleted = fileObject.value(QLatin1String("deleted")).toBool();
            entry.sha256 = fileObject.value(QLatin1String("sha256")).toString();
            if (entry.path.isEmpty() || (!entry.deleted && entry.sha256.isEmpty())) {
                return false;
            }
            version.files.append(entry);
        }
        versions->append(version);
    }
    return true;
}

/// The index is replaced in one step, so an interrupted save leaves the old one.
bool saveIndex(const QString &root, const QVector<Version> &versions)
{
    QJsonArray array;
    for (const Version &version : versions) {
        QJsonArray files;
        for (const FileEntry &entry : version.files) {
            QJsonObject object;
            object.insert(QLatin1String("path"), entry.path);
            if (entry.deleted) {
                object.insert(QLatin1String("deleted"), true);
            } else {
                object.insert(QLatin1String("sha256"), entry.sha256);
            }
            files.append(object);
        }
        QJsonObject object;
        object.insert(QLatin1String("number"), version.number);
        object.insert(QLatin1String("author"), version.author);
        object.insert(QLatin1String("date"), version.date.toUTC().toString(Qt::ISODateWithMs));
        object.insert(QLatin1String("notes"), version.notes);
        object.insert(QLatin1String("files"), files);
        array.append(object);
    }
    QJsonObject top;
    top.insert(QLatin1String("format"), indexFormat);
    top.insert(QLatin1String("versions"), array);

    QSaveFile file(indexPath(root));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(top).toJson(QJsonDocument::Indented));
    return file.commit();
}

/// The hash of every file as of the last version; files whose last entry is a deletion are left out.
std::map<QString, QString> lastSnapshot(const QVector<Version> &versions)
{
    std::map<QString, QString> snapshot;
    for (const Version &version : versions) {
        for (const FileEntry &entry : version.files) {
            if (entry.deleted) {
                snapshot.erase(entry.path);
            } else {
                snapshot[entry.path] = entry.sha256;
            }
        }
    }
    return snapshot;
}

/// Paths relative to the project, with '/' as the separator, without the version store and the
/// files that are never synchronised. Links are not followed.
void collectFiles(const QDir &directory, const QString &prefix, QStringList *paths)
{
    const QFileInfoList entries = directory.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString relative = prefix + entry.fileName();
        if (entry.isDir()) {
            if (relative != QLatin1String(versionsFolder)) {
                collectFiles(QDir(entry.absoluteFilePath()), relative + QLatin1Char('/'), paths);
            }
        } else if (!isNeverSynced(relative)) {
            paths->append(relative);
        }
    }
}

QStringList workingFiles(const QString &root)
{
    QStringList paths;
    collectFiles(QDir(root), QString(), &paths);
    return paths;
}

/// Hashes the file. False when it cannot be read or the operation was cancelled.
bool hashFile(const QString &path, const Cancelled &cancelled, QString *hash)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled()) {
            return false;
        }
        const QByteArray chunk = file.read(chunkSize);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            return false;
        }
        hasher.addData(chunk);
    }
    *hash = QString::fromLatin1(hasher.result().toHex());
    return true;
}

/// Copies @p source to @p destination in one pass and hashes what was copied, which is what the
/// index must record even when the file changes while it is being copied.
Problem copyAndHash(const QString &source, const QString &destination, const Cancelled &cancelled,
                    bool replaceAtomically, QString *hash)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        return Problem::CannotRead;
    }
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        return Problem::CannotWrite;
    }

    QSaveFile saving(destination);
    QFile plain(destination);
    QFileDevice &out = replaceAtomically ? static_cast<QFileDevice &>(saving) : static_cast<QFileDevice &>(plain);
    if (!out.open(QIODevice::WriteOnly)) {
        return Problem::CannotWrite;
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    while (!in.atEnd()) {
        if (cancelled()) {
            if (replaceAtomically) {
                saving.cancelWriting();
            } else {
                plain.close();
                plain.remove();
            }
            return Problem::Cancelled;
        }
        const QByteArray chunk = in.read(chunkSize);
        if (chunk.isEmpty() && in.error() != QFileDevice::NoError) {
            if (replaceAtomically) {
                saving.cancelWriting();
            } else {
                plain.close();
                plain.remove();
            }
            return Problem::CannotRead;
        }
        hasher.addData(chunk);
        if (out.write(chunk) != chunk.size()) {
            if (replaceAtomically) {
                saving.cancelWriting();
            } else {
                plain.close();
                plain.remove();
            }
            return Problem::CannotWrite;
        }
    }

    bool committed = true;
    if (replaceAtomically) {
        committed = saving.commit();
    } else {
        plain.close();
        committed = plain.error() == QFileDevice::NoError;
        if (!committed) {
            plain.remove();
        }
    }
    if (!committed) {
        return Problem::CannotWrite;
    }
    *hash = QString::fromLatin1(hasher.result().toHex());
    return Problem::None;
}

Outcome doDownload(const QString &root)
{
    QMutexLocker locker(storeMutex());
    if (!isProjectFolder(root)) {
        return problem(Problem::FolderMissing, root);
    }
    if (!QDir().mkpath(storePath(root))) {
        return problem(Problem::IndexUnwritable, root);
    }
    return Outcome();
}

Outcome doStatus(const QString &root)
{
    QMutexLocker locker(storeMutex());
    if (!isProjectFolder(root)) {
        return problem(Problem::FolderMissing, root);
    }
    QVector<Version> versions;
    if (!loadIndex(root, &versions)) {
        return problem(Problem::IndexUnreadable, root);
    }
    const std::map<QString, QString> snapshot = lastSnapshot(versions);
    const QDir directory(root);

    QMap<QString, FileState> changes;   // sorted by path
    const QStringList present = workingFiles(root);
    const QSet<QString> presentSet(present.constBegin(), present.constEnd());
    for (const QString &path : present) {
        QString hash;
        if (!hashFile(directory.filePath(path), []() { return false; }, &hash)) {
            return problem(Problem::CannotRead, path);
        }
        const auto known = snapshot.find(path);
        if (known == snapshot.end()) {
            changes.insert(path, FileState::Added);
        } else if (known->second != hash) {
            changes.insert(path, FileState::Modified);
        }
    }
    for (const auto &known : snapshot) {
        if (!presentSet.contains(known.first) && !isNeverSynced(known.first)) {
            changes.insert(known.first, FileState::Deleted);
        }
    }

    ProjectStatus status;
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        FileStatus file;
        file.path = it.key();
        file.state = it.value();
        status.files.append(file);
    }
    Outcome outcome;
    outcome.result = QVariant::fromValue(status);
    return outcome;
}

Outcome doSynchronise(const QString &root, const QString &author, const QString &notes, const Cancelled &cancelled,
                      const Summary &summary)
{
    QMutexLocker locker(storeMutex());
    if (!isProjectFolder(root)) {
        return problem(Problem::FolderMissing, root);
    }
    // Other processes (a second instance, or another computer on a shared or synced folder) may
    // synchronise the same project: the lock is held from reading the index to saving it, so the
    // number chosen below is never taken by anyone else.
    if (!QDir().mkpath(storePath(root))) {
        return problem(Problem::IndexUnwritable, root);
    }
    QLockFile storeLock(QDir(storePath(root)).filePath(QString::fromLatin1(lockFileName)));
    if (!storeLock.tryLock(lockTimeoutMs.load())) {
        return problem(Problem::Locked, root);
    }
    QVector<Version> versions;
    if (!loadIndex(root, &versions)) {
        return problem(Problem::IndexUnreadable, root);
    }
    const std::map<QString, QString> snapshot = lastSnapshot(versions);
    const QDir directory(root);

    // What changed since the last version.
    QStringList changedPaths;
    const QStringList present = workingFiles(root);
    const QSet<QString> presentSet(present.constBegin(), present.constEnd());
    for (const QString &path : present) {
        QString hash;
        if (!hashFile(directory.filePath(path), cancelled, &hash)) {
            return cancelled() ? problem(Problem::Cancelled) : problem(Problem::CannotRead, path);
        }
        const auto known = snapshot.find(path);
        if (known == snapshot.end() || known->second != hash) {
            changedPaths.append(path);
        }
    }
    QStringList deletedPaths;
    for (const auto &known : snapshot) {
        if (!presentSet.contains(known.first) && !isNeverSynced(known.first)) {
            deletedPaths.append(known.first);
        }
    }

    SyncResult result;
    if (changedPaths.isEmpty() && deletedPaths.isEmpty()) {
        Outcome outcome;
        outcome.result = QVariant::fromValue(result);
        return outcome;
    }

    Version version;
    version.number = versions.isEmpty() ? 1 : versions.last().number + 1;
    version.author = author;
    version.date = QDateTime::currentDateTimeUtc();
    version.notes = notes.trimmed().isEmpty() ? summary(changedPaths.size() + deletedPaths.size()) : notes;

    // Copies first and index last: the index is what makes the version exist. The index was read
    // under the lock and has no entry for this number, so a folder already named like it holds
    // leftovers of an interrupted run that nothing references; no committed copy is ever removed.
    const QString folder = versionFolder(root, version.number);
    QDir(folder).removeRecursively();
    auto discard = [&folder]() { QDir(folder).removeRecursively(); };

    for (const QString &path : changedPaths) {
        FileEntry entry;
        entry.path = path;
        const Problem copied = copyAndHash(directory.filePath(path), QDir(folder).filePath(path), cancelled, false, &entry.sha256);
        if (copied != Problem::None) {
            discard();
            return copied == Problem::Cancelled ? problem(Problem::Cancelled) : problem(copied == Problem::CannotWrite ? Problem::CannotCopy : copied, path);
        }
        version.files.append(entry);
    }
    for (const QString &path : deletedPaths) {
        FileEntry entry;
        entry.path = path;
        entry.deleted = true;
        version.files.append(entry);
    }
    std::sort(version.files.begin(), version.files.end(), [](const FileEntry &a, const FileEntry &b) { return a.path < b.path; });

    if (cancelled()) {
        discard();
        return problem(Problem::Cancelled);
    }
    versions.append(version);
    if (!saveIndex(root, versions)) {
        discard();
        return problem(Problem::IndexUnwritable, root);
    }

    for (const FileEntry &entry : version.files) {
        result.sent.append(entry.path);
    }
    Outcome outcome;
    outcome.result = QVariant::fromValue(result);
    return outcome;
}

Outcome doFileHistory(const QString &root, const QString &path, int offset, int limit)
{
    QMutexLocker locker(storeMutex());
    if (!isProjectFolder(root)) {
        return problem(Problem::FolderMissing, root);
    }
    QVector<Version> versions;
    if (!loadIndex(root, &versions)) {
        return problem(Problem::IndexUnreadable, root);
    }

    QVector<VersionInfo> history;
    int skipped = 0;
    for (const Version &version : versions) {   // oldest first
        bool touchesFile = false;
        for (const FileEntry &entry : version.files) {
            if (entry.path == path) {
                touchesFile = true;
                break;
            }
        }
        if (!touchesFile) {
            continue;
        }
        if (skipped < offset) {
            ++skipped;
            continue;
        }
        if (limit > 0 && history.size() >= limit) {
            break;
        }
        VersionInfo info;
        info.id = QString::number(version.number);
        info.number = version.number;
        info.author = version.author;
        info.date = version.date;
        info.notes = version.notes;
        history.append(info);
    }
    Outcome outcome;
    outcome.result = QVariant::fromValue(history);
    return outcome;
}

Outcome doFetch(const QString &root, const QString &path, const QString &versionId, const QString &outputPath,
                const Cancelled &cancelled)
{
    QMutexLocker locker(storeMutex());
    if (!isProjectFolder(root)) {
        return problem(Problem::FolderMissing, root);
    }
    QVector<Version> versions;
    if (!loadIndex(root, &versions)) {
        return problem(Problem::IndexUnreadable, root);
    }
    bool numeric = false;
    const int wanted = versionId.toInt(&numeric);
    if (!numeric || versions.isEmpty() || wanted < versions.first().number || wanted > versions.last().number) {
        return problem(Problem::NoSuchVersion, versionId);
    }

    // The file as it was then: the newest entry for it at or before that version.
    for (int i = versions.size() - 1; i >= 0; --i) {
        if (versions[i].number > wanted) {
            continue;
        }
        for (const FileEntry &entry : versions[i].files) {
            if (entry.path != path) {
                continue;
            }
            if (entry.deleted) {
                return problem(Problem::NotInVersion, path);
            }
            QString hash;
            const Problem copied = copyAndHash(QDir(versionFolder(root, versions[i].number)).filePath(path),
                                               outputPath, cancelled, true, &hash);
            if (copied != Problem::None) {
                return copied == Problem::Cancelled ? problem(Problem::Cancelled) : problem(copied, copied == Problem::CannotWrite ? outputPath : path);
            }
            return Outcome();
        }
    }
    return problem(Problem::NotInVersion, path);
}

QString textFor(const Outcome &outcome)
{
    switch (outcome.problem) {
    case Problem::Cancelled:
        return i18n("The operation was cancelled.");
    case Problem::FolderMissing:
        return i18n("The folder %1 does not exist.", QDir::toNativeSeparators(outcome.subject));
    case Problem::IndexUnreadable:
        return i18n("The versions kept in %1 could not be read.", QDir::toNativeSeparators(outcome.subject));
    case Problem::IndexUnwritable:
        return i18n("The new version could not be saved in %1.", QDir::toNativeSeparators(outcome.subject));
    case Problem::CannotRead:
        return i18n("%1 could not be read.", QDir::toNativeSeparators(outcome.subject));
    case Problem::CannotCopy:
        return i18n("%1 could not be copied.", QDir::toNativeSeparators(outcome.subject));
    case Problem::CannotWrite:
        return i18n("%1 could not be written.", QDir::toNativeSeparators(outcome.subject));
    case Problem::NoSuchVersion:
        return i18n("There is no version %1.", outcome.subject);
    case Problem::NotInVersion:
        return i18n("%1 does not exist in that version.", outcome.subject);
    case Problem::Locked:
        return i18n("The versions kept in %1 are being used by another program. Try again in a moment.",
                    QDir::toNativeSeparators(outcome.subject));
    case Problem::None:
        break;
    }
    return QString();
}

/**
 * Runs @p work on a worker thread and ends the job on the job's thread. The job has no parent and
 * the worker only holds copies of its arguments, so both are safe when the driver is destroyed
 * first. The job deletes itself once it has ended. The worker may ask the job's thread for the
 * default summary of a version (the job's thread runs its event loop, as jobs require).
 */
KisVersioningJob *startJob(const std::function<Outcome(const Cancelled &, const Summary &)> &work)
{
    auto *job = new KisVersioningJob();
    auto *watcher = new QFutureWatcher<Outcome>(job);
    QObject::connect(watcher, &QFutureWatcher<Outcome>::finished, job, [job, watcher]() {
        const Outcome outcome = watcher->result();
        if (outcome.problem == Problem::None) {
            job->finish(outcome.result);
        } else {
            job->fail(outcome.problem == Problem::Cancelled ? KisVersioningJob::Cancelled : KisVersioningJob::Failed,
                      textFor(outcome));
        }
    });
    // The job ends only through the watcher, so the pointer stays valid for as long as the worker runs.
    watcher->setFuture(QtConcurrent::run([work, job]() {
        return work([job]() { return job->cancelRequested(); },
                    [job](int changedFiles) {
                        QString text;
                        QMetaObject::invokeMethod(job, [&text, changedFiles]() { text = defaultNotes(changedFiles); },
                                                  Qt::BlockingQueuedConnection);
                        return text;
                    });
    }));
    return job;
}

KisVersioningJob *unsupported()
{
    auto *job = new KisVersioningJob();
    job->fail(KisVersioningJob::Failed, i18n("Versions kept in a folder do not support this operation."));
    return job;
}

} // namespace

void LocalFolderDriver::setLockTimeout(int milliseconds)
{
    lockTimeoutMs = milliseconds;
}

LocalFolderDriver::LocalFolderDriver(QObject *parent)
    : KisVersioningDriver(parent)
{
}

LocalFolderDriver::~LocalFolderDriver() = default;

QString LocalFolderDriver::id() const
{
    return QStringLiteral("localfolder");
}

Capabilities LocalFolderDriver::capabilities() const
{
    return Notes;
}

QString LocalFolderDriver::currentUser() const
{
    return KUser().loginName();
}

KisVersioningJob *LocalFolderDriver::signIn(const QString &)
{
    return unsupported();
}

KisVersioningJob *LocalFolderDriver::signOut()
{
    return unsupported();
}

KisVersioningJob *LocalFolderDriver::listProjects(const QString &)
{
    auto *job = new KisVersioningJob();
    job->finish(QVariant::fromValue(QVector<ProjectInfo>()));
    return job;
}

KisVersioningJob *LocalFolderDriver::listFolders(const ProjectInfo &)
{
    return unsupported();
}

KisVersioningJob *LocalFolderDriver::downloadProject(const ProjectInfo &, const QString &localPath, const QStringList &)
{
    return startJob([localPath](const Cancelled &, const Summary &) { return doDownload(localPath); });
}

KisVersioningJob *LocalFolderDriver::setFolders(const QString &, const QStringList &)
{
    return unsupported();
}

KisVersioningJob *LocalFolderDriver::status(const QString &localPath)
{
    return startJob([localPath](const Cancelled &, const Summary &) { return doStatus(localPath); });
}

KisVersioningJob *LocalFolderDriver::synchronise(const QString &localPath, const QString &notes)
{
    const QString author = currentUser();
    return startJob([localPath, author, notes](const Cancelled &cancelled, const Summary &summary) {
        return doSynchronise(localPath, author, notes, cancelled, summary);
    });
}

KisVersioningJob *LocalFolderDriver::resolve(const QString &, const QString &, Resolution, const QString &)
{
    return unsupported();
}

KisVersioningJob *LocalFolderDriver::fileHistory(const QString &localPath, const QString &path, int offset, int limit)
{
    return startJob([localPath, path, offset, limit](const Cancelled &, const Summary &) {
        return doFileHistory(localPath, path, offset, limit);
    });
}

KisVersioningJob *LocalFolderDriver::fetchFileAtVersion(const QString &localPath, const QString &path,
                                                        const QString &versionId, const QString &outputPath)
{
    return startJob([localPath, path, versionId, outputPath](const Cancelled &cancelled, const Summary &) {
        return doFetch(localPath, path, versionId, outputPath, cancelled);
    });
}

KisVersioningJob *LocalFolderDriver::announceEditing(const QString &, const QString &, bool)
{
    return unsupported();
}

KisVersioningJob *LocalFolderDriver::whoIsEditing(const QString &, const QStringList &)
{
    return unsupported();
}
