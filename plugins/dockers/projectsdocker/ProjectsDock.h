/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSDOCK_H
#define PROJECTSDOCK_H

#include <QDockWidget>
#include <QSet>
#include <QStringList>

class ProjectsController;
class QLabel;
class QLineEdit;
class QModelIndex;
class QProgressBar;
class QPushButton;
class QTreeView;

/**
 * The "Projects" panel of one window: sign in when the driver needs it, choose
 * the folders, see the state of each file, and synchronise every downloaded
 * project with one button. What it shows and allows comes from the
 * application's ProjectsController, which the panels of all windows share.
 */
class ProjectsDock : public QDockWidget
{
    Q_OBJECT
public:
    ProjectsDock();
    ~ProjectsDock() override;

private:
    void buildInterface(QWidget *page);
    void update();
    void signIn();
    void addFolder();
    void synchronise();
    void onSyncProgress(int project, int projectCount, qint64 done, qint64 total, const QString &label);
    void openItem(const QModelIndex &index);
    void showContextMenu(const QPoint &position);
    QString keyOf(const QModelIndex &index) const;
    void rememberExpanded();
    void restoreExpanded();

    ProjectsController *m_controller {nullptr};
    bool m_startedRun {false};
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
