/**********************************************************************

Audacity: A Digital Audio Editor

@file WaveTrackMenuItems.cpp
@brief Injects menu items using WaveTrack but not the views of it

Paul Licameli split from TrackMenus.cpp

**********************************************************************/

#include "CommonCommandFlags.h"
#include "ProjectHistory.h"
#include "ProjectRate.h"

#include "SelectUtilities.h"
#include "TrackFocus.h"
#include "Viewport.h"
#include "WaveTrack.h"
#include "CommandContext.h"
#include "MenuRegistry.h"
#include "QualitySettings.h"
#include "TrackUtilities.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include "SettingsVisitor.h"

namespace {
class AddRelativeAudioTrackCommand : public AudacityCommand {
   int mTrack{};
   bool mBefore;
protected:
   explicit AddRelativeAudioTrackCommand(bool before) : mBefore(before) {}
public:
   TranslatableString GetDescription() const override
   { return XO("Insert an empty audio track before or after an existing track."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   { visitor.Define(mTrack, wxT("Track"), 0, 0, 100000); return true; }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool PromptUser(AudacityProject &project) override
   {
      const auto *focused = TrackFocus::Get(project).Get();
      mTrack = 0;
      for (const auto *track : TrackList::Get(project)) {
         if (track == focused) return true;
         ++mTrack;
      }
      return false;
   }
   bool Apply(const CommandContext &context) override
   {
      int index{};
      for (auto *track : TrackList::Get(context.project)) {
         if (index++ == mTrack)
            return TrackUtilities::InsertAudioTrackRelativeTo(context.project, *track, mBefore);
      }
      context.Error(_("The anchor track does not exist."));
      return false;
   }
};
class AddAudioTrackBeforeCommand final : public AddRelativeAudioTrackCommand {
public:
   AddAudioTrackBeforeCommand() : AddRelativeAudioTrackCommand(true) {}
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
class AddAudioTrackAfterCommand final : public AddRelativeAudioTrackCommand {
public:
   AddAudioTrackAfterCommand() : AddRelativeAudioTrackCommand(false) {}
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol AddAudioTrackBeforeCommand::Symbol{ XO("AddAudioTrackBefore") };
const ComponentInterfaceSymbol AddAudioTrackAfterCommand::Symbol{ XO("AddAudioTrackAfter") };
BuiltinCommandsModule::Registration<AddAudioTrackBeforeCommand> sAddBefore;
BuiltinCommandsModule::Registration<AddAudioTrackAfterCommand> sAddAfter;
}

namespace {
using namespace MenuRegistry;

void OnNewWaveTrack(const CommandContext &context)
{
   auto &project = context.project;
   auto &tracks = TrackList::Get( project );
   auto &trackFactory = WaveTrackFactory::Get( project );

   auto defaultFormat = QualitySettings::SampleFormatChoice();

   auto rate = ProjectRate::Get(project).GetRate();

   auto track = trackFactory.Create(defaultFormat, rate);
   track->SetName(tracks.MakeUniqueTrackName(WaveTrack::GetDefaultAudioTrackNamePreference()));
   tracks.Add(track);
   SelectUtilities::SelectNone( project );

   track->SetSelected(true);

   ProjectHistory::Get( project )
      .PushState(XO("Created new audio track"), XO("New Track"));

   TrackFocus::Get(project).Set(track.get());
   Viewport::Get(project).ShowTrack(*track);
}

void OnNewStereoTrack(const CommandContext &context)
{
   auto &project = context.project;
   auto &tracks = TrackList::Get( project );
   auto &trackFactory = WaveTrackFactory::Get( project );

   auto defaultFormat = QualitySettings::SampleFormatChoice();
   auto rate = ProjectRate::Get(project).GetRate();

   SelectUtilities::SelectNone( project );

   tracks.Add(trackFactory.Create(2, defaultFormat, rate));
   auto &newTrack = **tracks.rbegin();
   newTrack.SetSelected(true);
   newTrack.SetName(tracks.MakeUniqueTrackName(WaveTrack::GetDefaultAudioTrackNamePreference()));

   ProjectHistory::Get( project )
      .PushState(XO("Created new stereo audio track"), XO("New Track"));

   TrackFocus::Get(project).Set(&newTrack);
   Viewport::Get(project).ShowTrack(newTrack);
}

AttachedItem sAttachment{
   Items( "",
      Command( wxT("NewMonoTrack"), XXO("&Mono Track"), OnNewWaveTrack,
         AudioIONotBusyFlag(), wxT("Ctrl+Shift+N") ),
      Command( wxT("NewStereoTrack"), XXO("&Stereo Track"),
         OnNewStereoTrack, AudioIONotBusyFlag() ),
      Command("AddAudioTrackBefore", XXO("Audio Track Before Focused Track"),
         CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag() | TracksExistFlag()),
      Command("AddAudioTrackAfter", XXO("Audio Track After Focused Track"),
         CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag() | TracksExistFlag())
   ),
   wxT("Tracks/Add/Add")
};

}
