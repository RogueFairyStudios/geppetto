/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningDriverRegistry.h"

#include <QGlobalStatic>

#include <KoPluginLoader.h>

Q_GLOBAL_STATIC(KisVersioningDriverRegistry, s_instance)

KisVersioningDriverRegistry::KisVersioningDriverRegistry() = default;

KisVersioningDriverRegistry::~KisVersioningDriverRegistry()
{
    qDeleteAll(values());
}

void KisVersioningDriverRegistry::init()
{
    KoPluginLoader::PluginsConfig config;
    config.blacklist = "VersioningDriversDisabled";
    config.group = "krita";
    KoPluginLoader::instance()->load(QStringLiteral("Krita/VersioningDriver"), config);
}

KisVersioningDriverRegistry *KisVersioningDriverRegistry::instance()
{
    if (!s_instance.exists()) {
        s_instance->init();
    }
    return s_instance;
}
