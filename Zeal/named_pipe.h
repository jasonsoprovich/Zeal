#pragma once
#include <Windows.h>

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game_packets.h"
#include "json.hpp"
#include "vectors.h"
#include "zeal_settings.h"

namespace Zeal::GameStructures {
struct Entity;
}

enum struct pipe_data_type { log, label, gauge, player, custom, raid, group, death, item, consider, spell };

struct pipe_data {
  pipe_data_type type;
  std::string data;
  std::string character;
  pipe_data(pipe_data_type _type, std::string _data);
  pipe_data() : type(pipe_data_type::custom), data{""}, character{""} {};

  nlohmann::json serialize() const {
    return nlohmann::json{{"type", type}, {"data_len", data.length()}, {"data", data}, {"character", character}};
  }

  void deserialize(nlohmann::json json_obj) {
    type = json_obj["type"];
    data = json_obj["data"];
  }
};

class NamedPipe {
 public:
  NamedPipe(class ZealService *zeal);
  ~NamedPipe();
  void chat_msg(const char *data, int color_index);
  void write(std::string data, pipe_data_type data_type);
  void write(std::string data);
  void write(const char *format, ...);
  void main_loop();
  void update_delay(unsigned new_delay);

  bool is_connected() const { return pipe_handles.size() != 0; }

 private:
  // How long an auto consider's reply is still expected (and kept out of chat). Generous so a lagged
  // reply stays quiet; zoning drops any that are still pending.
  static constexpr ULONGLONG kAutoConsiderTimeoutMs = 30000;

  void write_item(int item_id, const std::string &name);
  void write_death(const Zeal::Packets::Death_Struct &death);
  void write_consider(const Zeal::Packets::Consider_Struct &consider);
  void auto_consider();
  bool handle_consider(const Zeal::Packets::Consider_Struct &consider);
  void write_spell_action(const Zeal::Packets::Action_Struct &action);
  void update_pipe_spells(const std::string &mode);
  void add_new_pipe_handle(const HANDLE &handle);
  void record_spawn_locs();
  void update_pipe_handles();
  ZealSetting<int> pipe_delay = {100, "Zeal", "PipeDelay", false};
  ZealSetting<bool> pipe_verbose = {false, "Zeal", "PipeVerbose", false};
  ZealSetting<bool> pipe_consider = {false, "Zeal", "PipeConsider", false};
  int last_target_id = 0;                              // Spawn id of the target at the last auto_consider() check.
  // Spawn id -> request time of the consider auto_consider() sent and hasn't seen a reply to (one per spawn).
  std::unordered_map<int, ULONGLONG> auto_considers;
  ZealSetting<int> pipe_spells = {0, "Zeal", "PipeSpells", false};  // 0 off, 1 detrimental only, 2 all.
  // Where each NPC was when the client created it (on zoning in, or when it spawned), by spawn id. For a
  // mob that hasn't moved yet that is its spawn point, which a consumer can use to tell same-name spawns
  // apart after they've been pulled away. Entities created since the last frame wait in the pending set,
  // since their position is filled in after the entity is constructed. Each record keeps the entity it was
  // taken from and is only used while that is still the entity with that spawn id (ids get reused).
  struct SpawnLoc {
    const Zeal::GameStructures::Entity *entity;
    Vec3 position;
  };
  std::unordered_set<Zeal::GameStructures::Entity *> spawn_loc_pending;
  std::unordered_map<int, SpawnLoc> spawn_locs;
  bool end_thread = false;
  std::string name = "\\\\.\\pipe\\zeal_";
  std::vector<HANDLE> pipe_handles;
  std::vector<HANDLE> pipe_handle_queue;  // Mutex protected transfer queue.
  std::thread pipe_thread;
  std::mutex pipe_handle_mutex;
};
