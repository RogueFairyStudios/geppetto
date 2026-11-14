/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSPRESENCE_H
#define PROJECTSPRESENCE_H

#include <functional>

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class KisVersioningDriver;
class KisVersioningSettings;

/**
 * Tells the backend which project files this artist is editing, and finds out
 * who else is editing the file being opened. The notice is advisory: nobody is
 * ever prevented from editing.
 *
 * A file counts when it lies inside the local folder of a downloaded project of
 * the driver and is not one of the files that are never synchronised. A driver
 * without the Presence capability makes every slot do nothing.
 *
 * Any failure to announce is silent: with no network, or when another person's
 * notice is already registered, nothing is shown and the next announcement is
 * still attempted.
 *
 * While held (a synchronisation is under way), nothing reaches the driver: what
 * is asked meanwhile is sent, in order, once released.
 */
class ProjectsPresence : public QObject
{
    Q_OBJECT
public:
    ProjectsPresence(KisVersioningDriver *driver, KisVersioningSettings *settings, QObject *parent = nullptr);
    ~ProjectsPresence() override;

public Q_SLOTS:
    /// The first change to a project file: announces it once.
    void documentModified(const QString &absolutePath);
    /// A document was closed; its notice stays when it had changes that were not sent.
    void documentClosed(const QString &absolutePath, bool hadChanges);
    /// The work of the project was sent: every notice of its files is withdrawn.
    void projectSynchronised(const QString &projectPath);
    /// A document is shown to the artist: asks who is editing it.
    void documentActivated(const QString &absolutePath);
    void setHeld(bool held);

Q_SIGNALS:
    void someoneElseIsEditing(const QString &absolutePath, const QString &user);

private:
    struct Entry {
        QString projectPath;
        QString relativePath;
        quint64 serial {0};
    };

    bool locate(const QString &absolutePath, QString *projectPath, QString *relativePath) const;
    void withdraw(const QString &projectPath, const QString &relativePath);
    /// Runs @p call now, or once released.
    void callDriver(const std::function<void()> &call);

    KisVersioningDriver *m_driver;
    KisVersioningSettings *m_settings;
    /// The files announced, or being announced, keyed by the form in which paths are compared.
    QHash<QString, Entry> m_announced;
    quint64 m_nextSerial {1};
    bool m_held {false};
    QList<std::function<void()>> m_waiting;
};

#endif // PROJECTSPRESENCE_H
