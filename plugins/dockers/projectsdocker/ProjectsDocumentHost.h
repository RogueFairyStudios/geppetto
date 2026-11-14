/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSDOCUMENTHOST_H
#define PROJECTSDOCUMENTHOST_H

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include "ProjectsDocuments.h"

class KisDocument;

/**
 * The documents open in the application, as the synchronisation session sees
 * them. Paths are matched after cleaning them, whatever their separators, and
 * ignoring case on Windows and macOS.
 *
 * Saving runs in the background, as Krita always saves: done() comes from the
 * document's signals, never from a nested event loop. Only documents in Krita's
 * own format are saved; a save already under way is awaited first.
 *
 * Krita has no "reload" action, so reloading opens the file again as a new
 * document and puts it in the place of each view of the old one, keeping the
 * zoom and position of each view. A document with unsaved changes is never
 * reloaded.
 */
class ProjectsDocumentHost : public QObject, public ProjectsDocuments
{
    Q_OBJECT
public:
    explicit ProjectsDocumentHost(QObject *parent = nullptr);
    ~ProjectsDocumentHost() override;

    QStringList modifiedOpenDocuments(const QString &projectPath) const override;
    void saveDocument(const QString &absolutePath, std::function<void(bool)> done) override;
    bool isOpenAndUnmodified(const QString &absolutePath) const override;
    void reloadDocument(const QString &absolutePath) override;
    /// From the saves the application reported since this object was made.
    bool savedSince(const QString &absolutePath, const QDateTime &moment) const override;
    QStringList openDocumentsIn(const QString &folder) const override;
    QStringList unsavedForeignDocuments(const QString &projectPath) const override;

    /// True when a document of that file is open with unsaved changes.
    bool isOpenAndModified(const QString &absolutePath) const;
    /// Brings forward a view of the open document of that file; false when the file is not open.
    static bool showDocument(const QString &absolutePath);
    /// The open documents of that file that reloadDocument() may replace: none when any of them
    /// has unsaved changes or is being saved, and none when the file is not open.
    QList<KisDocument *> documentsToReload(const QString &absolutePath) const;

    /// The form in which two paths are compared.
    static QString pathKey(const QString &path);
    /// True when @p document is kept in Krita's own format, which saves with no loss and no question.
    static bool isNativeFormat(const KisDocument *document);
    /// True when @p path is @p folder or lies inside it.
    static bool isInside(const QString &path, const QString &folder);

Q_SIGNALS:
    /// A reload asked for did not happen (the document changed, a save or a stroke was under way,
    /// or the file could not be opened): the window shows a version the file no longer holds.
    void documentNotReloaded(const QString &absolutePath);

private:
    static QList<KisDocument *> documentsAt(const QString &absolutePath);
    void startSave(const QPointer<KisDocument> &document, std::function<void(bool)> done);
    void waitForSave(const QPointer<KisDocument> &document, std::function<void(bool)> done);
    void reloadNow(const QString &absolutePath, int attempt);
    bool reload(const QPointer<KisDocument> &document);

    /// When each file last finished saving (UTC), by the form in which paths are compared.
    QHash<QString, QDateTime> m_lastSaved;
};

#endif // PROJECTSDOCUMENTHOST_H
