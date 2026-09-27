#include "config.hpp"

#include "ra2yrproto/commands_yr.pb.h"

#include "constants.hpp"
#include "protocol/helpers.hpp"
#include "util_string.hpp"

#include <fmt/core.h>
#include <google/protobuf/struct.pb.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ra2yrcpp::config;

namespace gpb = google::protobuf;

// Keys accepted for the command allowlist. The camelCase form matches the
// JSON names of the other settings; the snake_case form matches the field
// naming that the protobuf JSON parser also accepts for them.
static constexpr const char* ALLOWED_COMMANDS_KEYS[] = {"allowedCommands",
                                                        "allowed_commands"};

// Remove the allowlist from the JSON document, if present, and return it. The
// key is not a field of ra2yrproto::commands::Configuration, whose JSON parser
// rejects unknown keys, so it is extracted here before that parse.
static std::optional<std::vector<std::string>> take_allowed_commands(
    std::string* json) {
  gpb::Struct root;
  if (!ra2yrcpp::protocol::from_json(*json, &root)) {
    throw std::runtime_error("Failed to parse configuration");
  }
  auto* fields = root.mutable_fields();
  std::optional<std::vector<std::string>> res;
  for (const auto* key : ALLOWED_COMMANDS_KEYS) {
    auto it = fields->find(key);
    if (it == fields->end()) {
      continue;
    }
    if (res.has_value()) {
      throw std::runtime_error(
          "Configuration: allowedCommands specified more than once");
    }
    if (it->second.kind_case() != gpb::Value::kListValue) {
      throw std::runtime_error(
          fmt::format("Configuration: {} must be a list of strings", key));
    }
    std::vector<std::string> names;
    for (const auto& v : it->second.list_value().values()) {
      if (v.kind_case() != gpb::Value::kStringValue ||
          v.string_value().empty()) {
        throw std::runtime_error(fmt::format(
            "Configuration: {} entries must be non-empty strings", key));
      }
      names.push_back(v.string_value());
    }
    res = std::move(names);
    fields->erase(it);
  }
  if (res.has_value()) {
    *json = ra2yrcpp::protocol::to_json(root);
  }
  return res;
}

ConfigData ConfigData::parse(std::string json) {
  auto allowed_commands = take_allowed_commands(&json);
  ra2yrproto::commands::Configuration C;
  if (!ra2yrcpp::protocol::from_json(ra2yrcpp::to_bytes(json), &C)) {
    throw std::runtime_error("Failed to parse configuration");
  }

  ConfigData defaults{};

  ConfigData CC{C.debug_log(),        C.record_filename(),
                C.traffic_filename(), C.parse_map_data_interval(),
                C.single_step(),      C.port(),
                C.max_connections(),  C.allowed_hosts_regex(),
                C.log_filename(),     allowed_commands};

  if (CC.max_connections == 0) {
    CC.max_connections = defaults.max_connections;
  }
  if (CC.port == 0) {
    CC.port = defaults.port;
  }
  if (CC.allowed_hosts_regex.empty()) {
    CC.allowed_hosts_regex = defaults.allowed_hosts_regex;
  }
  if (CC.parse_map_data_interval == 0) {
    CC.parse_map_data_interval = defaults.parse_map_data_interval;
  }

  return CC;
}

Config::Config(ConfigData c) : c_(c) {}

Config::Config(std::string json) : Config(ConfigData::parse(json)) {}

const ConfigData& Config::c() const { return c_; }

void Config::set_debug_log(bool value) { c_.debug_log = value; }

void Config::set_allowed_hosts_regex(std::string pattern) {
  c_.allowed_hosts_regex = pattern;
}

void Config::set_max_connections(unsigned value) {
  if (value == 0 || value > cfg::MAX_CLIENTS) {
    throw std::invalid_argument(fmt::format(
        "got {}, expecting 0 < max_connections < {}", value, cfg::MAX_CLIENTS));
  }

  c_.max_connections = value;
}

void Config::set_parse_map_data_interval(unsigned value) {
  c_.parse_map_data_interval = value;
}

void Config::set_single_step(bool value) { c_.single_step = value; }

std::string Config::to_json() {
  ra2yrproto::commands::Configuration C;
#define X(k) C.set_##k(c_.k)
  X(debug_log);
  X(record_filename);
  X(traffic_filename);
  X(parse_map_data_interval);
  X(single_step);
  X(port);
  X(max_connections);
  X(allowed_hosts_regex);
  X(log_filename);
#undef X
  return protocol::to_json(C);
}
