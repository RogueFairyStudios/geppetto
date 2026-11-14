/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSCONTROLLER_H
#define PROJECTSCONTROLLER_H

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

class KisVersioningDriver;
class KisVersioningJob;
class KisVersioningSettings;
class ProjectsDocuments;
class ProjectsModel;
class ProjectsPresence;
class ProjectsSyncRunner;
class QModelIndex;
class QTimer;

/**
 * What the Projects panel decides, apart from how it looks: when a
 * synchronisation, a refresh or a folder choice may start, what the panel
 * shows after a run, who is signed in, and when editing notices go.
 *
 * The application has one controller, shared by the panel of every window
 * (instance()), so that two windows never work on the same project at once.
 * The tests build their own with a fake driver and fake documents.
 *
 * While a synchronisation is under way nothing else reaches the driver: no
 * refresh starts (one asked for meanwhile runs once the run ends), no folder
 * choice, "Add folder…" or "Conflict resolved" is accepted, and the editing
 * notices wait. A run asked for while the list is being read starts once it
 * is read.
 */
class ProjectsController : public QObject
{
    Q_OBJECT
public:
    /// Who the driver works for, as the panel shows it.
    enum class Account {
        NotNeeded,   ///< the driver has no sign-in
        Checking,    ///< asking the driver, without the artist, who is signed in
        Unknown,     ///< the driver could not tell (no network): work goes on, the artist may sign in
        SignedOut,   ///< nobody is signed in: nothing can be synchronised or chosen
        SignedIn,
    };

    /**
     * The controller of the application, made on first use with the driver chosen in the settings
     * (or the folder driver when that one is missing). Null when no driver is installed.
     */
    static ProjectsController *instance();

    /// @p driver, @p documents and @p settings belong to the caller and must outlive this object.
    ProjectsController(KisVersioningDriver *driver, ProjectsDocuments *documents, KisVersioningSettings *settings,
                       QObject *parent = nullptr);
    ~ProjectsController() override;

    /// Reads the list for the first time and, for a driver with sign-in and a known server, asks who
    /// is signed in. instance() calls it; tests call it once they have set things up.
    void start();

    KisVersioningDriver *driver() const;
    ProjectsModel *model() const;
    /// The server address kept in the settings; empty until the first sign-in.
    QString serverAddress() const;

    Account account() const;
    /// The person signed in, when account() is SignedIn.
    QString user() const;
    bool isSigningIn() const;
    /// What the sign-in under way asks of the artist (the browser step, the code to confirm).
    QString signInProgress() const;

    /// True from the moment a run is asked for until its last project is done.
    bool isSynchronising() const;
    bool canSynchronise() const;
    /// Folder choices, "Add folder…" and "Conflict resolved".
    bool canChooseFolders() const;
    /// Some work is not on the server: a project's state says so, or the last run of a project
    /// ended with work kept here and no run has sent it since.
    bool pendingUpload() const;
    /// What the panel tells the artist now.
    QStringList messages() const;
    /// Open documents of the projects that a synchronisation would save.
    int documentsToSave() const;

    void synchronise(const QString &notes);
    /// Reads the list again now, or once the run under way has ended.
    void requestRefresh();
    void signIn(const QString &serverAddress);
    void addFolder(const QString &path);
    void markConflictResolved(const QModelIndex &index);
    void showMessages(const QStringList &messages);

    /// A panel became visible or hidden. The list is read again from time to time while one shows.
    void setPanelVisible(QObject *panel, bool visible);
    /// How often the list is read again while a panel shows (milliseconds).
    void setSlowRefreshInterval(int milliseconds);
    /// How long after a save of a project file the list is read again (milliseconds).
    void setSaveRefreshDelay(int milliseconds);

public Q_SLOTS:
    // What happens to the documents of the application.
    void documentModified(const QString &absolutePath);
    void documentClosed(const QString &absolutePath);
    void documentActivated(const QString &absolutePath);
    void documentSaved(const QString &absolutePath);
    /// An open document's unsaved state changed, or a document was opened: the warning follows.
    void documentsChanged();

Q_SIGNALS:
    /// Something the panel shows changed.
    void changed();
    void progress(int project, int projectCount, qint64 done, qint64 total, const QString &label);
    /// A run ended. @p everythingWentWell when no project ended with an error.
    void runFinished(bool everythingWentWell);
    void someoneElseIsEditing(const QString &absolutePath, const QString &user);

private:
    void startRun();
    void onRefreshed();
    void onProjectSynchronised(int project);
    void onRunFinished();
    /// Asks the driver, without the artist, who is signed in to the server kept in the settings.
    void resumeSession();
    void setAccount(Account account);
    void updateChoices();
    void connectToApplication();

    KisVersioningDriver *m_driver;
    ProjectsDocuments *m_documents;
    KisVersioningSettings *m_settings;
    ProjectsModel *m_model;
    ProjectsSyncRunner *m_runner;
    ProjectsPresence *m_presence;
    QTimer *m_saveRefresh;
    QTimer *m_slowRefresh;

    Account m_account {Account::NotNeeded};
    bool m_signingIn {false};
    QString m_signInProgress;
    bool m_runRequested {false};      ///< a run waits for the list being read
    QString m_requestedNotes;
    bool m_refreshAfterRun {false};
    /// Projects whose last run ended with work kept here, by the form in which paths are compared.
    QSet<QString> m_pendingProjects;
    QStringList m_messages;
    QStringList m_runMessages;
    QSet<QObject *> m_visiblePanels;
};

#endif // PROJECTSCONTROLLER_H
