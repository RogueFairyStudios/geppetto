/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningFileRules.h"

#include <QRegularExpression>

#include <KConfigGroup>
#include <KSharedConfig>
#include <klocalizedstring.h>

namespace KisVersioning {

bool isNeverSynced(const QString &path, const QString &backupSuffix)
{
    const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);

    // The same expressions KisDocument uses to recognise its autosave files.
    static const QRegularExpression autosavePattern1(QStringLiteral("^\\..+-autosave.kra$"));
    static const QRegularExpression autosavePattern2(QStringLiteral("^.+-autosave.kra$"));
    if (autosavePattern1.match(name).hasMatch() || autosavePattern2.match(name).hasMatch()) {
        return true;
    }
    return !backupSuffix.isEmpty() && name.endsWith(backupSuffix);
}

QString configuredBackupSuffix()
{
    // The same entry, in the same group, as KisDocument reads when it makes a backup.
    return KSharedConfig::openConfig()->group(QString()).readEntry("backupfilesuffix", QStringLiteral("~"));
}

QString conflictCopyName(const QString &fileName, const QString &userName, const QStringList &existingNames)
{
    QString base = fileName;
    QString extension;
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        base = fileName.left(dot);
        extension = fileName.mid(dot);
    }

    static const QRegularExpression forbidden(QStringLiteral("[/\\\\:*?\"<>|]"));
    QString user = userName;
    user.replace(forbidden, QStringLiteral("_"));

    // Windows and macOS file systems ignore case, so the names are compared that way.
    QStringList taken;
    for (const QString &name : existingNames) {
        taken << name.toLower();
    }

    for (int number = user.isEmpty() ? 2 : 1;; ++number) {
        QString candidate;
        if (user.isEmpty()) {
            candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(number).arg(extension);
        } else if (number == 1) {
            candidate = QStringLiteral("%1 (%2)%3").arg(base, user, extension);
        } else {
            // "botao (Bruno) 2.kra": the number goes after the closing parenthesis.
            candidate = QStringLiteral("%1 (%2) %3%4").arg(base, user).arg(number).arg(extension);
        }
        if (!taken.contains(candidate.toLower())) {
            return candidate;
        }
    }
}

QString defaultNotes(int changedFiles)
{
    return i18np("1 file changed", "%1 files changed", changedFiles);
}

}
