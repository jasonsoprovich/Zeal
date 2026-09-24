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
// Auto-capture on camp sends five server commands about this far apart, and
// won't fire again within this many ms of the last time it fired.
constexpr DWORD kAutoCaptureStepMs = 300;
constexpr DWORD kAutoCaptureMinIntervalMs = 60000;

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
}  // namespace

PopFlagsExport::PopFlagsExport(ZealService *zeal) {
  zeal->chat_hook->add_print_chat_callback(
      [this](const char *data, int color_index) { HandlePrintChat(data, color_index); });
  zeal->callbacks->AddGeneric([this]() { HandleMainLoop(); }, callback_type::MainLoop);
  zeal->callbacks->AddGeneric([this]() { HandleCharacterSelect(); }, callback_type::CharacterSelect);
  zeal->callbacks->AddGeneric([this]() { HandleEnterZone(); }, callback_type::EnterZone);
}

void PopFlagsExport::HandlePrintChat(const char *data, int color_index) {
  if (!data || !data[0]) return;
  std::string line(data);

  if (IsSectionHeader(line)) {
    EnsureCharacterContext();
    if (captured_character.empty()) return;  // Not in game; ignore defensively.
    if (!active_header.empty()) FinishActiveSection();  // Back-to-back sections with no idle gap.
    active_header = line;
    active_lines.clear();
    active_last_line_tick = GetTickCount64();
    return;
  }

  if (active_header.empty()) return;  // Not currently inside a captured block.

  active_lines.push_back(line);
  active_last_line_tick = GetTickCount64();
  if (active_lines.size() >= kSectionLineCap) FinishActiveSection();
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
  active_header.clear();
  active_lines.clear();
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
  // running one section per session (rather than all five plus overview in
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

void PopFlagsExport::SendAutoCaptureCommands() {
  if (!Zeal::Game::is_in_game()) return;
  ZealService *zeal = ZealService::get_instance();
  for (int tier = 1; tier <= 5; tier++) {
    zeal->callbacks->AddDelayed([tier]() { Zeal::Game::do_say(true, "#popflags " + std::to_string(tier)); },
                                (tier - 1) * kAutoCaptureStepMs);
  }
  // Give the last reply time to arrive and the idle timeout time to close
  // that section before writing.
  zeal->callbacks->AddDelayed([this]() { WriteFile(); }, 5 * kAutoCaptureStepMs + kSectionIdleTimeoutMs + 500);
}

void PopFlagsExport::OnCamp() {
  EnsureCharacterContext();
  if (ZealService::get_instance()->outputfile->setting_export_on_camp.get()) WriteFile();

  if (setting_auto_on_camp.get()) {
    ULONGLONG now = GetTickCount64();
    if (now - last_auto_capture_tick > kAutoCaptureMinIntervalMs) {
      last_auto_capture_tick = now;
      SendAutoCaptureCommands();
    }
  }
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
    Zeal::Game::print_chat("PopFlags auto-capture on camp: %s", setting_auto_on_camp.get() ? "on" : "off");
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
