/**
 ******************************************************************************
 *
 * @file       GCSControlplugin.h
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup GCSControlGadgetPlugin GCSControl Gadget Plugin
 * @{
 * @brief A gadget to control the UAV, either from the keyboard or a joystick
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef GCSControlPLUGIN_H_
#define GCSControlPLUGIN_H_

#include <extensionsystem/iplugin.h>

#if defined(USE_SDL)
#include "sdlgamepad/sdlgamepad.h"
#endif

class GCSControlGadgetFactory;

class GCSControlPlugin : public ExtensionSystem::IPlugin
{
public:
    GCSControlPlugin();
   ~GCSControlPlugin();

   void extensionsInitialized();
   bool initialize(const QStringList & arguments, QString * errorString);
   void shutdown();

#if defined(USE_SDL)
   SDLGamepad *sdlGamepad;
#endif

private:
   GCSControlGadgetFactory *mf;

};
#endif /* GCSControlPLUGIN_H_ */
