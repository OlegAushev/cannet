#include <canopen/sdo.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace cannet::canopen;

TEST_CASE("read request encodes as an SDO upload initiate", "[sdo]")
{
  auto const sdo = make_sdo_read_request(0x2001, 0x03);
  auto const data = to_payload(sdo);

  CHECK(data[0] == 0x40); // cs = 2 (client init read)
  CHECK(data[1] == 0x01); // index, little-endian
  CHECK(data[2] == 0x20);
  CHECK(data[3] == 0x03); // subindex
  CHECK(data[4] == 0x00);
  CHECK(data[7] == 0x00);

  CHECK(sdo_cs_code(data) == sdo_cs_codes::client_init_read);
}

TEST_CASE("write request announces its data size", "[sdo]")
{
  // CiA 301 expedited download command bytes: 0x2F/0x2B/0x27/0x23 for 1..4
  // significant bytes.
  struct {
    std::uint32_t size;
    std::uint8_t command;
  } const cases[] = {{1, 0x2F}, {2, 0x2B}, {3, 0x27}, {4, 0x23}};

  for (auto const& c : cases) {
    auto const sdo = make_sdo_write_request(0x2001,
                                            0x04,
                                            {0xEF, 0xBE, 0xAD, 0xDE},
                                            c.size);
    auto const data = to_payload(sdo);

    CAPTURE(c.size);
    CHECK(data[0] == c.command);
    CHECK(sdo.data_size() == c.size);
    CHECK(sdo_cs_code(data) == sdo_cs_codes::client_init_write);
    CHECK(data[4] == 0xEF);
    CHECK(data[7] == 0xDE);
  }
}

TEST_CASE("server read response decodes into fields", "[sdo]")
{
  // 0x43: cs = 2, expedited, size indicated, 4 significant bytes.
  payload const data = {0x43, 0x0A, 0x10, 0x00, 0x78, 0x56, 0x34, 0x12};
  auto const sdo = from_payload<expedited_sdo>(data);

  CHECK(sdo.cs == sdo_cs_codes::server_init_read);
  CHECK(sdo.expedited_transfer == 1);
  CHECK(sdo.data_size_indicated == 1);
  CHECK(sdo.data_size() == 4);
  CHECK(sdo.index == 0x100A);
  CHECK(sdo.subindex == 0x00);
  CHECK(sdo.data == expedited_sdo_data{0x78, 0x56, 0x34, 0x12});
}

TEST_CASE("server write response carries no data", "[sdo]")
{
  payload const data = {0x60, 0x01, 0x20, 0x04, 0x00, 0x00, 0x00, 0x00};
  auto const sdo = from_payload<expedited_sdo>(data);

  CHECK(sdo.cs == sdo_cs_codes::server_init_write);
  CHECK(sdo.index == 0x2001);
  CHECK(sdo.subindex == 0x04);
}

TEST_CASE("abort frame decodes into an abort code", "[sdo]")
{
  payload const data = {0x80, 0x01, 0x20, 0x04, 0x00, 0x00, 0x02, 0x06};
  auto const abort = from_payload<abort_sdo>(data);

  REQUIRE(abort.valid());
  CHECK(sdo_cs_code(data) == sdo_cs_codes::abort);
  CHECK(abort.index == 0x2001);
  CHECK(abort.subindex == 0x04);
  CHECK(abort.error_code
        == std::to_underlying(sdo_abort_code::object_not_found));
}

TEST_CASE("an expedited response is not mistaken for an abort", "[sdo]")
{
  auto const response = from_payload<abort_sdo>(
      payload{0x43, 0x0A, 0x10, 0x00, 0x78, 0x56, 0x34, 0x12});
  CHECK_FALSE(response.valid());
}

TEST_CASE("every abort code has a message", "[sdo]")
{
  sdo_abort_code const codes[] = {sdo_abort_code::invalid_cs,
                                  sdo_abort_code::unsupported_access,
                                  sdo_abort_code::read_from_write_only,
                                  sdo_abort_code::write_to_read_only,
                                  sdo_abort_code::object_not_found,
                                  sdo_abort_code::hardware_error,
                                  sdo_abort_code::data_type_mismatch,
                                  sdo_abort_code::value_range_exceeded,
                                  sdo_abort_code::value_too_high,
                                  sdo_abort_code::value_too_low,
                                  sdo_abort_code::general_error,
                                  sdo_abort_code::data_store_error,
                                  sdo_abort_code::local_control_error,
                                  sdo_abort_code::state_error,
                                  sdo_abort_code::no_data_available};

  for (auto code : codes) {
    CAPTURE(std::to_underlying(code));
    auto const message = to_string(code);
    CHECK_FALSE(message.empty());
    CHECK(message != "unknown abort code");
  }

  CHECK(to_string(static_cast<sdo_abort_code>(0x1234'5678))
        == "unknown abort code");
}
