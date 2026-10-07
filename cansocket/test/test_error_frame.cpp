#include <cansocket/raw/error_frame.hpp>

#include <catch2/catch_test_macros.hpp>

#include <linux/can/error.h>

#include <array>
#include <cstdint>

using namespace cannet::raw;

namespace {

can_frame error_frame(canid_t classes, std::array<std::uint8_t, 8> data = {})
{
  can_frame frame{};
  frame.can_id = CAN_ERR_FLAG | classes;
  frame.len = CAN_ERR_DLC;
  for (std::size_t i = 0; i < data.size(); ++i) {
    frame.data[i] = data[i];
  }
  return frame;
}

} // namespace

TEST_CASE("a frame from the bus is no error frame", "[error_frame]")
{
  can_frame frame{};
  frame.can_id = 0x081;
  frame.len = 8;
  CHECK_FALSE(is_error_frame(frame));
  CHECK_FALSE(decode_error_frame(frame));

  // An extended id is no error flag.
  frame.can_id = 0x1FFFFFFF | CAN_EFF_FLAG;
  CHECK_FALSE(decode_error_frame(frame));
}

TEST_CASE("a bus-off and a restart name the state they leave the controller "
          "in",
          "[error_frame]")
{
  auto const off = decode_error_frame(error_frame(CAN_ERR_BUSOFF));
  REQUIRE(off);
  CHECK(off->state == controller_state::bus_off);
  CHECK_FALSE(off->restarted);

  auto const restarted = decode_error_frame(error_frame(CAN_ERR_RESTARTED));
  REQUIRE(restarted);
  CHECK(restarted->restarted);
  CHECK(restarted->state == controller_state::error_active);
}

TEST_CASE("a change of state names the worse of the two counters' states",
          "[error_frame]")
{
  auto const state_of = [](std::uint8_t status) {
    auto const report = decode_error_frame(
        error_frame(CAN_ERR_CRTL, {0, status, 0, 0, 0, 0, 0, 0}));
    REQUIRE(report);
    return report->state;
  };
  CHECK(state_of(CAN_ERR_CRTL_RX_WARNING) == controller_state::error_warning);
  CHECK(state_of(CAN_ERR_CRTL_TX_WARNING) == controller_state::error_warning);
  CHECK(state_of(CAN_ERR_CRTL_RX_PASSIVE) == controller_state::error_passive);
  CHECK(state_of(CAN_ERR_CRTL_TX_PASSIVE | CAN_ERR_CRTL_RX_WARNING)
        == controller_state::error_passive);
  CHECK(state_of(CAN_ERR_CRTL_ACTIVE) == controller_state::error_active);
  // An overflow alone changes no state.
  CHECK_FALSE(state_of(CAN_ERR_CRTL_RX_OVERFLOW));
}

TEST_CASE("an error frame reports lost frames, errors on the wire and the "
          "counters",
          "[error_frame]")
{
  auto const report = decode_error_frame(
      error_frame(CAN_ERR_CRTL
                      | CAN_ERR_PROT
                      | CAN_ERR_ACK
                      | CAN_ERR_BUSERROR
                      | CAN_ERR_TRX
                      | CAN_ERR_LOSTARB
                      | CAN_ERR_TX_TIMEOUT
                      | CAN_ERR_CNT,
                  {0,
                   CAN_ERR_CRTL_RX_OVERFLOW | CAN_ERR_CRTL_TX_OVERFLOW,
                   CAN_ERR_PROT_STUFF,
                   CAN_ERR_PROT_LOC_ACK,
                   CAN_ERR_TRX_CANH_NO_WIRE,
                   0,
                   10,
                   128}));
  REQUIRE(report);
  CHECK(report->rx_overflow);
  CHECK(report->tx_overflow);
  CHECK(report->protocol_violation);
  CHECK(report->no_ack);
  CHECK(report->bus_error);
  CHECK(report->transceiver);
  CHECK(report->lost_arbitration);
  CHECK(report->tx_timeout);
  CHECK_FALSE(report->restarted);
  CHECK_FALSE(report->state);
  REQUIRE(report->counters);
  CHECK(report->counters->tx == 10);
  CHECK(report->counters->rx == 128);
}

TEST_CASE("an error frame reports only what its classes name", "[error_frame]")
{
  // Bytes a frame's classes do not name are not read.
  auto const report = decode_error_frame(
      error_frame(CAN_ERR_ACK, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 7, 9}));
  REQUIRE(report);
  CHECK(report->no_ack);
  CHECK_FALSE(report->rx_overflow);
  CHECK_FALSE(report->state);
  CHECK_FALSE(report->counters);
  CHECK_FALSE(report->protocol_violation);

  // A frame too short for the counters reports them as zero.
  auto shortened = error_frame(CAN_ERR_CNT, {0, 0, 0, 0, 0, 0, 7, 9});
  shortened.len = 6;
  auto const counted = decode_error_frame(shortened);
  REQUIRE(counted);
  REQUIRE(counted->counters);
  CHECK(counted->counters->tx == 0);
  CHECK(counted->counters->rx == 0);
}

TEST_CASE("every controller_state has a description and a name",
          "[error_frame]")
{
  for (auto const s : {controller_state::error_active,
                       controller_state::error_warning,
                       controller_state::error_passive,
                       controller_state::bus_off}) {
    CHECK(to_string(s) != "unknown state");
    CHECK(name(s) != "unknown");
  }
  CHECK(name(controller_state::bus_off) == "bus_off");
  CHECK(name(static_cast<controller_state>(0xFF)) == "unknown");
}
