#include "ra2yrproto/commands_game.pb.h"

#include "gtest/gtest.h"
#include "seat_checks.hpp"

#include <cstdint>

#include <map>
#include <stdexcept>
#include <string>

using ra2yrcpp::seat_checks::check_unique_id;
using ra2yrcpp::seat_checks::check_unit_order_unique_ids;
using ra2yrproto::commands::UnitOrder;

namespace {

// Live objects of a pretend game: address -> UniqueID.
class UniqueIdTest : public ::testing::Test {
 protected:
  void SetUp() override {
    live_ = {{0x1000U, 11U}, {0x2000U, 12U}, {0x3000U, 13U}};
  }

  std::uint32_t lookup(std::uintptr_t a) {
    lookups_++;
    return live_.at(a);
  }

  void check(const UnitOrder& o, bool check_target = true) {
    check_unit_order_unique_ids(o, check_target,
                                [this](std::uintptr_t a) { return lookup(a); });
  }

  // Expect a rejection whose message contains needle.
  void expect_reject(const UnitOrder& o, const std::string& needle,
                     bool check_target = true) {
    try {
      check(o, check_target);
      FAIL() << "order was accepted, expected: " << needle;
    } catch (const std::runtime_error& e) {
      EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
          << e.what();
    }
  }

  static UnitOrder order(std::initializer_list<std::uint32_t> addresses,
                         std::initializer_list<std::uint32_t> ids) {
    UnitOrder o;
    for (auto a : addresses) {
      o.add_object_addresses(a);
    }
    for (auto i : ids) {
      o.add_object_unique_ids(i);
    }
    return o;
  }

  std::map<std::uintptr_t, std::uint32_t> live_;
  int lookups_{0};
};

}  // namespace

TEST_F(UniqueIdTest, OrderWithoutIdsIsUnchecked) {
  auto o = order({0x1000U, 0x2000U}, {});
  o.set_target_object(0x3000U);
  EXPECT_NO_THROW(check(o));
  EXPECT_EQ(lookups_, 0);
}

TEST_F(UniqueIdTest, MatchingIdsPass) {
  auto o = order({0x1000U, 0x2000U}, {11U, 12U});
  o.set_target_object(0x3000U);
  o.set_target_unique_id(13U);
  EXPECT_NO_THROW(check(o));
  EXPECT_EQ(lookups_, 3);
}

TEST_F(UniqueIdTest, LengthMismatchRejects) {
  expect_reject(order({0x1000U, 0x2000U}, {11U}),
                "1 object_unique_ids for 2 object_addresses");
  expect_reject(order({0x1000U}, {11U, 12U}),
                "2 object_unique_ids for 1 object_addresses");
}

TEST_F(UniqueIdTest, RecycledSourceRejects) {
  // 0x2000 was freed and reused: the client saw UniqueID 7, it is now 12.
  expect_reject(order({0x1000U, 0x2000U}, {11U, 7U}),
                "order source 0x2000 was recycled: expected unique_id 7, "
                "found 12");
}

TEST_F(UniqueIdTest, ZeroSourceIdIsNotAWildcard) {
  expect_reject(order({0x1000U}, {0U}), "order source 0x1000 was recycled");
}

TEST_F(UniqueIdTest, RecycledTargetRejects) {
  auto o = order({0x1000U}, {11U});
  o.set_target_object(0x3000U);
  o.set_target_unique_id(99U);
  expect_reject(o,
                "order target 0x3000 was recycled: expected unique_id 99, "
                "found 13");
}

TEST_F(UniqueIdTest, TargetCheckedWithoutSourceIds) {
  auto o = order({0x1000U}, {});
  o.set_target_object(0x3000U);
  o.set_target_unique_id(99U);
  expect_reject(o, "order target 0x3000 was recycled");
}

TEST_F(UniqueIdTest, TargetIdWithoutTargetRejects) {
  auto o = order({0x1000U}, {});
  o.set_target_unique_id(13U);
  expect_reject(o, "target_unique_id 13 set without target_object");
}

TEST_F(UniqueIdTest, TargetIgnoredWhenActionHasNoTarget) {
  // e.g. UNIT_ACTION_STOP, which never passes the target to the game.
  auto o = order({0x1000U}, {11U});
  o.set_target_object(0x3000U);
  o.set_target_unique_id(99U);
  EXPECT_NO_THROW(check(o, false));
}

TEST(CheckUniqueIdTest, ZeroExpectedIsNotChecked) {
  EXPECT_NO_THROW(check_unique_id("building", 0x1000U, 0U, 42U));
  EXPECT_NO_THROW(check_unique_id("building", 0x1000U, 42U, 42U));
  try {
    check_unique_id("building", 0x1000U, 41U, 42U);
    FAIL() << "mismatch accepted";
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ(e.what(),
                 "building 0x1000 was recycled: expected unique_id 41, "
                 "found 42");
  }
}
