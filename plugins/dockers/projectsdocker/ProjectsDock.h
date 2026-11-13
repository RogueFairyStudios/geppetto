/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSDOCK_H
#define PROJECTSDOCK_H

#include <QDockWidget>
#include <QSet>
#include <QStringList>

#include <KisVersioningSettings.h>

class KisDocument;
class KisVersioningDriver;
class ProjectsDocumentHost;
class ProjectsModel;
class ProjectsSyncRunner;
class QLabel;
class QLineEdit;
class QModelIndex;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeView;

/**
 * The "Projects" panel: sign in when the driver needs it, choose the folders,
 * see the state of each file, and synchronise every downloaded project with
 * one button. It talks only to the versioning library.
 */
class ProjectsDock : public QDockWidget
{
    Q_OBJECT
public:
    ProjectsDock();
    ~ProjectsDock() override;

private:
    void buildInterface(QWidget *page);
    void updateAccount();
    void signIn();
    void addFolder();
    void synchronise();
    void onRefreshed();
    void onProjectSynchronised(int project);
    void onSyncProgress(int project, int projectCount, qint64 done, qint64 total, const QString &label);
    void onSyncFinished();
    void openItem(const QModelIndex &index);
    void showContextMenu(const QPoint &position);
    void updateSaveWarning();
    void watchDocument(KisDocument *document);
    void onDocumentSaved(const QString &path);
    void showMessages(const QStringList &messages);
    QString keyOf(const QModelIndex &index) const;
    void rememberExpanded();
    void restoreExpanded();

    KisVersioningSettings m_settings;
    KisVersioningDriver *m_driver {nullptr};
    ProjectsDocumentHost *m_host {nullptr};
    ProjectsModel *m_model {nullptr};
    ProjectsSyncRunner *m_runner {nullptr};
    QTimer *m_refreshTimer {nullptr};
    bool m_signedIn {false};
    QStringList m_syncMessages;
    QSet<QString> m_expanded;
    QSet<QString> m_knownProjects;

    QLabel *m_userLabel {nullptr};
    QLineEdit *m_addressEdit {nullptr};
    QPushButton *m_signInButton {nullptr};
    QPushButton *m_addFolderButton {nullptr};
    QTreeView *m_view {nullptr};
    QLabel *m_messageLabel {nullptr};
    QLabel *m_summaryLabel {nullptr};
    QLabel *m_pendingLabel {nullptr};
    QLineEdit *m_notesEdit {nullptr};
    QLabel *m_saveWarningLabel {nullptr};
    QProgressBar *m_progressBar {nullptr};
    QPushButton *m_syncButton {nullptr};
};

#endif // PROJECTSDOCK_H
