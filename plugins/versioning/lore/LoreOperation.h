/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LOREOPERATION_H
#define LOREOPERATION_H

#include <functional>
#include <memory>

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <KisVersioningJob.h>
#include <KisVersioningTypes.h>

#include "LoreCall.h"

/// How long the driver waits for the answer to a server check. Without network a check normally
/// ends in about 2 s; right after the server went away any call can wait for minutes.
constexpr int loreReachabilityWaitMs = 10000;

/// How long the answer "the server did not answer" is reused before the server is checked again.
/// That it answered is never reused: the network can go away in between, and the next call would
/// wait for minutes.
constexpr qint64 loreReachabilityReuseMs = 10000;

/// The same, while the call of a check that passed its limit is still waiting in the library: the
/// library cannot stop it, so checks are spaced out to keep such calls few.
constexpr qint64 loreReachabilityReuseLingeringMs = 60000;

/**
 * What the driver knows about whether a server answers, per key (a project folder or a server
 * address), from its server checks. Each check gets a number when it begins; only the newest
 * check's answer is recorded, whatever order the answers come in. Used on the driver's thread.
 */
class LoreReachability
{
public:
    enum class Decision {
        Ask,          ///< no check under way and no recent "no": a new check is needed
        Wait,         ///< the newest check is under way, within its limit: share its answer
        Unreachable,  ///< the server did not answer lately
    };

    Decision decide(const QString &key) const;
    /// A check begins. It becomes the one later operations wait for. Returns its number.
    quint64 begin(const QString &key);
    /// The check numbered @p check answered within its limit, or passed it (@p timedOut, and then
    /// @p reachable is false). Recorded only when no newer check has begun.
    void settle(const QString &key, quint64 check, bool reachable, bool timedOut);
    /// The call of a check that passed its limit ended at last. Recorded only when no newer check
    /// has begun since.
    void lateEnd(const QString &key, quint64 check, bool reachable);
    /// Calls of checks that passed their limit and still wait in the library.
    int lingering(const QString &key) const;

private:
    struct Entry {
        quint64 latest {0};      ///< number of the newest check begun
        bool checking {false};   ///< the newest check has not answered yet
        bool known {false};
        bool reachable {false};
        QElapsedTimer age;       ///< since the recorded answer
        int lingering {0};
    };
    QHash<QString, Entry> m_entries;
};

/**
 * What this computer created for a project and did not send yet. The library cannot say it with no
 * network (it then knows nothing of the server's side), so the driver keeps it in the project's own
 * Lore folder, where it outlives the application and is never sent itself.
 */
struct LoreUnsent {
    int versions {0};      ///< versions created here and not sent
    QStringList paths;     ///< the files in them, for SyncResult::sent

    bool isEmpty() const
    {
        return versions <= 0 && paths.isEmpty();
    }
};

/// A missing or unreadable record counts as nothing unsent.
LoreUnsent readLoreUnsent(const QString &projectPath);
/// An empty record removes the file. False when it could not be written.
bool writeLoreUnsent(const QString &projectPath, const LoreUnsent &unsent);

/**
 * What the driver remembers during a session of the application, per project folder. It is only
 * a help: everything that matters is also on disk.
 */
struct LoreMemory {
    /// Paths that Lore merged by itself and the artist chose to keep as they are on disk.
    QHash<QString, QSet<QString>> keptAfterMerge;

    LoreReachability reachability;
    /// The newest check per key, while it has not answered: operations that start meanwhile
    /// share its answer.
    QHash<QString, QPointer<QObject>> checks;
};

/// What an operation takes from the driver when it starts, so that it does not need the driver
/// to live on.
struct LoreSettings {
    QString identity;          ///< empty: the library's own (the session's, or the project's)
    bool offline {false};      ///< tests only: every call runs as if there were no network
    std::shared_ptr<LoreMemory> memory;
};

/// The arguments every call takes. args() points into this object.
struct LoreGlobals {
    QByteArray repositoryPath;
    QByteArray workingDirectory;
    QByteArray identity;
    bool offline {false};
    bool cache {false};

    lore_global_args_t args() const;
};

/// One entry of lore_repository_status: a file or folder that differs from the current version.
struct LoreStatusFile {
    QString path;
    int action {0};           ///< lore_file_action_t
    bool isFile {false};
    bool staged {false};
    bool merged {false};
    bool conflict {false};    ///< both sides changed it (Lore may have merged it by itself)
    bool unresolved {false};  ///< a conflict nobody settled yet
    bool mine {false};
    bool theirs {false};
    bool dirty {false};
};

struct LoreStatus {
    QString repository;       ///< the project's identifier on the server (hexadecimal)
    QString remoteRevision;   ///< the server's latest version (hexadecimal); zeros when not known
    QString branchName;
    QString revision;         ///< the version the folder is on (hexadecimal)
    quint64 number {0};
    quint64 localNumber {0};
    quint64 remoteNumber {0};
    bool localAhead {false};
    bool remoteAhead {false};
    bool remoteAvailable {false};
    QVector<LoreStatusFile> files;
};

/// What went wrong, as the artist is told.
enum class LoreProblem {
    Other,
    Offline,
    NotSignedIn,
    NoAccess,
    NotFound,
    Busy,
    Diverged,       ///< the server holds versions this computer has not merged yet
    LockedByOther,
};

/// The library reports several refusals as -1 with the reason only in the text.
LoreProblem classifyLoreError(int status, const QString &rawText);

/// '/' separators, as the library expects on every platform.
QByteArray lorePath(const QString &path);

/// A path inside a project as the library names it: '/' separators, no leading "./" or '/'.
QString loreRelativePath(const QString &path);

/// A server address without trailing '/', as the library takes it.
QString loreServerKey(const QString &serverAddress);

/// The address of a project on its server, as the library takes it.
QString loreRepositoryUrl(const KisVersioning::ProjectInfo &project);

/// True for a folder that holds a downloaded project.
bool isLoreProject(const QString &localPath);

/**
 * Runs the Lore calls of one driver operation, one after the other, and ends its job. It is a
 * child of the job, so it goes when the job deletes itself after finished().
 */
class LoreOperation : public QObject
{
    Q_OBJECT
public:
    using EventHandler = std::function<void(const LoreEvent &)>;
    using DoneHandler = std::function<void(int status, const QString &rawText)>;

    LoreOperation(KisVersioningJob *job, const LoreSettings &settings, const QString &projectPath);
    ~LoreOperation() override;

    KisVersioningJob *job() const;
    const LoreSettings &settings() const;
    QString projectPath() const;

    /// Globals for a call on the project: its folder as repository and working directory.
    LoreGlobals globals() const;

    /// Starts one call. @p onDone runs after END, on this object's thread.
    void run(const LoreGlobals &globals, const LoreCall::Starter &starter, const EventHandler &onEvent,
             const DoneHandler &onDone);

    /// Asks lore_repository_status about the project. @p scan compares the files on disk with the
    /// current version; without it only what the library already knows is reported. On failure
    /// the job ends and @p onStatus is not called.
    void runStatus(bool scan, const std::function<void(const LoreStatus &)> &onStatus, const QString &failText);

    /// Ends the job and returns false when the project folder is missing or holds no project.
    bool requireProject();

    /**
     * A call with no network can wait for minutes and cannot be cancelled, so a call that needs
     * the server first checks that it answers. The check is bounded: @p onAnswer runs once, with
     * the server's answer when it comes within loreReachabilityWaitMs, or with reachable false when
     * it does not (the check's call then goes on in the library, which cannot stop it).
     *
     * An operation that starts while a check of the same server is under way, within its limit,
     * shares that check's answer. An operation that starts while the server lately did not answer
     * gets reachable false at once, without a call.
     *
     * @p onAnswer gets the status of the check's call too: a refusal that is not about the network
     * (such as a sign-in that expired) comes with reachable false and that status.
     */
    using Answer = std::function<void(bool reachable, int status, const QString &rawText)>;
    /// For the project's server: a status call on the project.
    void checkProjectServer(const Answer &onAnswer);
    /// For a server named by address, for calls made outside a project: a listing of its projects.
    /// With @p ownListing, a new check is made in any case and its events go to @p onEvent (the
    /// operation needs the listing itself).
    void checkServer(const QString &serverAddress, const Answer &onAnswer, bool ownListing = false,
                     const EventHandler &onEvent = EventHandler());

    /// The checks above, for an operation that cannot go on without the server: they run @p then
    /// when the server answers, and otherwise end the job with Offline (or the error of the check's
    /// refusal) and @p failText where no text of its own fits.
    void requireProjectServer(const std::function<void()> &then, const QString &failText);
    void requireServer(const QString &serverAddress, const std::function<void()> &then, const QString &failText);

    /// Ends the job after a check without an answer: Offline, or the error of a refusal that is
    /// not about the network. @p failText is used where no text of its own fits.
    void endWithoutServer(int status, const QString &rawText, const QString &failText);

    /// From now on every call of this operation runs with the library's offline option: it works
    /// on what this computer has, and never waits for the network.
    void setWorkOffline(bool offline);

    /// True, after ending the job with Cancelled, when the artist asked to stop. Checked between
    /// one call and the next: a call in flight always runs to its end.
    bool stopIfCancelled();

    void reportProgress(qint64 done, qint64 total, const QString &label);
    /// For operations that write to the project's store: before the job ends, wait for the
    /// library's background work on that store, so that nothing of the library is still writing
    /// in the project folder once the job has ended.
    void setFlushBeforeEnd(bool flush);

    void finish(const QVariant &result = QVariant());
    /// Ends the job with @p error and @p text, which must be artist-facing.
    void fail(KisVersioningJob::Error error, const QString &text);
    /// Logs the library's text and ends the job with the error that fits @p status. @p text is
    /// used for failures that have no text of their own.
    void failFromLore(const char *what, int status, const QString &rawText, const QString &text);

private:
    void endAfterFlush(const std::function<void()> &end);
    void check(const QString &key, const LoreGlobals &globals, const LoreCall::Starter &starter, bool readsStatus,
               bool own, const EventHandler &onEvent, const Answer &onAnswer);

    bool m_workOffline {false};
    QPointer<KisVersioningJob> m_job;
    bool m_flushBeforeEnd {false};
    bool m_ending {false};
    LoreSettings m_settings;
    QString m_projectPath;
};

namespace LoreText {
QString cancelled();
QString folderMissing(const QString &path);
QString notAProject(const QString &path);
QString forProblem(LoreProblem problem, const QString &fallback);
}

#endif // LOREOPERATION_H
