/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LOCALFOLDERDRIVERPLUGIN_H
#define LOCALFOLDERDRIVERPLUGIN_H

#include <QObject>
#include <QVariant>

/// Registers the local folder driver in KisVersioningDriverRegistry when the plugin loads.
class LocalFolderDriverPlugin : public QObject
{
    Q_OBJECT
public:
    LocalFolderDriverPlugin(QObject *parent, const QVariantList &);
    ~LocalFolderDriverPlugin() override;
};

#endif // LOCALFOLDERDRIVERPLUGIN_H
