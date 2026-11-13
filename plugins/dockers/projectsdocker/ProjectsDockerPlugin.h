/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTSDOCKERPLUGIN_H
#define PROJECTSDOCKERPLUGIN_H

#include <QObject>
#include <QVariant>

/// Registers the "Projects" panel when the plugin loads.
class ProjectsDockerPlugin : public QObject
{
    Q_OBJECT
public:
    ProjectsDockerPlugin(QObject *parent, const QVariantList &);
    ~ProjectsDockerPlugin() override;
};

#endif // PROJECTSDOCKERPLUGIN_H
