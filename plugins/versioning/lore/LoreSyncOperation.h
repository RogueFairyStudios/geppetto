/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LORESYNCOPERATION_H
#define LORESYNCOPERATION_H

#include <QString>

#include <KisVersioningTypes.h>

class KisVersioningJob;
struct LoreSettings;

/**
 * Synchronises a downloaded project: creates a version of what changed, sends it, and receives
 * what others sent. The order of the calls is the one the prototype recorded: stage, commit,
 * push; when the server holds versions this computer does not have, the two sides are merged.
 *
 * The merge is started without letting the library commit it, because the library merges text
 * files line by line by itself: a file both people changed must never become a file neither of
 * them made with nobody told. Every file both sides changed, merged by the library or not, is
 * reported as a divergence, with the artist's own bytes back in the working file.
 */
KisVersioningJob *startLoreSynchronise(const LoreSettings &settings, const QString &localPath, const QString &notes);

/// Settles one divergence reported by startLoreSynchronise(). The next synchronisation creates the
/// version and sends it.
KisVersioningJob *startLoreResolve(const LoreSettings &settings, const QString &localPath, const QString &path,
                                   KisVersioning::Resolution resolution, const QString &mergedFile);

#endif // LORESYNCOPERATION_H
