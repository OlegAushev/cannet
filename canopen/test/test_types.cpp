#include <canopen/types.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace cannet::canopen;

namespace {

constexpr auto node = node_id::literal(0x0A);

struct tpdo1_layout {
  std::uint8_t state;
  std::uint8_t flags;
  std::uint16_t voltage;
  std::uint32_t counter;
};

} // namespace

TEST_CASE("node id accepts only [1, 127]", "[types]")
{
  CHECK_FALSE(node_id::make(0).has_value());
  CHECK_FALSE(node_id::make(128).has_value());
  CHECK_FALSE(node_id::make(0xFFFF'FFFF).has_value());

  auto const lowest = node_id::make(1);
  auto const highest = node_id::make(127);
  REQUIRE(lowest.has_value());
  REQUIRE(highest.has_value());
  CHECK(lowest->get() == 1);
  CHECK(highest->get() == 127);
  CHECK(*lowest == node_id::literal(1));
  CHECK(*lowest < *highest);
}

TEST_CASE("predefined connection set matches CiA 301", "[types]")
{
  STATIC_CHECK(cob_id(cob_type::tsdo, node) == 0x58A);
  STATIC_CHECK(cob_id(cob_type::rsdo, node) == 0x60A);
  STATIC_CHECK(cob_id(cob_type::heartbeat, node) == 0x70A);
  STATIC_CHECK(cob_id(cob_type::emcy, node) == 0x08A);

  // Broadcast services carry no node id.
  STATIC_CHECK(cob_id(cob_type::nmt, node) == 0x000);
  STATIC_CHECK(cob_id(cob_type::sync, node) == 0x080);
  STATIC_CHECK(cob_id(cob_type::time, node) == 0x100);

  STATIC_CHECK(cob_id(cob_type::sync) == 0x080);
  STATIC_CHECK_FALSE(cob_id(cob_type::tsdo).has_value());
}

TEST_CASE("predefined PDO COB-IDs exist for PDO 1..4 only", "[types]")
{
  STATIC_CHECK(pdo_cob_id(cob_type::tpdo, 1, node) == 0x18A);
  STATIC_CHECK(pdo_cob_id(cob_type::tpdo, 4, node) == 0x48A);
  STATIC_CHECK(pdo_cob_id(cob_type::rpdo, 1, node) == 0x20A);
  STATIC_CHECK(pdo_cob_id(cob_type::rpdo, 4, node) == 0x50A);

  STATIC_CHECK_FALSE(pdo_cob_id(cob_type::tpdo, 0, node).has_value());
  STATIC_CHECK_FALSE(pdo_cob_id(cob_type::tpdo, 5, node).has_value());
  STATIC_CHECK_FALSE(pdo_cob_id(cob_type::heartbeat, 1, node).has_value());
}

TEST_CASE("payload conversion round-trips a PDO layout", "[types]")
{
  tpdo1_layout const sent = {.state = 0x05,
                             .flags = 0x81,
                             .voltage = 0x1234,
                             .counter = 0xDEAD'BEEF};

  payload const data = to_payload(sent);
  CHECK(data[0] == 0x05);
  CHECK(data[1] == 0x81);
  CHECK(data[2] == 0x34); // little-endian
  CHECK(data[3] == 0x12);

  auto const received = from_payload<tpdo1_layout>(data);
  CHECK(received.state == sent.state);
  CHECK(received.flags == sent.flags);
  CHECK(received.voltage == sent.voltage);
  CHECK(received.counter == sent.counter);
}

TEST_CASE("make_frame transmits only the announced bytes", "[types]")
{
  payload const data = {1, 2, 3, 4, 5, 6, 7, 8};
  can_frame const frame = make_frame(0x18A, 3, data);

  CHECK(frame.can_id == 0x18A);
  CHECK(frame.len == 3);
  CHECK(frame.data[0] == 1);
  CHECK(frame.data[2] == 3);
  CHECK(frame.data[3] == 0); // beyond len: untouched
  CHECK(frame.data[7] == 0);
}

TEST_CASE("NMT commands carry their wire codes", "[types]")
{
  STATIC_CHECK(std::to_underlying(nmt_command::start) == 0x01);
  STATIC_CHECK(std::to_underlying(nmt_command::stop) == 0x02);
  STATIC_CHECK(std::to_underlying(nmt_command::enter_pre_operational) == 0x80);
  STATIC_CHECK(std::to_underlying(nmt_command::reset_node) == 0x81);
  STATIC_CHECK(std::to_underlying(nmt_command::reset_communication) == 0x82);

  STATIC_CHECK(std::to_underlying(nmt_state::operational) == 0x05);
  STATIC_CHECK(std::to_underlying(nmt_state::pre_operational) == 0x7F);

  CHECK(to_string(nmt_state::stopped) == "stopped");
  CHECK(to_string(nmt_command::reset_node) == "reset node");
}

TEST_CASE("a COB-ID filter takes standard data frames only", "[types]")
{
  constexpr auto filter = cob_filter(0x181);
  STATIC_CHECK(filter.can_id == 0x181);
  STATIC_CHECK((filter.can_mask & CAN_SFF_MASK) == CAN_SFF_MASK);
  STATIC_CHECK((filter.can_mask & CAN_EFF_FLAG) != 0);
  STATIC_CHECK((filter.can_mask & CAN_RTR_FLAG) != 0);
}

TEST_CASE("NMT states and commands have stable names", "[types]")
{
  CHECK(name(nmt_state::initializing) == "initializing");
  CHECK(name(nmt_state::stopped) == "stopped");
  CHECK(name(nmt_state::operational) == "operational");
  CHECK(name(nmt_state::pre_operational) == "pre_operational");
  CHECK(name(static_cast<nmt_state>(0x42)) == "unknown");

  CHECK(name(nmt_command::start) == "start");
  CHECK(name(nmt_command::stop) == "stop");
  CHECK(name(nmt_command::enter_pre_operational) == "enter_pre_operational");
  CHECK(name(nmt_command::reset_node) == "reset_node");
  CHECK(name(nmt_command::reset_communication) == "reset_communication");
}

TEST_CASE("NMT frames carry the command and its target", "[types]")
{
  auto const all = make_nmt_frame(
      {.command = nmt_command::start, .target = std::nullopt});
  CHECK(all.can_id == 0x000);
  CHECK(all.len == 2);
  CHECK(all.data[0] == 0x01);
  CHECK(all.data[1] == 0x00);
  CHECK(decode_nmt(all)
        == nmt_message{.command = nmt_command::start, .target = std::nullopt});

  auto const one = make_nmt_frame(
      {.command = nmt_command::reset_node, .target = node_id::literal(5)});
  CHECK(one.data[0] == 0x81);
  CHECK(one.data[1] == 0x05);
  CHECK(decode_nmt(one)
        == nmt_message{.command = nmt_command::reset_node,
                       .target = node_id::literal(5)});

  auto short_frame = one;
  short_frame.len = 1;
  CHECK_FALSE(decode_nmt(short_frame));
  auto unknown_command = one;
  unknown_command.data[0] = 0x03;
  CHECK_FALSE(decode_nmt(unknown_command));
  auto bad_target = one;
  bad_target.data[1] = 0x80;
  CHECK_FALSE(decode_nmt(bad_target));
}

TEST_CASE("SYNC carries no data", "[types]")
{
  auto const sync = make_sync_frame();
  CHECK(sync.can_id == 0x080);
  CHECK(sync.len == 0);
}

TEST_CASE("a heartbeat carries the node's state", "[types]")
{
  auto const hb = make_heartbeat_frame(node_id::literal(1),
                                       nmt_state::pre_operational);
  CHECK(hb.can_id == 0x701);
  CHECK(hb.len == 1);
  CHECK(hb.data[0] == 0x7F);
  CHECK(decode_heartbeat(hb) == nmt_state::pre_operational);

  auto const boot_up = make_heartbeat_frame(node_id::literal(1),
                                            nmt_state::initializing);
  CHECK(decode_heartbeat(boot_up) == nmt_state::initializing);

  auto toggled = hb; // node guarding sets bit 7 alternately
  toggled.data[0] = 0x85;
  CHECK(decode_heartbeat(toggled) == nmt_state::operational);

  auto empty = hb;
  empty.len = 0;
  CHECK_FALSE(decode_heartbeat(empty));
  auto unknown = hb;
  unknown.data[0] = 0x42;
  CHECK_FALSE(decode_heartbeat(unknown));
}

TEST_CASE("an EMCY frame carries code, register and manufacturer bytes",
          "[types]")
{
  emcy_message const rpdo_timeout{.error_code = 0x8250,
                                  .error_register = 0x10,
                                  .manufacturer = {1, 2, 3, 4, 5}};
  auto const frame = make_emcy_frame(node_id::literal(1), rpdo_timeout);
  CHECK(frame.can_id == 0x081);
  CHECK(frame.len == 8);
  CHECK(frame.data[0] == 0x50); // little-endian, as emblib sends it
  CHECK(frame.data[1] == 0x82);
  CHECK(frame.data[2] == 0x10);
  CHECK(frame.data[7] == 5);
  CHECK(decode_emcy(frame) == rpdo_timeout);

  auto short_frame = frame;
  short_frame.len = 3;
  CHECK(decode_emcy(short_frame)
        == emcy_message{.error_code = 0x8250,
                        .error_register = 0x10,
                        .manufacturer = {}});
  short_frame.len = 2;
  CHECK_FALSE(decode_emcy(short_frame));
}
