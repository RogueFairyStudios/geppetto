/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisVersioningDriver.h"

KisVersioningDriver::KisVersioningDriver(QObject *parent)
    : QObject(parent)
{
}

KisVersioningDriver::~KisVersioningDriver() = default;

KisVersioningDriverFactory::~KisVersioningDriverFactory() = default;
