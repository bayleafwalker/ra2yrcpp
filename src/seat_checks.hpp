#pragma once
#include "ra2yrproto/commands_game.pb.h"

#include <cstdint>

#include <functional>

namespace ra2yrcpp {
namespace seat_checks {

/// Return the live AbstractClass::UniqueID of the object at an address.
using unique_id_lookup_t =
    std::function<std::uint32_t(std::uintptr_t)>;  // NOLINT

/// Compare the stable IDs carried by a UnitOrder with the live objects.
///
/// Object addresses are reused after an object is freed, so an address the
/// client saw earlier may now belong to a newer object. If object_unique_ids
/// is non-empty it must have one entry per object_addresses entry, and each
/// live object's UniqueID must equal its entry. If check_target is true and
/// target_unique_id is non-zero, the live target's UniqueID must equal it.
/// Orders without IDs pass unchanged.
///
/// Throws std::runtime_error on the first mismatch. Call it after every source
/// and the target are known to be live and before anything is issued, so a
/// mismatch rejects the whole order.
void check_unit_order_unique_ids(const ra2yrproto::commands::UnitOrder& order,
                                 bool check_target,
                                 const unique_id_lookup_t& live_unique_id);

/// Compare the UniqueID the client sent for an object (0 = not checked) with
/// the live object's UniqueID. role names the object in the error message.
void check_unique_id(const char* role, std::uintptr_t address,
                     std::uint32_t expected, std::uint32_t actual);

}  // namespace seat_checks
}  // namespace ra2yrcpp
