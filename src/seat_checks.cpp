#include "seat_checks.hpp"

#include <fmt/core.h>

#include <stdexcept>

namespace {

void require_unique_id(const char* role, std::uintptr_t address,
                       std::uint32_t expected, std::uint32_t actual) {
  if (expected != actual) {
    throw std::runtime_error(
        fmt::format("{} {:#x} was recycled: expected unique_id {}, found {}",
                    role, address, expected, actual));
  }
}

}  // namespace

void ra2yrcpp::seat_checks::check_unit_order_unique_ids(
    const ra2yrproto::commands::UnitOrder& order, bool check_target,
    const unique_id_lookup_t& live_unique_id) {
  const auto& ids = order.object_unique_ids();
  const auto& addresses = order.object_addresses();
  if (!ids.empty()) {
    if (ids.size() != addresses.size()) {
      throw std::runtime_error(
          fmt::format("order has {} object_unique_ids for {} object_addresses",
                      ids.size(), addresses.size()));
    }
    for (int i = 0; i < addresses.size(); i++) {
      const auto address = static_cast<std::uintptr_t>(addresses.Get(i));
      require_unique_id("order source", address, ids.Get(i),
                        live_unique_id(address));
    }
  }

  if (check_target && order.target_unique_id() != 0U) {
    const auto target = static_cast<std::uintptr_t>(order.target_object());
    if (target == 0U) {
      throw std::runtime_error(
          fmt::format("order target_unique_id {} set without target_object",
                      order.target_unique_id()));
    }
    require_unique_id("order target", target, order.target_unique_id(),
                      live_unique_id(target));
  }
}

void ra2yrcpp::seat_checks::check_unique_id(const char* role,
                                            std::uintptr_t address,
                                            std::uint32_t expected,
                                            std::uint32_t actual) {
  if (expected != 0U) {
    require_unique_id(role, address, expected, actual);
  }
}
