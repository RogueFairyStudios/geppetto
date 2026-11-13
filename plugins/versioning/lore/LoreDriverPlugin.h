/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LOREDRIVERPLUGIN_H
#define LOREDRIVERPLUGIN_H

#include <QObject>
#include <QVariant>

/// Registers the Lore driver in KisVersioningDriverRegistry when the plugin loads.
class LoreDriverPlugin : public QObject
{
    Q_OBJECT
public:
    LoreDriverPlugin(QObject *parent, const QVariantList &);
    ~LoreDriverPlugin() override;
};

#endif // LOREDRIVERPLUGIN_H
