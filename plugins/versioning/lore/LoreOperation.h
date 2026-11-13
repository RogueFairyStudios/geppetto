/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LOREOPERATION_H
#define LOREOPERATION_H

#include <functional>
#include <memory>

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

/**
 * What the driver remembers during a session of the application, per project folder. It is only
 * a help: everything that matters is also in the library's own state on disk.
 */
struct LoreMemory {
    /// Paths of versions created on this computer and not yet sent, for SyncResult::sent.
    QHash<QString, QStringList> unsent;
    /// Paths that Lore merged by itself and the artist chose to keep as they are on disk.
    QHash<QString, QSet<QString>> keptAfterMerge;
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
