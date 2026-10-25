/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2018 Audacity Team
   File License: wxWidgets

   Dan Horgan

******************************************************************//**

\file ScriptCommandRelay.cpp
\brief Contains definitions for ScriptCommandRelay

*//****************************************************************//**

\class ScriptCommandRelay
\brief ScriptCommandRelay is just a way to move some of the scripting-specific
code out of ModuleManager.

*//*******************************************************************/

#include "ScriptCommandRelay.h"

#include "CommandTargets.h"
#include "CommandBuilder.h"
#include "ActiveProject.h"
#include "AppCommandEvent.h"
#include "Project.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include <wx/app.h>
#include <exception>
#include <future>
#include <string>
#include <thread>
#include <utility>

/// This is the function which actually obeys one command.
static int ExecCommand(wxString *pIn, wxString *pOut, bool fromMain)
{
   if (!fromMain) {
      // Project references, command construction, and their final destruction
      // belong to the GUI thread. Transfer only UTF-8 values across threads;
      // otherwise Close can make the worker the last owner of a GUI project.
      const auto requestBytes = pIn->ToUTF8();
      std::string request = requestBytes
         ? std::string{ requestBytes.data(), requestBytes.length() } : std::string{};
      auto completion = std::make_shared<std::promise<std::string>>();
      auto response = completion->get_future();
      auto dispatch = [request = std::move(request), completion] {
         try {
            wxString command = wxString::FromUTF8(request.c_str(), request.size());
            wxString reply;
            ExecCommand(&command, &reply, true);
            const auto replyBytes = reply.ToUTF8();
            completion->set_value(replyBytes
               ? std::string{ replyBytes.data(), replyBytes.length() } : std::string{});
         }
         catch (const std::exception &error) {
            completion->set_value(std::string{ "Scripting dispatch failed: " } +
               error.what() + "\nBatchCommand finished: Failed!\n");
         }
         catch (...) {
            completion->set_value("Scripting dispatch failed with an unknown exception.\n"
               "BatchCommand finished: Failed!\n");
         }
      };
      wxTheApp->CallAfter([dispatch = std::move(dispatch)] {
         // A modal window can be hidden before its stack frame has returned.
         // Construct no native command until that owner has released its guard;
         // waiting here on the GUI thread would deadlock the dialog itself.
         if (!ClipActions::QueueCommandDuringMediaDialog(dispatch)) dispatch();
      });
      const auto reply = response.get();
      *pOut = wxString::FromUTF8(reply.c_str(), reply.size());
      return 0;
   }

   if (auto pProject = ::GetActiveProject().lock()) {
      CommandBuilder builder(*pProject, *pIn);
      if (builder.WasValid())
      {
         OldStyleCommandPointer cmd = builder.GetCommand();

         AppCommandEvent ev;
         ev.SetCommand(cmd);

         // Both script entry points now execute commands on the GUI thread.
         wxTheApp->SafelyProcessEvent(ev);
      }

      // Wait for and retrieve the response
      *pOut = builder.GetResponse();
   }
   else
      *pOut = wxT("No project is open.\nBatchCommand finished: Failed!\n");

   return 0;
}

/// Executes a command in the worker (script) thread
static int ExecFromWorker(wxString *pIn, wxString *pOut)
{
   return ExecCommand(pIn, pOut, false);
}

/// Executes a command on the main (GUI) thread.
int ExecFromMain(wxString *pIn, wxString *pOut)
{
   return ExecCommand(pIn, pOut, true);
}

/// Starts the script server
void ScriptCommandRelay::StartScriptServer(tpRegScriptServerFunc scriptFn)
{
   wxASSERT(scriptFn != NULL);

   auto server = [](tpRegScriptServerFunc function)
   {
      while (true)
      {
         function(ExecFromWorker);
      }
   };

   std::thread(server, scriptFn).detach();
}
