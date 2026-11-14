/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsDocumentHost.h"

#include <algorithm>
#include <memory>

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

#include <KisDocument.h>
#include <KisImportExportErrorCode.h>
#include <KisImportExportUtils.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <kis_properties_configuration.h>

#include <KisVersioningFileRules.h>

ProjectsDocumentHost::ProjectsDocumentHost(QObject *parent)
    : QObject(parent)
{
    // Every save the application finishes, the artist's or a synchronisation's.
    connect(KisPart::instance(), &KisPart::sigDocumentSaved, this, [this](const QString &path) {
        m_lastSaved.insert(pathKey(path), QDateTime::currentDateTimeUtc());
    });
}

ProjectsDocumentHost::~ProjectsDocumentHost() = default;

QString ProjectsDocumentHost::pathKey(const QString &path)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return clean.toLower();
#else
    return clean;
#endif
}

bool ProjectsDocumentHost::isInside(const QString &path, const QString &folder)
{
    if (path.isEmpty() || folder.isEmpty()) {
        return false;
    }
    const QString pathKeyed = pathKey(path);
    const QString folderKeyed = pathKey(folder);
    if (pathKeyed == folderKeyed) {
        return true;
    }
    return pathKeyed.startsWith(folderKeyed.endsWith(QLatin1Char('/')) ? folderKeyed : folderKeyed + QLatin1Char('/'));
}

QList<KisDocument *> ProjectsDocumentHost::documentsAt(const QString &absolutePath)
{
    QList<KisDocument *> found;
    const QString key = pathKey(absolutePath);
    const QList<QPointer<KisDocument>> documents = KisPart::instance()->documents();
    for (const QPointer<KisDocument> &document : documents) {
        if (document && !document->path().isEmpty() && pathKey(document->path()) == key) {
            found << document;
        }
    }
    return found;
}

QStringList ProjectsDocumentHost::modifiedOpenDocuments(const QString &projectPath) const
{
    QStringList paths;
    const QList<QPointer<KisDocument>> documents = KisPart::instance()->documents();
    for (const QPointer<KisDocument> &document : documents) {
        if (!document || !document->isModified()) {
            continue;
        }
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(document->path()));
        if (isInside(path, projectPath) && !KisVersioning::isNeverSynced(QFileInfo(path).fileName())
            && !paths.contains(path)) {
            paths << path;
        }
    }
    return paths;
}

QStringList ProjectsDocumentHost::openDocumentsIn(const QString &folder) const
{
    QStringList paths;
    const QList<QPointer<KisDocument>> documents = KisPart::instance()->documents();
    for (const QPointer<KisDocument> &document : documents) {
        if (!document || document->path().isEmpty()) {
            continue;
        }
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(document->path()));
        if (isInside(path, folder) && !paths.contains(path)) {
            paths << path;
        }
    }
    return paths;
}

QStringList ProjectsDocumentHost::unsavedForeignDocuments(const QString &projectPath) const
{
    QStringList paths;
    const QList<QPointer<KisDocument>> documents = KisPart::instance()->documents();
    for (const QPointer<KisDocument> &document : documents) {
        if (!document || !document->isModified() || isNativeFormat(document)) {
            continue;
        }
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(document->path()));
        if (isInside(path, projectPath) && !KisVersioning::isNeverSynced(QFileInfo(path).fileName())
            && !paths.contains(path)) {
            paths << path;
        }
    }
    return paths;
}

bool ProjectsDocumentHost::isNativeFormat(const KisDocument *document)
{
    return document->mimeType() == KisDocument::nativeFormatMimeType();
}

void ProjectsDocumentHost::saveDocument(const QString &absolutePath, std::function<void(bool)> done)
{
    QPointer<KisDocument> document;
    const QList<KisDocument *> open = documentsAt(absolutePath);
    for (KisDocument *candidate : open) {
        if (candidate->isModified()) {
            document = candidate;
            break;
        }
    }
    if (!document) {
        // Saved by the artist in the meantime, the file on disk is current; closed, it is gone.
        done(!open.isEmpty());
        return;
    }

    struct Pending {
        std::function<void(bool)> done;
        QList<QMetaObject::Connection> connections;
        bool ended = false;
    };
    auto pending = std::make_shared<Pending>();
    pending->done = std::move(done);
    // Reports the save once, then stops listening to the document.
    auto end = [pending](bool ok) {
        if (pending->ended) {
            return;
        }
        pending->ended = true;
        for (const QMetaObject::Connection &connection : std::as_const(pending->connections)) {
            QObject::disconnect(connection);
        }
        pending->done(ok);
    };

    // KisDocument::slotCompleteSavingDocument emits completed() after a successful save, once the
    // document is marked as saved. A failed or cancelled background save emits no signal of its own
    // (canceled() belongs to opening a file); its status arrives through sigCompleteBackgroundSaving.
    pending->connections << connect(document, &KisDocument::completed, this, [end]() { end(true); });
    pending->connections << connect(document, &KisDocument::sigCompleteBackgroundSaving, this,
                                    [end](const KritaUtils::ExportFileJob &, KisImportExportErrorCode status,
                                          const QString &, const QString &) {
                                        if (!status.isOk()) {
                                            end(false);
                                        }
                                    });
    pending->connections << connect(document, &QObject::destroyed, this, [end]() { end(false); });

    if (!document->save(false, nullptr)) {
        end(false);
    } else if (!pending->ended && document && !document->isSaving()) {
        // Saved without going to the background (Krita does that for files in its resource
        // folders): no signal follows.
        end(!document->isModified());
    }
}

bool ProjectsDocumentHost::savedSince(const QString &absolutePath, const QDateTime &moment) const
{
    const QDateTime saved = m_lastSaved.value(pathKey(absolutePath));
    return saved.isValid() && saved > moment;
}

bool ProjectsDocumentHost::isOpenAndUnmodified(const QString &absolutePath) const
{
    const QList<KisDocument *> open = documentsAt(absolutePath);
    return !open.isEmpty()
        && std::none_of(open.begin(), open.end(), [](KisDocument *document) { return document->isModified(); });
}

bool ProjectsDocumentHost::isOpenAndModified(const QString &absolutePath) const
{
    const QList<KisDocument *> open = documentsAt(absolutePath);
    return std::any_of(open.begin(), open.end(), [](KisDocument *document) { return document->isModified(); });
}

bool ProjectsDocumentHost::showDocument(const QString &absolutePath)
{
    const QList<KisDocument *> open = documentsAt(absolutePath);
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    if (open.isEmpty() || !window) {
        return false;
    }
    window->showDocument(open.first());
    return true;
}

void ProjectsDocumentHost::reloadDocument(const QString &absolutePath)
{
    // Not from inside the session's step: opening a file runs parts of the event loop.
    QMetaObject::invokeMethod(this, [this, absolutePath]() { reloadNow(absolutePath); }, Qt::QueuedConnection);
}

QList<KisDocument *> ProjectsDocumentHost::documentsToReload(const QString &absolutePath) const
{
    const QList<KisDocument *> open = documentsAt(absolutePath);
    for (KisDocument *document : open) {
        // Reloading would throw away unsaved work, or a save under way.
        if (document->isModified() || document->isSaving()) {
            return {};
        }
    }
    return open;
}

void ProjectsDocumentHost::reloadNow(const QString &absolutePath)
{
    // Asked again now: the artist may have changed the document since the session looked.
    const QList<KisDocument *> documents = documentsToReload(absolutePath);
    for (KisDocument *document : documents) {
        reload(document);
    }
}

void ProjectsDocumentHost::reload(const QPointer<KisDocument> &document)
{
    KisPart *part = KisPart::instance();
    QList<QPointer<KisView>> views;
    const QList<QPointer<KisView>> allViews = part->views();
    for (const QPointer<KisView> &view : allViews) {
        if (view && view->document() == document) {
            if (view->mainWindow() && view->mainWindow()->hackIsSaving()) {
                return; // a window that is saving cannot let a view go
            }
            views << view;
        }
    }
    if (views.isEmpty()) {
        return; // nothing shows it
    }

    KisDocument *fresh = part->createDocument();
    if (!fresh->openPath(document->path(), KisDocument::DontAddToRecent)) {
        delete fresh;
        return;
    }
    // Opening runs parts of the event loop: everything is checked again before the swap.
    views.removeAll(QPointer<KisView>());
    if (!document || document->isModified() || document->isSaving() || views.isEmpty()) {
        fresh->deleteLater();
        return;
    }

    part->addDocument(fresh);
    for (const QPointer<KisView> &view : std::as_const(views)) {
        if (!view) {
            continue;
        }
        KisPropertiesConfiguration state;
        view->saveViewState(state);
        // The new view takes the old one's place in the window. When the last view of the old
        // document goes, the application closes that document.
        KisView *replacement = view->replaceBy(fresh);
        if (replacement) {
            replacement->restoreViewState(state);
        }
    }
}
