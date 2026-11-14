/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGFILERULES_H
#define KISVERSIONINGFILERULES_H

#include <QString>
#include <QStringList>

#include "kritaversioning_export.h"

namespace KisVersioning {

/**
 * True for files that are never synchronised: Krita's autosave files and
 * its backup files, which end with @p backupSuffix (give it
 * configuredBackupSuffix()). @p path is relative to the project root, with
 * '/' as the separator (a bare file name works too).
 */
KRITAVERSIONING_EXPORT bool isNeverSynced(const QString &path, const QString &backupSuffix);

/**
 * The suffix Krita gives its backup files, as the artist set it in the
 * application's settings ("~" unless changed). Read it on the interface thread
 * and hand it to workers.
 */
KRITAVERSIONING_EXPORT QString configuredBackupSuffix();

/**
 * The name of the copy kept when two people changed the same file, for
 * example "botao (Bruno).kra". A number is added when the name is already in
 * @p existingNames. Characters not allowed in file names are replaced in
 * @p userName; without a user name the copy carries only the number.
 */
KRITAVERSIONING_EXPORT QString conflictCopyName(const QString &fileName,
                                                const QString &userName,
                                                const QStringList &existingNames);

/** The notes text proposed when the user gives none. */
KRITAVERSIONING_EXPORT QString defaultNotes(int changedFiles);

}

#endif // KISVERSIONINGFILERULES_H
