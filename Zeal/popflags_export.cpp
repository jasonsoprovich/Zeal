#include "popflags_export.h"

#include <Windows.h>

#include <ctime>
#include <fstream>
#include <sstream>

#include "callbacks.h"
#include "chat.h"
#include "game_functions.h"
#include "game_structures.h"
#include "outputfile.h"
#include "string_util.h"
#include "zeal.h"

namespace {
// A section ends after this long with no new captured line (the client
// prints a #popflags report as a fast burst, well under a second).
constexpr DWORD kSectionIdleTimeoutMs = 1000;
// Safety cap so a stuck capture (e.g. an unrelated header-like line) cannot
// grow forever. The largest real section is well under this.
constexpr size_t kSectionLineCap = 150;
// Auto-capture on camp sends six server commands about this far apart, and
// won't fire again within this many ms of the last time it fired.
constexpr DWORD kAutoCaptureStepMs = 300;
constexpr DWORD kAutoCaptureMinIntervalMs = 60000;
constexpr int kAutoCaptureCommands = 6;
// How long after the last auto-capture command report lines are still hidden.
// A reply arriving later than this simply shows up in chat as usual.
constexpr DWORD kAutoCaptureHideMs = 5000;
// The file is written as soon as all six sections are in. This is the fallback
// for a reply that never arrives, counted from the last command.
constexpr DWORD kAutoCaptureWriteFallbackMs = 8000;
// The client's host tag for Project Quarm (see Zeal::Game::get_host_tag()).
constexpr char kQuarmHostTag[] = "_pq.proj";

// Copied verbatim from EQMacEmu zone/gm_commands/popflags.cpp. This is the
// only place the server's wording is assumed - the rest of a report's text
// is stored and exported as opaque lines.
const std::vector<std::string> kSectionHeaders = {
    "=== Planes of Power Progression ===", "=== Tier 1 Progression ===", "=== Tier 2 Progression ===",
    "=== Tier 3 Progression ===",          "=== Tier 4 Progression ===", "=== Plane of Time ===",
};

bool IsSectionHeader(const std::string &line) {
  for (const auto &header : kSectionHeaders)
    if (line == header) return true;
  return false;
}

// Player chat (a tell landing in the middle of a report, for example) is never
// part of a section, so it is never captured or hidden.
bool IsPlayerChat(int color_index) {
  return (color_index >= USERCOLOR_SAY && color_index <= USERCOLOR_EMOTE) || color_index == USERCOLOR_CHAT_CHANNEL ||
         color_index == USERCOLOR_RAID_SAY;
}
}  // namespace

PopFlagsExport::PopFlagsExport(ZealService *zeal) {
  // A filter rather than a plain print chat callback so that the replies to an auto-capture can be hidden.
  zeal->chat_hook->add_print_chat_filter(
      [this](const char *data, int color_index) { return HandlePrintChat(data, color_index); });
  zeal->callbacks->AddGeneric([this]() { HandleMainLoop(); }, callback_type::MainLoop);
  zeal->callbacks->AddGeneric([this]() { HandleCharacterSelect(); }, callback_type::CharacterSelect);
  zeal->callbacks->AddGeneric([this]() { HandleEnterZone(); }, callback_type::EnterZone);
}

bool PopFlagsExport::HandlePrintChat(const char *data, int color_index) {
  if (!data || !data[0]) return false;
  std::string line(data);
  // Only the replies to a capture Zeal sent itself are hidden. A '#popflags'
  // the player types prints as usual.
  bool hide = GetTickCount64() < auto_hide_until_tick;

  if (IsSectionHeader(line)) {
    EnsureCharacterContext();
    if (captured_character.empty()) return false;  // Not in game; ignore defensively.
    if (!active_header.empty()) FinishActiveSection();  // Back-to-back sections with no idle gap.
    active_header = line;
    active_lines.clear();
    active_last_line_tick = GetTickCount64();
    return hide;
  }

  if (active_header.empty()) return false;  // Not currently inside a captured block.
  if (IsPlayerChat(color_index)) return false;

  active_lines.push_back(line);
  active_last_line_tick = GetTickCount64();
  if (active_lines.size() >= kSectionLineCap) FinishActiveSection();
  return hide;
}

void PopFlagsExport::HandleMainLoop() {
  if (!active_header.empty() && (GetTickCount64() - active_last_line_tick > kSectionIdleTimeoutMs))
    FinishActiveSection();
}

void PopFlagsExport::HandleCharacterSelect() { FinishActiveSection(); }

void PopFlagsExport::HandleEnterZone() { FinishActiveSection(); }

void PopFlagsExport::FinishActiveSection() {
  if (active_header.empty()) return;
  Section section;
  section.captured_at = static_cast<int64_t>(time(nullptr));
  section.header = active_header;
  section.lines = active_lines;
  sections[active_header] = std::move(section);
  if (auto_capture_pending) auto_capture_headers.insert(active_header);
  active_header.clear();
  active_lines.clear();

  if (auto_capture_pending && auto_capture_headers.size() == kSectionHeaders.size()) {
    auto_capture_pending = false;
    auto_hide_until_tick = 0;
    WriteFile();
  }
}

void PopFlagsExport::EnsureCharacterContext() {
  std::string name;
  if (Zeal::Game::is_in_game() && Zeal::Game::get_self() && Zeal::Game::get_self()->CharInfo)
    name = Zeal::Game::get_self()->CharInfo->Name;
  if (name.empty()) return;  // Not in game; leave any existing state untouched.
  if (name != captured_character) {
    // Character switched (or this is the first capture ever): drop anything
    // captured so far so one character's flags can never end up in another
    // character's export file.
    sections.clear();
    active_header.clear();
    active_lines.clear();
    captured_character = name;
  }
}

std::map<std::string, PopFlagsExport::Section> PopFlagsExport::ReadExistingFile(const std::string &filename) {
  std::map<std::string, Section> result;
  std::ifstream file(filename);
  if (!file.is_open()) return result;

  Section *current = nullptr;
  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("Section\t", 0) == 0) {
      size_t first_tab = line.find('\t');
      size_t second_tab = line.find('\t', first_tab + 1);
      if (second_tab == std::string::npos) continue;  // Malformed line; skip.
      Section section;
      try {
        section.captured_at = std::stoll(line.substr(first_tab + 1, second_tab - first_tab - 1));
      } catch (const std::exception &) {
        section.captured_at = 0;
      }
      section.header = line.substr(second_tab + 1);
      result[section.header] = section;
      current = &result[section.header];
    } else if (current && line.rfind("Line\t", 0) == 0) {
      current->lines.push_back(line.substr(5));
    }
    // The "Format" header line (and anything else unrecognized) is skipped.
  }
  return result;
}

void PopFlagsExport::WriteFile(const std::string &optional_name) {
  EnsureCharacterContext();
  FinishActiveSection();
  if (sections.empty() || captured_character.empty()) return;  // Nothing captured; don't touch the file.

  std::string filename = optional_name;
  if (filename.empty()) {
    filename = captured_character + "-PopFlags";
    bool new_format = (ZealService::get_instance()->outputfile->setting_export_format.get() != 0);
    if (new_format) filename += Zeal::Game::get_host_tag();
  }
  filename += ".txt";

  // Merge this session's captures onto whatever is already on disk, so
  // running one section per session (rather than the overview plus all five tiers in
  // one sitting) still builds up a complete file over multiple sessions.
  std::map<std::string, Section> merged = ReadExistingFile(filename);
  for (auto &[header, section] : sections) merged[header] = section;

  std::ostringstream oss;
  oss << "Format\tPopFlags\t1\n";
  for (auto &[header, section] : merged) {
    oss << "Section\t" << section.captured_at << "\t" << section.header << "\n";
    for (auto &line : section.lines) oss << "Line\t" << line << "\n";
  }

  std::ofstream file(filename);
  file << oss.str();
}

bool PopFlagsExport::CanAutoCapture() const {
  // '#popflags' is a Project Quarm server command. Anywhere else it would only
  // get "Unknown command" replies.
  return setting_auto_on_camp.get() && Zeal::Game::get_host_tag() == kQuarmHostTag;
}

void PopFlagsExport::SendAutoCaptureCommands() {
  if (!Zeal::Game::is_in_game()) return;
  ZealService *zeal = ZealService::get_instance();
  ULONGLONG last_command_tick = GetTickCount64() + (kAutoCaptureCommands - 1) * kAutoCaptureStepMs;
  auto_hide_until_tick = last_command_tick + kAutoCaptureHideMs;
  auto_capture_pending = true;
  auto_capture_headers.clear();
  // Bare '#popflags' is the overview, then tiers 1-5 (5 = Plane of Time).
  // do_say(true, ...) keeps the commands themselves out of chat.
  for (int step = 0; step < kAutoCaptureCommands; step++) {
    std::string command = (step == 0) ? "#popflags" : "#popflags " + std::to_string(step);
    zeal->callbacks->AddDelayed([command]() { Zeal::Game::do_say(true, command); }, step * kAutoCaptureStepMs);
  }
  // Normally FinishActiveSection() writes the file once all six sections are
  // in. This writes whatever did arrive if one of the replies never does.
  zeal->callbacks->AddDelayed(
      [this]() {
        if (!auto_capture_pending) return;
        auto_capture_pending = false;
        auto_hide_until_tick = 0;
        WriteFile();
      },
      (kAutoCaptureCommands - 1) * kAutoCaptureStepMs + kAutoCaptureWriteFallbackMs);
}

void PopFlagsExport::OnCamp() {
  EnsureCharacterContext();
  if (!ZealService::get_instance()->outputfile->setting_export_on_camp.get()) return;

  if (CanAutoCapture()) {
    ULONGLONG now = GetTickCount64();
    if (now - last_auto_capture_tick > kAutoCaptureMinIntervalMs) {
      last_auto_capture_tick = now;
      SendAutoCaptureCommands();  // Writes the file once the replies are in.
      return;
    }
  }
  WriteFile();  // Whatever was captured this session; a no-op if nothing was.
}

bool PopFlagsExport::HandleCommand(std::vector<std::string> &args) {
  // args[0] is "/outputfile" (or an alias), args[1] is "popflags".
  if (args.size() >= 3 && Zeal::String::compare_insensitive(args[2], "auto")) {
    if (args.size() == 3) {
      setting_auto_on_camp.toggle();
    } else if (args.size() == 4 && Zeal::String::compare_insensitive(args[3], "on")) {
      setting_auto_on_camp.set(true);
    } else if (args.size() == 4 && Zeal::String::compare_insensitive(args[3], "off")) {
      setting_auto_on_camp.set(false);
    } else {
      Zeal::Game::print_chat("usage: /outputfile popflags auto [on | off]");
      return true;
    }
    Zeal::Game::print_chat("PopFlags auto-capture on camp: %s%s", setting_auto_on_camp.get() ? "on" : "off",
                           ZealService::get_instance()->outputfile->setting_export_on_camp.get()
                               ? ""
                               : " (needs Export on Camp, which is off)");
    return true;
  }

  if (args.size() > 3) return false;  // Not a popflags form this handles; let OutputFile print its usage.

  EnsureCharacterContext();
  if (captured_character.empty()) {
    Zeal::Game::print_chat("Not in game.");
    return true;
  }
  FinishActiveSection();
  if (sections.empty()) {
    Zeal::Game::print_chat("No #popflags output captured yet this session. Run #popflags first.");
    return true;
  }

  std::string optional_name = (args.size() == 3) ? args[2] : "";
  Zeal::Game::print_chat("Outputting popflags...");
  WriteFile(optional_name);
  return true;
}
