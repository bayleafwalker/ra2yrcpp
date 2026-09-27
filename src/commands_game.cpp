#include "commands_game.hpp"

#include "ra2yrproto/commands_game.pb.h"
#include "ra2yrproto/ra2yr.pb.h"

#include "command/is_command.hpp"
#include "constants.hpp"
#include "hooks_yr.hpp"
#include "logging.hpp"
#include "ra2/abi.hpp"
#include "ra2/common.hpp"
#include "ra2/state_context.hpp"
#include "ra2/yrpp_export.hpp"
#include "types.h"

#include <fmt/core.h>

#include <cstdint>

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ra2yrcpp::commands_game;

using ra2yrcpp::command::get_async_cmd;
using ra2yrcpp::command::get_cmd;
using ra2yrcpp::hooks_yr::get_gameloop_command;

namespace r2p = ra2yrproto::ra2yr;

namespace {

// Live seat checks.
//
// Everything below reads game memory directly and must run in the game thread
// (i.e. inside a get_gameloop_command callback). The parsed state snapshot
// (StateContext::s_) is only refreshed at the beginning of each frame and can
// lag behind the game, e.g. when Yuri's mind control transfers a unit. Commands
// that act on behalf of the local house therefore re-validate against the live
// objects immediately before they issue anything.

/// Return the house this client controls, or throw if there is none (e.g. the
/// game has not started) or it is the observer house.
HouseClass* live_local_house() {
  HouseClass* house = HouseClass::CurrentPlayer;
  if (house == nullptr) {
    throw std::runtime_error("order requires a local player");
  }
  if (HouseClass::IsCurrentPlayerObserver()) {
    throw std::runtime_error("order rejected: local player is an observer");
  }
  return house;
}

/// Resolve a caller supplied address to a live TechnoClass. The address is
/// never dereferenced unless it is present in TechnoClass::Array, so a forged
/// or stale address cannot make us read freed memory.
TechnoClass* find_live_techno(std::uintptr_t address) {
  auto* A = TechnoClass::Array.get();
  for (int i = 0; i < A->Count; i++) {
    if (reinterpret_cast<std::uintptr_t>(A->Items[i]) == address) {
      return A->Items[i];
    }
  }
  return nullptr;
}

/// Return true if address is a live game object (any owner). Used for order
/// targets, which the game dereferences, so a forged or dangling target must
/// be rejected before it reaches the game.
bool is_live_abstract(std::uintptr_t address) {
  if (find_live_techno(address) != nullptr) {
    return true;
  }
  auto* A = AbstractClass::Array.get();
  for (int i = 0; i < A->Count; i++) {
    if (reinterpret_cast<std::uintptr_t>(A->Items[i]) == address) {
      return true;
    }
  }
  return false;
}

/// Return the live object at address if it exists, is alive, is not in limbo
/// and is owned by house. Throw otherwise.
TechnoClass* live_owned_techno(std::uintptr_t address,
                               const HouseClass* house) {
  auto* T = find_live_techno(address);
  if (T == nullptr) {
    throw std::runtime_error(
        fmt::format("order source {:#x} does not exist", address));
  }
  if (!T->IsAlive || T->InLimbo) {
    throw std::runtime_error(
        fmt::format("order source {:#x} is dead or in limbo", address));
  }
  if (T->Owner != house) {
    throw std::runtime_error(fmt::format(
        "order source {:#x} is not owned by the local player", address));
  }
  return T;
}

}  // namespace

struct UnitOrderCtx {
  UnitOrderCtx(ra2::StateContext* ctx, const ra2yrproto::commands::UnitOrder* o)
      : ctx_(ctx), o_(o) {}

  const ra2yrproto::commands::UnitOrder& uo() { return *o_; }

  std::uintptr_t p_obj() const { return src_; }

  void click_event(ra2yrproto::ra2yr::NetworkEvent e) {
    ctx_->abi_->ClickEvent(p_obj(), e);
  }

  bool requires_target_object() {
    switch (uo().action()) {
      case r2p::UnitAction::UNIT_ACTION_STOP:
        return false;
      default:
        return true;
    }
    return true;
  }

  bool requires_target_cell() {
    switch (uo().action()) {
      case r2p::UnitAction::UNIT_ACTION_STOP:
        return false;
      case r2p::UnitAction::UNIT_ACTION_CAPTURE:
        return false;
      case r2p::UnitAction::UNIT_ACTION_REPAIR:
        return false;
      default:
        return true;
    }
    return true;
  }

  // Return true if source object's current mission is invalid for execution of
  // the UnitAction.
  static bool is_illegal_mission(Mission m) {
    return (m == Mission::None || m == Mission::Construction);
  }

  bool click_mission(ra2yrproto::ra2yr::Mission m) {
    CellClass* cell = nullptr;
    std::uintptr_t p_target = 0U;
    if (requires_target_object()) {
      p_target = uo().target_object();
    }
    // TODO(shmocz): figure out events where the cell should be null
    if (requires_target_cell()) {
      auto c = uo().coordinates();
      if (p_target != 0U && !uo().has_coordinates()) {
        c = ctx_->get_object_entry(p_target).o->coordinates();
      }
      if ((cell = ra2::get_map_cell(c)) == nullptr) {
        throw std::runtime_error("invalid cell");
      }
    }
    return ra2::abi::ClickMission::call(
        ctx_->abi_, p_obj(), static_cast<Mission>(m), p_target, cell, nullptr);
  }

  // Apply action to single object
  void unit_action() {
    using r2p::UnitAction;
    switch (uo().action()) {
      case UnitAction::UNIT_ACTION_DEPLOY:
        click_event(r2p::NETWORK_EVENT_Deploy);
        break;
      case UnitAction::UNIT_ACTION_SELL:
        click_event(r2p::NETWORK_EVENT_Sell);
        break;
      case UnitAction::UNIT_ACTION_SELL_CELL: {
        ra2yrproto::ra2yr::Event E;
        E.set_event_type(ra2yrproto::ra2yr::NETWORK_EVENT_SellCell);
        E.mutable_sell_cell()->mutable_cell()->CopyFrom(uo().coordinates());
        (void)ctx_->add_event(E);
      } break;
      case UnitAction::UNIT_ACTION_SELECT:
        (void)ctx_->abi_->SelectObject(p_obj());
        break;
      case UnitAction::UNIT_ACTION_MOVE:
        (void)click_mission(r2p::Mission_Move);
        break;
      case UnitAction::UNIT_ACTION_CAPTURE:
        (void)click_mission(r2p::Mission_Capture);
        break;
      case UnitAction::UNIT_ACTION_ATTACK:
        (void)click_mission(r2p::Mission_Attack);
        break;
      case UnitAction::UNIT_ACTION_ATTACK_MOVE:
        (void)click_mission(r2p::Mission_AttackMove);
        break;
      case UnitAction::UNIT_ACTION_REPAIR:
        (void)click_mission(r2p::Mission_Capture);
        break;
      case UnitAction::UNIT_ACTION_STOP:
        (void)click_mission(r2p::Mission_Stop);
        break;

      default:
        throw std::runtime_error("invalid unit action");
        break;
    }
  }

  // SELL_CELL sells whatever building occupies the cell, so the building
  // itself is the source object that must belong to the local player.
  void check_sell_cell(const HouseClass* house) {
    auto* cell = ra2::get_map_cell(uo().coordinates());
    if (cell == nullptr) {
      throw std::runtime_error("invalid cell");
    }
    auto* B = cell->GetBuilding();
    if (B == nullptr) {
      throw std::runtime_error("no building at cell");
    }
    (void)live_owned_techno(reinterpret_cast<std::uintptr_t>(B), house);
  }

  // Validate every source against live game memory, then issue the order for
  // each of them. Validation and issuing happen in the same game-thread
  // callback, so nothing can change ownership in between, and a list with any
  // bad source is rejected before any part of the order executes.
  void perform() {
    const auto* house = live_local_house();

    if (uo().action() == r2p::UnitAction::UNIT_ACTION_SELL_CELL) {
      check_sell_cell(house);
      unit_action();
      return;
    }

    if (requires_target_object() && uo().target_object() != 0U &&
        !is_live_abstract(uo().target_object())) {
      throw std::runtime_error(fmt::format("order target {:#x} does not exist",
                                           uo().target_object()));
    }

    std::vector<std::uintptr_t> sources;
    sources.reserve(uo().object_addresses_size());
    for (const auto k : uo().object_addresses()) {
      const auto* T = live_owned_techno(k, house);
      if (is_illegal_mission(T->CurrentMission)) {
        throw std::runtime_error(
            fmt::format("order source {:#x} has illegal mission: {}", k,
                        static_cast<int>(T->CurrentMission)));
      }
      sources.push_back(k);
    }
    for (const auto k : sources) {
      src_ = k;
      unit_action();
    }
  }

  ra2::StateContext* ctx_;
  const ra2yrproto::commands::UnitOrder* o_;
  std::uintptr_t src_{0U};
};

// TODO(shmocz): copy args automagically in async cmds
auto unit_order() {
  return get_cmd<ra2yrproto::commands::UnitOrder>([](auto* Q) {
    auto args = Q->command_data();

    get_gameloop_command(Q, [args](auto* cb) {
      auto* S = cb->get_state_context();
      UnitOrderCtx ctx(S, &args);
      ctx.perform();
    });
  });
}

auto produce_order() {
  return get_async_cmd<ra2yrproto::commands::ProduceOrder>([](auto* Q) {
    auto args = Q->command_data();

    get_gameloop_command(Q, [args](auto* cb) {
      auto* ctx = cb->get_state_context();
      auto* house = live_local_house();
      const auto* tc = ctx->get_type_class(args.object_type().pointer_self());
      auto can_build = ra2::abi::HouseClass_CanBuild::call(
          cb->abi(), house,
          reinterpret_cast<TechnoTypeClass*>(tc->pointer_self()), false, false);
      if (can_build != CanBuildResult::Buildable) {
        throw std::runtime_error(
            fmt::format("unbuildable {}", args.ShortDebugString()));
      }
      dprintf("can build={}", static_cast<int>(can_build));

      // Build the event from the type class that passed CanBuild, not from the
      // caller supplied heap id and RTTI, which could name a different type.
      ra2yrproto::ra2yr::Event E;
      E.set_event_type(ra2yrproto::ra2yr::NETWORK_EVENT_Produce);
      auto* P = E.mutable_production();
      P->set_heap_id(tc->array_index());
      P->set_rtti_id(static_cast<i32>(tc->type()));
      (void)ctx->add_event(E);
    });
  });
}

auto place_building() {
  return get_async_cmd<ra2yrproto::commands::PlaceBuilding>([](auto* Q) {
    auto args = Q->command_data();

    get_gameloop_command(Q, [args](auto* cb) {
      auto* ctx = cb->get_state_context();
      auto* house = live_local_house();
      const auto address =
          static_cast<std::uintptr_t>(args.building().pointer_self());

      // Require a live factory of the local house that has finished building
      // exactly this object.
      const FactoryClass* factory = nullptr;
      auto* FA = FactoryClass::Array.get();
      for (int i = 0; i < FA->Count; i++) {
        auto* F = FA->Items[i];
        if (F->Owner == house &&
            reinterpret_cast<std::uintptr_t>(F->Object) == address &&
            F->Production.Value == cfg::PRODUCTION_STEPS) {
          factory = F;
          break;
        }
      }
      if (factory == nullptr) {
        throw std::runtime_error(fmt::format(
            "completed object {:#x} not found from any factory of the local "
            "player",
            address));
      }

      // The type class of an object never changes, so the snapshot is fine here.
      const ra2::ObjectEntry OE = ctx->get_object_entry(address);
      const auto* H = ctx->get_house(reinterpret_cast<std::uintptr_t>(house));
      if (H == nullptr) {
        throw std::runtime_error("local player missing from state");
      }
      ctx->place_building(*H, *OE.tc, args.coordinates());
    });
  });
}

std::map<std::string, ra2yrcpp::command::iservice_cmd::handler_t>
ra2yrcpp::commands_game::get_commands() {
  return {
      unit_order(),     //
      produce_order(),  //
      place_building()  //
  };
}
