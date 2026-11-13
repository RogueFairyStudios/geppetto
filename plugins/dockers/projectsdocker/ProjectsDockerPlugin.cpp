/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ProjectsDockerPlugin.h"

#include <kpluginfactory.h>

#include <KoDockFactoryBase.h>
#include <KoDockRegistry.h>

#include <KisVersioningTranslations.h>

#include "ProjectsDock.h"

K_PLUGIN_FACTORY_WITH_JSON(ProjectsDockerPluginFactory, "kritaprojectsdocker.json",
                           registerPlugin<ProjectsDockerPlugin>();)

namespace {

class ProjectsDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override
    {
        return QStringLiteral("ProjectsDocker");
    }

    QDockWidget *createDockWidget() override
    {
        ProjectsDock *dock = new ProjectsDock();
        dock->setObjectName(id());
        return dock;
    }

    DockPosition defaultDockPosition() const override
    {
        return DockRight;
    }
};

} // namespace

ProjectsDockerPlugin::ProjectsDockerPlugin(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    KisVersioning::registerTranslations();
    // The registry owns the factory.
    KoDockRegistry::instance()->add(new ProjectsDockFactory());
}

ProjectsDockerPlugin::~ProjectsDockerPlugin() = default;

#include "ProjectsDockerPlugin.moc"
