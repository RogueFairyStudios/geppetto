/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSSYNCRUNNER_H
#define PROJECTSSYNCRUNNER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <KisVersioningSyncSession.h>

class KisVersioningDocumentHost;
class KisVersioningDriver;
class ProjectsDocuments;

/**
 * Synchronises several projects one after the other, with the same notes:
 * one KisVersioningSyncSession per project, each starting when the one
 * before has finished. A project that fails does not stop the others.
 *
 * A project with an open document that has unsaved changes and is not in
 * Krita's own format does not start: a synchronisation never saves such a
 * file, and the artist is asked to save it.
 *
 * finished() is emitted once per run, never from inside start().
 */
class ProjectsSyncRunner : public QObject
{
    Q_OBJECT
public:
    struct Result {
        QString projectPath;
        KisVersioningSyncSession::Report report;
        /// staleDocuments() of the report, asked when the session ended.
        QStringList staleDocuments;
    };

    /**
     * The files of @p report, received or kept both, whose open document has unsaved changes:
     * such a document was not reloaded, so its window shows work built on the version that the
     * file on disk no longer holds, and saving it would replace the other version. The report's
     * own staleDocuments come first. Paths are relative to the project, in the order of the report.
     */
    static QStringList staleDocuments(const QString &projectPath, const KisVersioningSyncSession::Report &report,
                                      const KisVersioningDocumentHost &host);

    ProjectsSyncRunner(KisVersioningDriver *driver, ProjectsDocuments *host, QObject *parent = nullptr);
    ~ProjectsSyncRunner() override;

    /// Does nothing while a run is under way.
    void start(const QStringList &projectPaths, const QString &notes);
    bool isRunning() const;
    /// The reports of the last run, in the order of the projects.
    QVector<Result> results() const;

Q_SIGNALS:
    /// @p project counts from 0 among @p projectCount; @p done, @p total and @p label come from the driver.
    void progress(int project, int projectCount, qint64 done, qint64 total, const QString &label);
    /// The report of @p project is in results(); the next project has not started yet.
    void projectFinished(int project);
    void finished();

private:
    void startNext();

    KisVersioningDriver *m_driver;
    ProjectsDocuments *m_host;
    bool m_running {false};
    QStringList m_paths;
    QString m_notes;
    QVector<Result> m_results;
};

#endif // PROJECTSSYNCRUNNER_H
