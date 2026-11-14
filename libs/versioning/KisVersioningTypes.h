/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVERSIONINGTYPES_H
#define KISVERSIONINGTYPES_H

#include <QDateTime>
#include <QFlags>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace KisVersioning {

enum class FileState { Unchanged, Modified, Added, Deleted };
enum class DivergenceKind { BothModified, DeletedHereModifiedThere, ModifiedHereDeletedThere };
enum class Resolution { Mine, Theirs, MergedFile };
enum Capability { SignIn = 0x1, RemoteProjects = 0x2, PartialDownload = 0x4, Presence = 0x8, Notes = 0x10 };
Q_DECLARE_FLAGS(Capabilities, Capability)

struct ProjectInfo   { QString id; QString name; QString serverAddress; };
struct FileStatus    { QString path; FileState state = FileState::Unchanged; };
struct ProjectStatus { QVector<FileStatus> files; int incoming = 0; int pendingUpload = 0;
                       QString serverVersion; };   ///< the latest version on the server; empty when not known
struct Divergence    { QString path; DivergenceKind kind = DivergenceKind::BothModified; QString otherAuthor; };
struct SyncResult    { QVector<Divergence> divergences; QStringList received; QStringList sent;
                       bool pendingUpload = false; bool serverChanged = false; };
struct VersionInfo   { QString id; int number = 0; QString author; QDateTime date; QString notes; bool isMerge = false; };
struct EditingInfo   { QString path; QString user; };

} // namespace KisVersioning

Q_DECLARE_OPERATORS_FOR_FLAGS(KisVersioning::Capabilities)

Q_DECLARE_METATYPE(KisVersioning::ProjectInfo)
Q_DECLARE_METATYPE(KisVersioning::FileStatus)
Q_DECLARE_METATYPE(KisVersioning::ProjectStatus)
Q_DECLARE_METATYPE(KisVersioning::Divergence)
Q_DECLARE_METATYPE(KisVersioning::SyncResult)
Q_DECLARE_METATYPE(KisVersioning::VersionInfo)
Q_DECLARE_METATYPE(KisVersioning::EditingInfo)

#endif // KISVERSIONINGTYPES_H
