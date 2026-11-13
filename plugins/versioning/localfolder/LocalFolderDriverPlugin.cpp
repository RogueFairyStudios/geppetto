/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LocalFolderDriverPlugin.h"

#include <kpluginfactory.h>
#include <klocalizedstring.h>

#include <KisVersioningDriver.h>
#include <KisVersioningDriverRegistry.h>

#include "LocalFolderDriver.h"

K_PLUGIN_FACTORY_WITH_JSON(LocalFolderDriverPluginFactory, "kritaversioninglocalfolder.json",
                           registerPlugin<LocalFolderDriverPlugin>();)

namespace {

class LocalFolderDriverFactory : public KisVersioningDriverFactory
{
public:
    QString id() const override
    {
        return QStringLiteral("localfolder");
    }

    QString name() const override
    {
        return i18n("Folder on this computer");
    }

    KisVersioningDriver *create(QObject *parent) const override
    {
        return new LocalFolderDriver(parent);
    }
};

} // namespace

LocalFolderDriverPlugin::LocalFolderDriverPlugin(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    // The registry owns the factory.
    KisVersioningDriverRegistry::instance()->add(new LocalFolderDriverFactory());
}

LocalFolderDriverPlugin::~LocalFolderDriverPlugin() = default;

#include "LocalFolderDriverPlugin.moc"
