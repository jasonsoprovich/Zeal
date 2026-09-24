#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "zeal_settings.h"

// Captures '#popflags' output (a player-facing command added by EQMacEmu PR
// #382) from chat and exports it to file, the same way OutputFile exports
// Inventory/Quarmy/Spellbook. Zeal has no way to read Plane of Power
// progression qglobals directly - this only records the text the server
// already prints to a player who runs the command themselves.
//
// The capture is purely textual: only the six '=== ... ===' section header
// strings are matched, and everything between a header and the end of that
// section is stored and exported verbatim. A wording change to the rest of
// the server's report text never requires a Zeal update.
class PopFlagsExport {
 public:
  PopFlagsExport(class ZealService *zeal);
  ~PopFlagsExport(){};

  // Writes every captured section (this session's captures merged onto
  // whatever sections are already in the existing file, so running one
  // section per session still builds up a complete file over time) to
  // <CharName>-PopFlags[hosttag].txt, or optional_name.txt when given. A
  // no-op if nothing has been captured yet for the active character.
  void WriteFile(const std::string &optional_name = "");

  // Called from OutputFile's GameCamp hook. Writes the file when the
  // existing ExportOnCamp setting is on, and kicks off an automatic
  // '#popflags 1'..'5' capture when setting_auto_on_camp is on.
  void OnCamp();

  // Handles the "popflags" sub-command of /outputfile ("/outputfile popflags
  // [filename]" or "/outputfile popflags auto [on|off]"). args[0] is the
  // command itself, args[1] is "popflags". Returns false for an args shape
  // it doesn't recognize, so the caller can fall through to its own usage
  // message; true otherwise (including "handled, printed an error").
  bool HandleCommand(std::vector<std::string> &args);

  // Off by default: sending five server commands and printing ~80 lines of
  // chat on every camp is not something to do silently. See README.
  ZealSetting<bool> setting_auto_on_camp = {false, "Zeal", "PopFlagsOnCamp", false};

 private:
  struct Section {
    int64_t captured_at = 0;  // Unix epoch seconds.
    std::string header;
    std::vector<std::string> lines;
  };

  void HandlePrintChat(const char *data, int color_index);
  void HandleMainLoop();
  void HandleCharacterSelect();
  void HandleEnterZone();
  void FinishActiveSection();     // Closes out the in-progress capture, if any.
  void EnsureCharacterContext();  // Resets all captured state on a character change.
  void SendAutoCaptureCommands();
  static std::map<std::string, Section> ReadExistingFile(const std::string &filename);

  std::map<std::string, Section> sections;  // Section header -> most recent capture this session.
  std::string active_header;                // Empty when no capture is in progress.
  std::vector<std::string> active_lines;
  unsigned long long active_last_line_tick = 0;  // GetTickCount64() of the last captured line.

  std::string captured_character;  // Character the current 'sections' belong to.

  unsigned long long last_auto_capture_tick = 0;  // GetTickCount64() of the last auto-send, for the min interval.
};
