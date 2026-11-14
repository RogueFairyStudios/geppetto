/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningTranslations.h"

#include <QDir>
#include <QFileInfo>
#include <QtGlobal>

#include <KoResourcePaths.h>
#include <klocalizedstring.h>

namespace KisVersioning {

namespace {

bool holdsCatalogue(const QString &folder)
{
    const QStringList languages = QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &language : languages) {
        if (QFileInfo::exists(QDir(folder).filePath(language + QStringLiteral("/LC_MESSAGES/geppetto.mo")))) {
            return true;
        }
    }
    return false;
}

} // namespace

QStringList translationsFolders(const QString &applicationRoot)
{
    QStringList folders;
    for (const QString &relative : {QStringLiteral("bin/locale"), QStringLiteral("share/locale")}) {
        const QString folder = QDir(applicationRoot + relative).absolutePath();
        if (holdsCatalogue(folder)) {
            folders << folder;
        }
    }
    if (folders.isEmpty()) {
        folders << QDir(applicationRoot + QStringLiteral("share/locale")).absolutePath();
    }
    return folders;
}

void registerTranslationsIn(const QString &applicationRoot)
{
    // Only folders that hold a catalogue are registered, so whichever of them KI18n asks has it.
    const QStringList folders = translationsFolders(applicationRoot);
    for (const QString &folder : folders) {
        KLocalizedString::addDomainLocaleDir("geppetto", folder);
    }
}

void registerTranslations()
{
    // The same root, and the same condition, as krita/main.cc uses for the "krita" domain.
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) || defined Q_OS_WIN || defined Q_OS_MACOS
    static const bool registered = []() {
        registerTranslationsIn(KoResourcePaths::getApplicationRoot());
        return true;
    }();
    Q_UNUSED(registered);
#endif
}

}
