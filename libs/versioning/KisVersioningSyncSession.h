/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGSYNCSESSION_H
#define KISVERSIONINGSYNCSESSION_H

#include <functional>
#include <memory>

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QVector>
#include <QString>
#include <QStringList>

#include "KisVersioningDriver.h"
#include "KisVersioningJob.h"
#include "KisVersioningTypes.h"
#include "kritaversioning_export.h"

/**
 * What the session needs to know about the documents open in the application.
 */
class KRITAVERSIONING_EXPORT KisVersioningDocumentHost
{
public:
    virtual ~KisVersioningDocumentHost();
    /// Open documents of the project that have unsaved changes. Absolute paths.
    virtual QStringList modifiedOpenDocuments(const QString &projectPath) const = 0;
    /// Starts saving the document. Calls @p done(ok) exactly once when the file is on disk
    /// (ok == true) or the save failed or was cancelled (ok == false). It may be called before
    /// this function returns, or later from the event loop.
    virtual void saveDocument(const QString &absolutePath, std::function<void(bool)> done) = 0;
    virtual bool isOpenAndUnmodified(const QString &absolutePath) const = 0;
    virtual void reloadDocument(const QString &absolutePath) = 0;
};

/**
 * One synchronisation of one project folder, from "the artist pressed
 * synchronise" to a report. It drives a driver step by step from the
 * finished() signal of each job, without nested event loops.
 *
 * The rule above all others: a locally changed file is never replaced or
 * deleted unless its content exists in another version or in a copy. When
 * both sides changed a file, the session keeps my content in a conflict copy
 * (and a "conflict to resolve" marker) before it lets the driver take the
 * other side's file. If the copy cannot be written, the file is left alone
 * and the session ends with Failed.
 *
 * finished() is always emitted exactly once, never from inside start().
 */
class KRITAVERSIONING_EXPORT KisVersioningSyncSession : public QObject
{
    Q_OBJECT
public:
    struct Report {
        QStringList sent;
        QStringList received;
        QStringList keptBoth;        ///< files where both sides changed; paths of the originals
        QStringList conflictCopies;  ///< the copies that hold my content, one per entry of keptBoth
        bool pendingUpload = false;  ///< something of mine is still not on the server
        KisVersioningJob::Error error = KisVersioningJob::NoError;
        QString errorText;
    };

    /// The most calls to driver->synchronise() in one session.
    static constexpr int MaxAttempts = 3;

    KisVersioningSyncSession(KisVersioningDriver *driver, KisVersioningDocumentHost *host, QObject *parent = nullptr);
    ~KisVersioningSyncSession() override;

    /// Starts the session; does nothing if one is already running.
    void start(const QString &projectPath, const QString &notes);

Q_SIGNALS:
    void progress(qint64 done, qint64 total, const QString &label);
    void finished(const KisVersioningSyncSession::Report &report);

private:
    struct SaveState;
    void saveNext();
    void synchroniseOnce();
    void onSynchronised(KisVersioningJob *job);
    void resolveNext();
    void runResolve(const QString &path, KisVersioning::Resolution resolution);
    void onResolved(KisVersioningJob *job);
    void afterResolutions();
    void startKeepBoth(const QString &path);
    void onCopyFinished(bool ok, const QString &name);
    void endWithDriverError(KisVersioningJob *job);
    void endWithError(KisVersioningJob::Error error, const QString &text);
    void conclude();
    void watch(KisVersioningJob *job);
    void reloadIfClean(const QString &path);

    KisVersioningDriver *m_driver;
    KisVersioningDocumentHost *m_host;
    bool m_running {false};
    QString m_projectPath;
    QString m_notes;
    int m_attempts {0};
    bool m_serverChanged {false};
    bool m_hadDivergence {false};
    QVector<KisVersioning::Divergence> m_divergences;
    int m_nextDivergence {0};
    QString m_resolvingPath;
    bool m_resolvingTheirs {false};
    QStringList m_reloaded;
    QStringList m_documentsToSave;
    QString m_savingDocument;
    std::shared_ptr<SaveState> m_save;
    bool m_inSaveCall {false};
    QString m_keepBothPath;
    QString m_keepBothFolder;
    QString m_keepBothUser;
    qint64 m_keepBothSize {0};
    QDateTime m_keepBothModified;
    Report m_report;
};

Q_DECLARE_METATYPE(KisVersioningSyncSession::Report)

#endif // KISVERSIONINGSYNCSESSION_H
