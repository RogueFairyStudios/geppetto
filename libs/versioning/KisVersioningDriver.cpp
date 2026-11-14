/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningDriver.h"

KisVersioningDriver::KisVersioningDriver(QObject *parent)
    : QObject(parent)
{
}

KisVersioningDriver::~KisVersioningDriver() = default;

KisVersioningJob *KisVersioningDriver::resumeSession(const QString &)
{
    auto *job = new KisVersioningJob(this);
    job->finish(QVariant(currentUser()));
    return job;
}

KisVersioningDriverFactory::~KisVersioningDriverFactory() = default;
