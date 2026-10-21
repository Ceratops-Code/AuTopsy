/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2009 Audacity Team
   File License: wxWidgets

   Dan Horgan

******************************************************************//**

\file CommandHandler.cpp
\brief Contains definitions for the CommandHandler class.

\class CommandHandler
\brief Contains methods for applying commands that are passed to it.

*//*******************************************************************/


#include "CommandHandler.h"

#include "../ActiveProject.h"
#include "Project.h"
#include "AppCommandEvent.h"
#include "ScriptCommandRelay.h"
#include "CommandContext.h"
#include "../commands/Command.h"
#include "Viewport.h"

#include <algorithm>

CommandHandler::CommandHandler()
{
}

CommandHandler::~CommandHandler()
{
}

void CommandHandler::OnReceiveCommand(AppCommandEvent &event)
{
   // First retrieve the actual command from the event 'envelope'.
   OldStyleCommandPointer cmd = event.GetCommand();

   if (const auto pProject = cmd->mProject.weak_from_this().lock()) {
      // Then apply it to current application & project.  Note that the
      // command may change the context - for example, switching to a
      // different project.
      CommandContext context{ *pProject };
      auto result = GuardedCall<bool>( [&] {
         return cmd->Apply( context );
      });
      wxUnusedVar(result);

      // Keep the original redraw target when a command switches projects,
      // but never redraw its window after Close or Quit has removed it.
      const auto openProjects = AllProjects{};
      if (std::find(openProjects.begin(), openProjects.end(), pProject)
            != openProjects.end())
         Viewport::Get(*pProject).Redraw();
   }
}
