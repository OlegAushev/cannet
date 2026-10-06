#include <canopen/detail/emcy_consumer.hpp>

#include <utility>

namespace cannet::canopen::detail {

struct emcy_consumer::state {
  explicit state(transport& b) : bus(b) {}

  transport& bus;
  subscription frames;
  event<emcy_message> received;

  void bind(node_id id)
  {
    frames = bus.subscribe(cob_filter(cob_id(cob_type::emcy, id)),
                           [this](can_frame const& frame) {
                             if (auto const message = decode_emcy(frame)) {
                               received.emit(*message);
                             }
                           });
  }
};

emcy_consumer::emcy_consumer(transport& bus, node_id id)
    : state_(std::make_unique<state>(bus))
{
  state_->bind(id);
}

emcy_consumer::~emcy_consumer() = default;

subscription emcy_consumer::on_emcy(emcy_handler handler)
{
  return state_->received.subscribe(std::move(handler));
}

void emcy_consumer::rebind(node_id id)
{
  state_->bind(id);
}

} // namespace cannet::canopen::detail
