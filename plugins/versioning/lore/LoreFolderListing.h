/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LOREFOLDERLISTING_H
#define LOREFOLDERLISTING_H

#include <KisVersioningTypes.h>

class KisVersioningJob;
struct LoreSettings;

/**
 * Lists the top-level folders of a project without downloading its files: a clone with no files
 * into a temporary folder, then the library's revision tree. The clone is removed from the disk
 * and from the library's registry before the job ends. The result is a QStringList.
 */
KisVersioningJob *startLoreFolderListing(const LoreSettings &settings, const KisVersioning::ProjectInfo &project);

#endif // LOREFOLDERLISTING_H
