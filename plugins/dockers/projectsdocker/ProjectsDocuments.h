/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSDOCUMENTS_H
#define PROJECTSDOCUMENTS_H

#include <QStringList>

#include <KisVersioningSyncSession.h>

/**
 * What the Projects panel needs to know about the documents open in the
 * application, beyond what the synchronisation session asks. The application
 * answers through ProjectsDocumentHost; the tests answer with a fake.
 * Paths are absolute.
 */
class ProjectsDocuments : public KisVersioningDocumentHost
{
public:
    ~ProjectsDocuments() override = default;

    /// Every open document inside @p folder, with or without unsaved changes.
    virtual QStringList openDocumentsIn(const QString &folder) const = 0;
    /// Open documents of @p projectPath with unsaved changes whose format is not Krita's own.
    virtual QStringList unsavedForeignDocuments(const QString &projectPath) const = 0;
};

#endif // PROJECTSDOCUMENTS_H
