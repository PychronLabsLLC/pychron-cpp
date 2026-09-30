#include "pychron/devices/spectrometer/legacy/dac_output.hpp"

#include <mutex>

#include "pychron/codecs/map215.hpp"

namespace pychron::spectrometer {

namespace map = codec::map215;

Map215Dac::Map215Dac(Transport& transport, int range, double full_scale)
    : transport_(transport), range_(range), full_scale_(full_scale) {}

Result<double> Map215Dac::write(double volts) {
  auto code = map::to_code(volts, full_scale_);
  if (!code) return fail(std::move(code).error());
  auto select = map::select_range(range_);
  if (!select) return fail(std::move(select).error());
  auto value = map::write_code(*code);
  if (!value) return fail(std::move(value).error());
  auto written = transact(transport_, [&]() -> Result<void> {
    if (auto r = transport_.write(select->tx); !r) return r;
    return transport_.write(value->tx);
  });
  if (!written) return fail(std::move(written).error());
  return map::to_volts(*code, full_scale_);
}

SimTransport::Hook map215_sim_hook(Map215SimModel model) {
  struct State {
    Map215SimModel model;
    std::mutex mutex;
    int range = 0;

    Bytes respond(const Bytes& tx) {
      std::lock_guard lock(mutex);
      auto request = map::decode_request(tx);
      if (!request) return {};
      if (request->kind == map::Request::Kind::SelectRange) {
        range = static_cast<int>(request->value);
      } else if (model.on_output) {
        model.on_output(range, map::to_volts(request->value, model.full_scale));
      }
      return {};
    }
  };
  auto state = std::make_shared<State>();
  state->model = std::move(model);
  return [state](const Bytes& tx) { return state->respond(tx); };
}

}  // namespace pychron::spectrometer
