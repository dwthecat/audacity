/**********************************************************************

  Audacity: A Digital Audio Editor

  Menus.cpp

  Dominic Mazzoni
  Brian Gunlogson
  et al.

*******************************************************************//**

\file Menus.cpp
\brief Functions for building toobar menus and enabling and disabling items

*//****************************************************************//**

\class MenuCreator
\brief MenuCreator is responsible for creating the main menu bar.

*//****************************************************************//**

\class MenuManager
\brief MenuManager handles updates to menu state.

*//*******************************************************************/


#include "Menus.h"



#include <wx/frame.h>

#include "Project.h"
#include "ProjectHistory.h"
#include "ProjectWindows.h"
#include "UndoManager.h"
#include "commands/CommandManager.h"
#include "AudacityMessageBox.h"
#include "BasicUI.h"

#include <unordered_set>

#include <wx/menu.h>
#include <wx/windowptr.h>
#include <wx/log.h>

MenuCreator::MenuCreator()
{
   mLastAnalyzerRegistration = repeattypenone;
   mLastToolRegistration = repeattypenone;
   
   mRepeatGeneratorFlags = 0;
   mRepeatEffectFlags = 0;
   mRepeatAnalyzerFlags = 0;
   mRepeatToolFlags = 0;
}

MenuCreator::~MenuCreator()
{
}

static const AudacityProject::AttachedObjects::RegisteredFactory key{
  []( AudacityProject &project ){
     return std::make_shared< MenuManager >( project ); }
};

MenuManager &MenuManager::Get( AudacityProject &project )
{
   return project.AttachedObjects::Get< MenuManager >( key );
}

const MenuManager &MenuManager::Get( const AudacityProject &project )
{
   return Get( const_cast< AudacityProject & >( project ) );
}

MenuManager::MenuManager( AudacityProject &project )
   : mProject{ project }
{
   UpdatePrefs();
   mUndoSubscription = UndoManager::Get(project)
      .Subscribe(*this, &MenuManager::OnUndoRedo);
}

MenuManager::~MenuManager()
{
}

void MenuManager::UpdatePrefs()
{
   bool bSelectAllIfNone;
   gPrefs->Read(wxT("/GUI/SelectAllOnNone"), &bSelectAllIfNone, false);
   // 0 is grey out, 1 is Autoselect, 2 is Give warnings.
#ifdef EXPERIMENTAL_DA
   // DA warns or greys out.
   mWhatIfNoSelection = bSelectAllIfNone ? 2 : 0;
#else
   // Audacity autoselects or warns.
   mWhatIfNoSelection = bSelectAllIfNone ? 1 : 2;
#endif
   mStopIfWasPaused = true;  // not configurable for now, but could be later.
}

std::pair<bool, bool> MenuTable::detail::VisitorBase::ShouldBeginGroup(
   const MenuItemProperties *pProperties)
{
   const auto properties =
      pProperties ? pProperties->GetProperties() : MenuItemProperties::None;

   bool inlined = false;
   bool shouldDoSeparator = false;

   switch (properties) {
   case MenuItemProperties::Inline: {
      inlined = true;
      break;
   }
   case MenuItemProperties::Section: {
      if (!needSeparator.empty())
         needSeparator.back() = true;
      break;
   }
   case MenuItemProperties::Whole:
   case MenuItemProperties::Extension: {
      shouldDoSeparator = ShouldDoSeparator();
      break;
   }
   default:
      break;
   }

   return { !inlined, shouldDoSeparator };
}

void MenuTable::detail::VisitorBase::AfterBeginGroup(
   const MenuItemProperties *pProperties)
{
   const auto properties =
      pProperties ? pProperties->GetProperties() : MenuItemProperties::None;

   bool isMenu = false;
   bool isExtension = false;

   switch (properties) {
   case MenuItemProperties::Whole:
   case MenuItemProperties::Extension: {
      isMenu = true;
      isExtension = (properties == MenuItemProperties::Extension);
      break;
   }
   default:
      break;
   }

   if (isMenu) {
      needSeparator.push_back(false);
      firstItem.push_back(!isExtension);
   }
}

bool MenuTable::detail::VisitorBase::ShouldEndGroup(
   const MenuItemProperties *pProperties)
{
   const auto properties =
      pProperties ? pProperties->GetProperties() : MenuItemProperties::None;

   bool inlined = false;

   switch (properties) {
   case MenuItemProperties::Inline: {
      inlined = true;
      break;
   }
   case MenuItemProperties::Section: {
      if ( !needSeparator.empty() )
         needSeparator.back() = true;
      break;
   }
   case MenuItemProperties::Whole:
   case MenuItemProperties::Extension: {
      firstItem.pop_back();
      needSeparator.pop_back();
      break;
   }
   default:
      break;
   }

   return !inlined;
}

bool MenuTable::detail::VisitorBase::ShouldDoSeparator()
{
   bool separate = false;
   if (!needSeparator.empty()) {
      separate = needSeparator.back() && !firstItem.back();
      needSeparator.back() = false;
      firstItem.back() = false;
   }
   return separate;
}

namespace MenuTable {

MenuItem::~MenuItem() {}
auto MenuItem::GetProperties() const -> Properties { return Whole; }

ConditionalGroupItem::~ConditionalGroupItem() {}

CommandItem::CommandItem(const CommandID &name_,
         const TranslatableString &label_in_,
         CommandFunctorPointer callback_,
         CommandFlag flags_,
         const CommandManager::Options &options_,
         CommandHandlerFinder finder_)
: SingleItem{ name_ }, label_in{ label_in_ }
, finder{ finder_ }, callback{ callback_ }
, flags{ flags_ }, options{ options_ }
{}
CommandItem::~CommandItem() {}

CommandGroupItem::CommandGroupItem(const Identifier &name_,
         std::vector< ComponentInterfaceSymbol > items_,
         CommandFunctorPointer callback_,
         CommandFlag flags_,
         bool isEffect_,
         CommandHandlerFinder finder_)
: SingleItem{ name_ }, items{ std::move(items_) }
, finder{ finder_ }, callback{ callback_ }
, flags{ flags_ }, isEffect{ isEffect_ }
{}
CommandGroupItem::~CommandGroupItem() {}

SpecialItem::~SpecialItem() {}
MenuPart::~MenuPart() {}
auto MenuPart::GetProperties() const -> Properties { return Section; }

MenuItems::~MenuItems() {}
auto MenuItems::GetOrdering() const -> Ordering {
   return name.empty() ? Anonymous : Weak;
}
auto MenuItems::GetProperties() const -> Properties { return Inline; }

MenuItemProperties::~MenuItemProperties() {}

CommandHandlerFinder FinderScope::sFinder =
   [](AudacityProject &project) -> CommandHandlerObject & {
      // If this default finder function is reached, then FinderScope should
      // have been used somewhere but was not, or an explicit
      // CommandHandlerFinder was not passed to menu item constructors
      wxASSERT( false );
      return project;
   };

}

/// CreateMenusAndCommands builds the menus, and also rebuilds them after
/// changes in configured preferences - for example changes in key-bindings
/// affect the short-cut key legend that appears beside each command,

namespace {

using namespace Registry;

const auto MenuPathStart = wxT("MenuBar");

}

auto MenuTable::ItemRegistry::Registry() -> Registry::GroupItem<Traits> &
{
   static GroupItem<Traits> registry{ MenuPathStart };
   return registry;
}

namespace {

using namespace MenuTable;

struct MenuItemVisitor : Visitor<Traits> {
   MenuItemVisitor(AudacityProject &proj, CommandManager &man)
   : Visitor<Traits> { std::tuple{
      // pre-visit
      std::tuple {
         [this](const MenuItem &menu, auto&) {
            manager.BeginMenu(menu.GetTitle());
         },
         [this](const ConditionalGroupItem &conditionalGroup, auto&) {
            const auto flag = conditionalGroup();
            if (!flag)
               manager.BeginOccultCommands();
            // to avoid repeated call of condition predicate in EndGroup():
            flags.push_back(flag);
         },
         [this](auto &item, auto&) {
            assert(IsSection(item));
         }
      },

      // leaf visit
      [this](const auto &item, const auto&) {
         const auto pCurrentMenu = manager.CurrentMenu();
         if (!pCurrentMenu) {
            // There may have been a mistake in the placement hint that registered
            // this single item.  It's not within any menu.
            assert(false);
         }
         else TypeSwitch::VDispatch<void, LeafTypes>(item,
            [&](const CommandItem &command) {
               manager.AddItem(mProject,
                  command.name, command.label_in,
                  command.finder, command.callback,
                  command.flags, command.options);
            },
            [&](const CommandGroupItem &commandList) {
               manager.AddItemList(commandList.name,
                  commandList.items.data(), commandList.items.size(),
                  commandList.finder, commandList.callback,
                  commandList.flags, commandList.isEffect);
            },
            [&](const SpecialItem &special) {
               special.fn(mProject, *pCurrentMenu);
            }
         );
      },

      // post-visit
      std::tuple {
         [this](const MenuItem &, const auto&) {
            manager.EndMenu();
         },
         [this](const ConditionalGroupItem &, const auto&) {
            const bool flag = flags.back();
            if (!flag)
               manager.EndOccultCommands();
            flags.pop_back();
         },
         [this](auto &item, auto&) {
            assert(IsSection(item));
         }
      }},

      [this]() {
         manager.AddSeparator();
      }
   }
   , mProject{ proj }
   , manager{ man }
   {}

   AudacityProject &mProject;
   CommandManager &manager;
   std::vector<bool> flags;
};
}

void MenuCreator::CreateMenusAndCommands(AudacityProject &project)
{
   // Once only, cause initial population of preferences for the ordering
   // of some menu items that used to be given in tables but are now separately
   // registered in several .cpp files; the sequence of registration depends
   // on unspecified accidents of static initialization order across
   // compilation units, so we need something specific here to preserve old
   // default appearance of menus.
   // But this needs only to mention some strings -- there is no compilation or
   // link dependency of this source file on those other implementation files.
   static Registry::OrderingPreferenceInitializer init{
      MenuPathStart,
      {
         {wxT(""), wxT(
"File,Edit,Select,View,Transport,Tracks,Generate,Effect,Analyze,Tools,Window,Optional,Help"
          )},
         {wxT("/Optional/Extra/Part1"), wxT(
"Transport,Tools,Mixer,Edit,PlayAtSpeed,Seek,Device,Select"
          )},
         {wxT("/Optional/Extra/Part2"), wxT(
"Navigation,Focus,Cursor,Track,Scriptables1,Scriptables2"
          )},
         {wxT("/View/Windows"), wxT("UndoHistory,Karaoke,MixerBoard")},
         {wxT("/Analyze/Analyzers/Windows"), wxT("ContrastAnalyser,PlotSpectrum")},
         {wxT("/Transport/Basic"), wxT("Play,Record,Scrubbing,Cursor")},
         {wxT("/View/Other/Toolbars/Toolbars/Other"), wxT(
"ShowTransportTB,ShowToolsTB,ShowRecordMeterTB,ShowPlayMeterTB,"
//"ShowMeterTB,"
"ShowMixerTB,"
"ShowEditTB,ShowTranscriptionTB,ShowScrubbingTB,ShowDeviceTB,ShowSelectionTB,"
"ShowSpectralSelectionTB") },
         {wxT("/Tracks/Add/Add"), wxT(
"NewMonoTrack,NewStereoTrack,NewLabelTrack,NewTimeTrack")},
         {wxT("/Optional/Extra/Part2/Scriptables1"), wxT(
"SelectTime,SelectFrequencies,SelectTracks,SetTrackStatus,SetTrackAudio,"
"SetTrackVisuals,GetPreference,SetPreference,SetClip,SetEnvelope,SetLabel"
"SetProject") },
         {wxT("/Optional/Extra/Part2/Scriptables2"), wxT(
"Select,SetTrack,GetInfo,Message,Help,Import2,Export2,OpenProject2,"
"SaveProject2,Drag,CompareAudio,Screenshot") },
      }
   };

   auto &commandManager = CommandManager::Get( project );

   // The list of defaults to exclude depends on
   // preference wxT("/GUI/Shortcuts/FullDefaults"), which may have changed.
   commandManager.SetMaxList();

   auto menubar = commandManager.AddMenuBar(wxT("appmenu"));
   wxASSERT(menubar);

   MenuItemVisitor visitor{ project, commandManager };
   MenuManager::Visit(visitor, project);

   GetProjectFrame( project ).SetMenuBar(menubar.release());

   mLastFlags = AlwaysEnabledFlag;

#if defined(_DEBUG)
//   c->CheckDups();
#endif
}

void MenuManager::Visit(
   MenuTable::Visitor<MenuTable::Traits> &visitor, AudacityProject &project)
{
   static const auto menuTree = MenuTable::Items( MenuPathStart );

   wxLogNull nolog;
   Registry::VisitWithFunctions(visitor, menuTree.get(),
      &MenuTable::ItemRegistry::Registry(), project);
}

// TODO: This surely belongs in CommandManager?
void MenuManager::ModifyUndoMenuItems(AudacityProject &project)
{
   TranslatableString desc;
   auto &undoManager = UndoManager::Get( project );
   auto &commandManager = CommandManager::Get( project );
   int cur = undoManager.GetCurrentState();

   if (undoManager.UndoAvailable()) {
      undoManager.GetShortDescription(cur, &desc);
      commandManager.Modify(wxT("Undo"),
         XXO("&Undo %s")
            .Format( desc ));
      commandManager.Enable(wxT("Undo"),
         ProjectHistory::Get( project ).UndoAvailable());
   }
   else {
      commandManager.Modify(wxT("Undo"),
                            XXO("&Undo"));
   }

   if (undoManager.RedoAvailable()) {
      undoManager.GetShortDescription(cur+1, &desc);
      commandManager.Modify(wxT("Redo"),
         XXO("&Redo %s")
            .Format( desc ));
      commandManager.Enable(wxT("Redo"),
         ProjectHistory::Get( project ).RedoAvailable());
   }
   else {
      commandManager.Modify(wxT("Redo"),
                            XXO("&Redo"));
      commandManager.Enable(wxT("Redo"), false);
   }
}

// Get hackcess to a protected method
class wxFrameEx : public wxFrame
{
public:
   using wxFrame::DetachMenuBar;
};

void MenuCreator::RebuildMenuBar(AudacityProject &project)
{
   // On OSX, we can't rebuild the menus while a modal dialog is being shown
   // since the enabled state for menus like Quit and Preference gets out of
   // sync with wxWidgets idea of what it should be.
#if defined(__WXMAC__) && defined(_DEBUG)
   {
      wxDialog *dlg =
         wxDynamicCast(wxGetTopLevelParent(wxWindow::FindFocus()), wxDialog);
      wxASSERT((!dlg || !dlg->IsModal()));
   }
#endif

   // Delete the menus, since we will soon recreate them.
   // Rather oddly, the menus don't vanish as a result of doing this.
   {
      auto &window = static_cast<wxFrameEx&>( GetProjectFrame( project ) );
      wxWindowPtr<wxMenuBar> menuBar{ window.GetMenuBar() };
      window.DetachMenuBar();
      // menuBar gets deleted here
   }

   CommandManager::Get( project ).PurgeData();

   CreateMenusAndCommands(project);
}

void MenuManager::OnUndoRedo(UndoRedoMessage message)
{
   switch (message.type) {
   case UndoRedoMessage::UndoOrRedo:
   case UndoRedoMessage::Reset:
   case UndoRedoMessage::Pushed:
   case UndoRedoMessage::Renamed:
      break;
   default:
      return;
   }
   ModifyUndoMenuItems( mProject );
   UpdateMenus();
}

CommandFlag MenuManager::GetUpdateFlags( bool checkActive ) const
{
   // This method determines all of the flags that determine whether
   // certain menu items and commands should be enabled or disabled,
   // and returns them in a bitfield.  Note that if none of the flags
   // have changed, it's not necessary to even check for updates.

   // static variable, used to remember flags for next time.
   static CommandFlag lastFlags;

   CommandFlag flags, quickFlags;

   const auto &options = ReservedCommandFlag::Options();
   size_t ii = 0;
   for ( const auto &predicate : ReservedCommandFlag::RegisteredPredicates() ) {
      if ( options[ii].quickTest ) {
         quickFlags[ii] = true;
         if( predicate( mProject ) )
            flags[ii] = true;
      }
      ++ii;
   }

   if ( checkActive && !GetProjectFrame( mProject ).IsActive() )
      // quick 'short-circuit' return.
      flags = (lastFlags & ~quickFlags) | flags;
   else {
      ii = 0;
      for ( const auto &predicate
           : ReservedCommandFlag::RegisteredPredicates() ) {
         if ( !options[ii].quickTest && predicate( mProject ) )
            flags[ii] = true;
         ++ii;
      }
   }

   lastFlags = flags;
   return flags;
}

// checkActive is a temporary hack that should be removed as soon as we
// get multiple effect preview working
void MenuManager::UpdateMenus( bool checkActive )
{
   auto &project = mProject;

   auto flags = GetUpdateFlags(checkActive);
   // Return from this function if nothing's changed since
   // the last time we were here.
   if (flags == mLastFlags)
      return;
   mLastFlags = flags;

   auto flags2 = flags;

   // We can enable some extra items if we have select-all-on-none.
   //EXPLAIN-ME: Why is this here rather than in GetUpdateFlags()?
   //ANSWER: Because flags2 is used in the menu enable/disable.
   //The effect still needs flags to determine whether it will need
   //to actually do the 'select all' to make the command valid.

   for ( const auto &enabler : RegisteredMenuItemEnabler::Enablers() ) {
      auto actual = enabler.actualFlags();
      if (
         enabler.applicable( project ) && (flags & actual) == actual
      )
         flags2 |= enabler.possibleFlags();
   }

   auto &commandManager = CommandManager::Get( project );

   // With select-all-on-none, some items that we don't want enabled may have
   // been enabled, since we changed the flags.  Here we manually disable them.
   // 0 is grey out, 1 is Autoselect, 2 is Give warnings.
   commandManager.EnableUsingFlags(
      flags2, // the "lax" flags
      (mWhatIfNoSelection == 0 ? flags2 : flags) // the "strict" flags
   );

   Publish({});
}

/// The following method moves to the previous track
/// selecting and unselecting depending if you are on the start of a
/// block or not.

void MenuCreator::RebuildAllMenuBars()
{
   for( auto p : AllProjects{} ) {
      MenuManager::Get(*p).RebuildMenuBar(*p);
#if defined(__WXGTK__)
      // Workaround for:
      //
      //   http://bugzilla.audacityteam.org/show_bug.cgi?id=458
      //
      // This workaround should be removed when Audacity updates to wxWidgets 3.x which has a fix.
      auto &window = GetProjectFrame( *p );
      wxRect r = window.GetRect();
      window.SetSize(wxSize(1,1));
      window.SetSize(r.GetSize());
#endif
   }
}

bool MenuManager::ReportIfActionNotAllowed(
   const TranslatableString & Name, CommandFlag & flags, CommandFlag flagsRqd )
{
   auto &project = mProject;
   bool bAllowed = TryToMakeActionAllowed( flags, flagsRqd );
   if( bAllowed )
      return true;
   auto &cm = CommandManager::Get( project );
   TellUserWhyDisallowed( Name, flags & flagsRqd, flagsRqd);
   return false;
}

/// Determines if flags for command are compatible with current state.
/// If not, then try some recovery action to make it so.
/// @return whether compatible or not after any actions taken.
bool MenuManager::TryToMakeActionAllowed(
   CommandFlag & flags, CommandFlag flagsRqd )
{
   auto &project = mProject;

   if( flags.none() )
      flags = GetUpdateFlags();

   // Visit the table of recovery actions
   auto &enablers = RegisteredMenuItemEnabler::Enablers();
   auto iter = enablers.begin(), end = enablers.end();
   while ((flags & flagsRqd) != flagsRqd && iter != end) {
      const auto &enabler = *iter;
      auto actual = enabler.actualFlags();
      auto MissingFlags = (~flags & flagsRqd);
      if (
         // Do we have the right precondition?
         (flags & actual) == actual
      &&
         // Can we get the condition we need?
         (MissingFlags & enabler.possibleFlags()).any()
      ) {
         // Then try the function
         enabler.tryEnable( project, flagsRqd );
         flags = GetUpdateFlags();
      }
      ++iter;
   }
   return (flags & flagsRqd) == flagsRqd;
}

void MenuManager::TellUserWhyDisallowed(
   const TranslatableString & Name, CommandFlag flagsGot, CommandFlag flagsRequired )
{
   // The default string for 'reason' is a catch all.  I hope it won't ever be seen
   // and that we will get something more specific.
   auto reason = XO("There was a problem with your last action. If you think\nthis is a bug, please tell us exactly where it occurred.");
   // The default title string is 'Disallowed'.
   auto untranslatedTitle = XO("Disallowed");
   wxString helpPage;

   bool enableDefaultMessage = true;
   bool defaultMessage = true;

   auto doOption = [&](const CommandFlagOptions &options) {
      if ( options.message ) {
         reason = options.message( Name );
         defaultMessage = false;
         if ( !options.title.empty() )
            untranslatedTitle = options.title;
         helpPage = options.helpPage;
         return true;
      }
      else {
         enableDefaultMessage =
            enableDefaultMessage && options.enableDefaultMessage;
         return false;
      }
   };

   const auto &alloptions = ReservedCommandFlag::Options();
   auto missingFlags = flagsRequired & ~flagsGot;

   // Find greatest priority
   unsigned priority = 0;
   for ( const auto &options : alloptions )
      priority = std::max( priority, options.priority );

   // Visit all unsatisfied conditions' options, by descending priority,
   // stopping when we find a message
   ++priority;
   while( priority-- ) {
      size_t ii = 0;
      for ( const auto &options : alloptions ) {
         if (
            priority == options.priority
         &&
            missingFlags[ii]
         &&
            doOption( options ) )
            goto done;

         ++ii;
      }
   }
   done:

   if (
      // didn't find a message
      defaultMessage
   &&
      // did find a condition that suppresses the default message
      !enableDefaultMessage
   )
      return;

   // Does not have the warning icon...
   BasicUI::ShowErrorDialog( {},
      untranslatedTitle,
      reason,
      helpPage);
}
