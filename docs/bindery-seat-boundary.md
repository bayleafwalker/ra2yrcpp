# Bindery seat boundary

This note describes what ra2yrcpp does to make sure a client that holds one
player's seat (the Bindery RA2 adapter, driving an AI agent) can only act for
that player. It covers what is enforced today, what is not, and what
would need a protocol change in
[ra2yrproto](https://github.com/shmocz/ra2yrproto).

Nothing here has been tested in a running game. The changes build with the
i686 MinGW-w64 toolchain (DLL and tests), and the allowlist and config parsing
are covered by native unit tests. The ownership checks run against game memory
and can only be checked in-game.

## Terms

- **Local house**: `HouseClass::CurrentPlayer`, the house of the player at this
  game client. Events that ra2yrcpp adds with `StateContext::add_event` (unless
  `spoof` is set) and orders issued through `TechnoClass::ClickedMission` /
  `ClickedEvent` are sent as this house.
- **Snapshot**: the protobuf `GameState` parsed by the `GameLoopBegin` hook at
  the start of each frame (`StateSave::state_to_protobuf`), and read by
  `StateContext`.
- **Game thread work**: commands that use `get_gameloop_command` queue a
  callback that `GameLoopBegin` runs right after it parses the snapshot, still
  inside the game thread.

## What the boundary guarantees

All checks below run in the game thread, in the same callback that issues the
order, so the game cannot change state between the check and the order.

### UnitOrder (`commands_game.cpp`, `UnitOrderCtx::perform`)

- The local house must exist and must not be the observer house
  (`HouseClass::IsCurrentPlayerObserver()`).
- Each source address is looked up in `TechnoClass::Array`. The address is
  not dereferenced unless it is in that array, so a forged or freed pointer is
  never touched. The object must be `IsAlive`, not `InLimbo`, and its live
  `Owner` must be the local house. This is read from the object in memory, not
  from the snapshot, so a unit taken by Yuri's mind control earlier in the frame
  is refused.
- Each source's live `CurrentMission` must not be `None` or `Construction`.
  This check used to run while the orders were being issued, so a later bad
  source could fail after earlier orders had already gone out. It now runs
  before any order.
- If the action uses a target object, the target must be a live game object
  (in `TechnoClass::Array` or `AbstractClass::Array`). It may belong to anyone.
- If the order carries `object_unique_ids`, it must have one entry per
  `object_addresses` entry, and each source's live `UniqueID` must equal its
  entry. If the action uses a target and `target_unique_id` is non-zero, the
  live target's `UniqueID` must equal it (and `target_object` must be set).
  A mismatch fails with `order source 0x... was recycled: expected unique_id
  N, found M` (or `order target ...`). See "Stable entity IDs".
- A single bad source rejects the whole order before any part of it runs.
  This keeps the all-or-nothing behaviour from `4599971`.
- `UNIT_ACTION_SELL_CELL`: the building at the target cell
  (`CellClass::GetBuilding`) must pass the same live check (alive, not in
  limbo, owned by the local house) before the `SellCell` event is added.
  Before this change it sold whatever was at the cell.

### ProduceOrder (`produce_order`)

- Requires a local house that is not the observer, instead of dereferencing
  `current_player()`, which could be null.
- Runs `HouseClass::CanBuild` for the live local house.
- The `Produce` event is now built from the type class that passed `CanBuild`.
  Before, the event used the heap id and RTTI supplied by the caller, while
  `CanBuild` checked `object_type.pointer_self`. A caller could pass a buildable
  pointer with the heap id of a type that is not buildable.

### PlaceBuilding (`place_building`)

- Requires a local house that is not the observer.
- The building must be the `Object` of a live `FactoryClass` whose `Owner` is
  the local house and whose production has finished
  (`Production.Value == PRODUCTION_STEPS`). Before, this was checked against the
  snapshot and the snapshot's `current_player`, which is the first house with
  `IsInPlayerControl` and could be null.
- If `building.unique_id` is non-zero, it must equal the live `UniqueID` of
  the factory's object (`completed object 0x... was recycled: ...`).
- The placement checks and the `Place` event use the local house.

### Command allowlist (`allowedCommands`)

An opt-in key in `ra2yrcpp.json`:

```json
{
  "port": 14521,
  "allowedHostsRegex": "127\\.0\\.0\\.1",
  "allowedCommands": [
    "GetGameState",
    "ReadValue",
    "UnitOrder",
    "ProduceOrder",
    "PlaceBuilding"
  ]
}
```

- **Key absent:** every registered command runs, as before. Existing
  configurations behave the same.
- **Key present:** only listed commands run, and an empty list allows none.
  Any other `CLIENT_COMMAND` is rejected in
  `InstrumentationService` `handle_cmd` before a command object is created or
  queued. The response has `code = ERROR` and a `TextResponse` like
  `command not permitted by allowedCommands: ra2yrproto.commands.AddEvent`.
- An entry is either a full protobuf type name
  (`ra2yrproto.commands.UnitOrder`) or a bare message name (`UnitOrder`).
  `allowed_commands` is also accepted as the key.
- A value that is not a list of non-empty strings makes the configuration fail
  to load, like any other malformed setting.
- The key is not a field of `ra2yrproto.commands.Configuration`, so
  `InspectConfiguration` neither reports nor changes it. It is read once at
  startup and applies to all connections.
- The active list is logged at startup (`allowedCommands active, N entries`).

Recommended Bindery allowlist:

| Command | Why |
| --- | --- |
| `GetGameState` | per-frame state for telemetry and the agent |
| `ReadValue` | `initial_game_state` (type classes, prerequisites) and map data (`map_data`, `map_data_soa`), all read-only |
| `UnitOrder` | unit orders, checked as above |
| `ProduceOrder` | production, checked as above |
| `PlaceBuilding` | building placement, checked as above |
| `PlaceQuery` (optional) | read-only placement validity query, if the agent needs it |

Commands that must stay out of an agent's allowlist:

- `AddEvent`: adds any network event. With `spoof = true` it uses a house index
  supplied by the caller, so it can act for any house.
- `ClickEvent`, `MissionClicked`, `UnitCommand`: raw per-object orders with no
  ownership check. They are left as they are because upstream tools use them as
  low-level primitives.
- `InspectConfiguration` with `update = true`: changes `single_step`, which can
  pause the game, and `parse_map_data_interval`.
- `StoreValue` / `GetValue`: the service's own key/value store. It is not game
  memory, but an agent does not need it.
- `GetSystemState`: lists connections and the game directory.
- `AddMessage`: in-game chat/message injection.

## What it does not guarantee

- **Recycled addresses, for clients that do not send IDs.** Object addresses
  are reused after an object is freed. If the agent holds address `P` for unit
  A, and A dies and a new unit B of the same house is allocated at `P`, an
  order for `P` without `object_unique_ids` passes every check and moves B. The
  IDs are optional; see "Stable entity IDs" below.
- **Event latency.** `ClickedMission`/`ClickedEvent`, `Produce`, `Place` and
  `SellCell` are network events. They execute several frames later
  (`MaxAhead`/`FrameSendRate`). ra2yrcpp checks ownership when it issues an
  event, not when the event executes. What the game does with an event for an
  object whose owner changed in between is the engine's own logic, and it has
  not been verified here.
- **Snapshot-derived data.** Target coordinates for a target object without
  explicit `coordinates`, and the building's type class in `PlaceBuilding`,
  still come from the snapshot. Neither decides ownership, and type classes do
  not change.
- **Connection identity.** The allowlist applies to the whole service. There
  is no per-connection policy or authentication beyond `allowedHostsRegex`,
  which filters by remote address when the TCP connection is set up.
- **Polling other queues.** `POLL_BLOCKING` takes a `queue_id` from the
  request, so a connection can read (and consume) another connection's command
  results. The allowlist does not cover `POLL`, `POLL_BLOCKING` or `SHUTDOWN`,
  which are command types, not commands. `SHUTDOWN` has no handler in the DLL.
- **ProduceOrder `action`.** The `action` field is ignored, as before.

## Observer clients

A spectator's client has a local house: `HouseClass::CurrentPlayer` is the
observer house (`HouseClass::Observer`). Before this change, `current_player()`
returned the snapshot's first house with `IsInPlayerControl`. For an observer
that is most likely the observer house, which owns no units. Every source would
then fail the ownership check, but `SELL_CELL` skipped the check. `ProduceOrder`
and `PlaceBuilding` relied on `CanBuild` and the factory owner.
`IsInPlayerControl` for the observer house was not confirmed from code alone,
and if no house had it these commands dereferenced a null pointer.

Now the answer comes straight from the code: `live_local_house()` rejects every
`UnitOrder` (including `SELL_CELL`), `ProduceOrder` and `PlaceBuilding` when
`HouseClass::IsCurrentPlayerObserver()` is true, with
`order rejected: local player is an observer`. An observer can still read state
with `GetGameState` / `ReadValue`, so a spectator client can serve telemetry,
but it cannot hold a seat.

## Stable entity IDs

### Problem

The protocol names objects by address (`Object.pointer_self`,
`UnitOrder.object_addresses`, `UnitOrder.target_object`,
`PlaceBuilding.building`). The game frees objects and reuses their memory, so
an address the agent stored some frames ago may now belong to a different
object. The live ownership check stops orders for foreign units, but not for a
newer unit of the same house at the same address.

### Implementation

The engine already has a stable ID. Every `AbstractClass` has a `UniqueID`
(`DWORD`, YRpp `AbstractClass.h`), assigned as
`++ScenarioClass::Instance->UniqueID` when the object is created. It rises
monotonically within a scenario and is never reused, which avoids keeping a
separate creation-frame counter.

ra2yrproto fields (bayleafwalker/ra2yrproto `feat/stable-ids`; the numbers
were free and not reserved at shmocz/ra2yrproto `0ad7245`):

```proto
// ra2yr.proto
message Object {
  // ...existing fields...
  uint32 unique_id = 21;  // AbstractClass::UniqueID
}

message Factory {
  // ...existing fields...
  uint32 object_unique_id = 7;  // UniqueID of Factory.object, 0 if none
}

// commands_game.proto
message UnitOrder {
  // ...existing fields...
  // If non-empty, must have the same length as object_addresses; element i
  // is the Object.unique_id the client saw at object_addresses[i].
  repeated uint32 object_unique_ids = 5;
  uint32 target_unique_id = 6;  // 0 = not checked
}
```

`PlaceBuilding.building` is already an `Object`, so it gets `unique_id` for
free.

ra2yrcpp:

1. `ClassParser::Object()` sets `unique_id` from `ObjectClass::UniqueID`, so
   every object in `GetGameState` carries it. `parse_Factories` sets
   `object_unique_id` from `Factory.Object`, or 0 if there is none.
2. `UnitOrderCtx::perform` runs the existing live checks (sources exist, are
   alive, owned, legal mission; target exists), then
   `seat_checks::check_unit_order_unique_ids`, then issues the order. A length
   mismatch, a source whose live `UniqueID` differs from its entry, or a
   target whose live `UniqueID` differs from `target_unique_id` rejects the
   whole order before anything is issued. A source entry of 0 is compared like
   any other value, not skipped. The target ID is ignored for actions that do
   not use a target (`STOP`).
3. `place_building` compares a non-zero `building.unique_id` with the live
   factory object's `UniqueID`.
4. The comparison logic is in `src/seat_checks.cpp` (part of
   `ra2yrcpp_core`, no game headers) and is unit-tested natively in
   `tests/test_seat_checks.cpp`. The game-memory lookups around it are only
   compiled for the DLL and have not been run in game.

The IDs are only checked when present, so old clients keep working. The
Bindery adapter should always send them: copy `Object.unique_id` from the same
snapshot the address came from.

### What remains

- **Optional, not enforced.** A client that omits the IDs gets the old
  behaviour. A config switch (for example `requireUniqueIds`) could make them
  mandatory once every client sends them. It is not implemented.
- **`SELL_CELL`** names a cell, not an object. The building is resolved from
  the live cell at issue time, so there is no stored address to go stale;
  `object_unique_ids` is not used for it.
- **`ProduceOrder`** names a type class, which is never freed, so it needs no
  ID.
- **Event latency** (see above) is unchanged: the ID is checked when the event
  is issued, not when it executes.
- **Submodule.** `src/protocol/ra2yrproto` points at the bayleafwalker fork
  until the fields are upstream.

### Without the IDs (older clients)

For clients that do not send the IDs, only partial measures are possible, and
none is implemented:

- The server cannot tell which object the agent meant, because the request
  carries nothing but the address. An in-process map from address to
  (UniqueID, first-seen frame) in the state parser could detect that an address
  was reused recently. It could not tell whether the agent saw the old or the
  new object. The only enforcement it allows is a heuristic, such as refusing
  objects created in the last N frames, which also refuses correct orders for
  new units. That is not clean, so it was not implemented.
- Putting `UniqueID` into an existing field, such as `Object.array_index`,
  would change that field's meaning for every client, so it was rejected.
- On the adapter side, Bindery can reduce the risk by ordering only addresses
  present in the latest `GetGameState` snapshot, and by dropping a stored
  address once it disappears from a snapshot or its `pointer_technotypeclass`
  changes. Snapshots are taken every frame, so a reuse inside one frame is the
  remaining gap. The server cannot enforce this.

## Command transport

### What exists

- `InstrumentationService` runs a websocketpp server on `port`
  (default 14521). Remote addresses are filtered by `allowedHostsRegex` in the
  TCP pre-init handler. `max_connections` caps concurrent connections.
- Each connection sends `ra2yrproto.Command` messages, as binary protobuf or as
  protobuf JSON. A plain HTTP `POST` with a JSON body is also accepted.
- `command_type = CLIENT_COMMAND` with `command` an `Any` wrapping, for
  example, `ra2yrproto.commands.UnitOrder` is the "RunCommand" path. The
  service takes the command name from the `Any` type URL, checks it against
  `allowedCommands`, and creates the command. It replies with `RunCommandAck`
  (`id`, `queue_id`), or with the results directly if `blocking` is set.
- Results are fetched with `POLL` / `POLL_BLOCKING`. Each connection gets a
  result queue whose id is its socket id, created on accept and destroyed on
  close.
- Game commands (`UnitOrder`, `ProduceOrder`, `PlaceBuilding`, ...) run their
  body on the game thread at the next `GameLoopBegin`. Their result arrives in
  the queue after that frame.

### What a per-seat connection should look like

For Bindery, each game client runs its own ra2yrcpp instance, so "one seat" is
"one game process". A seat connection should:

1. Run with `allowedCommands` set to the Bindery list above, and with
   `allowedHostsRegex` limited to the adapter's address, not `0.0.0.0` or a
   whole subnet.
2. Use one connection for orders, and optionally a second for telemetry reads.
   `max_connections` can be set to the number the adapter needs, which keeps
   other local processes off the port.
3. Always poll its own queue, i.e. omit `queue_id` in `POLL_BLOCKING`.

Possible follow-ups in C++ only (not implemented):

- Per-connection policy: tie an allowlist to the connection (for example, a
  port or listener per policy) instead of the whole service.
- Restrict `POLL_BLOCKING` to the caller's own queue when `allowedCommands` is
  set.

Follow-ups that need proto changes:

- An authenticated hello message that binds a connection to a seat and a
  policy.
- Returning the seat's house index or `self` in the ack, so the adapter can
  confirm which house it controls.
