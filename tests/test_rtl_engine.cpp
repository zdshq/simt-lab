#include "hardware_support.hpp"
#include "simt/tensor_engine.hpp"

namespace simt::hw::test {
namespace {
std::unique_ptr<TensorEngine> engine() {
    TensorConfig config; config.rtl = true;
    auto result = make_tensor_engine(config);
    require(result->name() == "rtl", "requested RTL silently fell back to model");
    return result;
}

void feed(TensorEngine& unit, const TensorInput& wave) {
    for (uint32_t attempt = 0; attempt < 10000; ++attempt) {
        if (unit.push(wave)) { unit.tick(); return; }
        unit.tick();
    }
    throw std::runtime_error("RTL input watchdog");
}

std::array<uint32_t, 16> result(TensorEngine& unit) {
    for (uint32_t attempt = 0; attempt < 10000; ++attempt) {
        if (auto output = unit.take()) return *output;
        unit.tick();
    }
    throw std::runtime_error("RTL result watchdog");
}

// API calls do not advance a hidden RTL clock; work counts follow physical injection and PE events.
// API 调用不推进隐藏 RTL 时钟；工作量计数跟随物理注入和 PE 事件。
void clock_and_event_contract() {
    auto unit = engine(); unit->begin(1, 1, 3, false, {});
    TensorInput wave; wave.raw_a[0] = 3; wave.raw_b[0] = 4; wave.last = true;
    for (uint32_t call = 0; call < 20; ++call) require(!unit->take() && !unit->push(wave), "unclocked command completed");
    require(unit->cycle() == 0 && unit->stats().tiles == 0, "API call advanced hardware");
    unit->tick(); unit->tick();
    require(unit->stats().tiles == 1 && unit->push(wave), "start handshake missing");
    require(unit->stats().waves == 0 && unit->stats().products == 0, "work counted at host submission");
    unit->tick(); require(unit->stats().waves == 0, "FIFO bypassed");
    unit->tick(); require(unit->stats().waves == 1 && unit->stats().products == 0, "edge injection accounting");
    unit->tick(); require(unit->stats().products == 1, "PE issue event missing");
    require(result(*unit)[0] == 12 && unit->cycle() == 9, "one-edge-per-clock result");
    require(!unit->take() && !unit->busy(), "result consumed twice");
}

void raw_formats_and_immediate_reuse() {
    auto unit = engine();
    constexpr std::array<uint32_t, 5> a{0x40000000, 0x4000, 0x4000, 2, 0x40};
    constexpr std::array<uint32_t, 5> b{0x40400000, 0x4200, 0x4040, 3, 0x05};
    for (uint32_t format = 0; format < 5; ++format) {
        std::array<uint32_t, 16> initial{}; initial[0] = format == 3 ? 1 : 0x3f800000;
        unit->begin(1, 1, format, format != 3, initial);
        TensorInput wave; wave.raw_a[0] = a[format]; wave.raw_b[0] = b[format]; wave.high_a[0] = true;
        feed(*unit, wave);
        wave.enabled[0] = false; wave.last = true; wave.block_end = true;
        wave.scale_a[0] = 0x3f000000; wave.scale_b[0] = 0x40000000; feed(*unit, wave);
        require(result(*unit)[0] == (format == 3 ? 7u : 0x40e00000), "raw format or previous tile leaked");
    }
    require(unit->stats().tiles == 5 && unit->stats().products == 5 && unit->stats().skipped == 5, "PE event masks");
    require(unit->stats().scale_products == 4 && unit->stats().scale_accumulates == 4, "scale issue events");
}

void independent_instances() {
    auto first = engine(), second = engine();
    first->begin(1, 1, 3, false, {}); second->begin(1, 1, 3, false, {});
    TensorInput wave; wave.raw_a[0] = 7; wave.raw_b[0] = 11; wave.last = true;
    feed(*first, wave); require(result(*first)[0] == 77, "first engine result");
    require(second->cycle() == 0 && !second->take(), "clock or state shared between SMs");
    wave.raw_a[0] = 13; feed(*second, wave);
    require(result(*second)[0] == 143, "second engine reused another SM's state");
}

void bounded_input_and_actual_work() {
    auto unit = engine(); unit->begin(4, 4, 3, false, {});
    TensorInput wave; wave.raw_a.fill(2); wave.raw_b.fill(3); wave.enabled = {true, false, true, false};
    for (uint32_t k = 0; k < 20; ++k) { wave.last = k == 19; feed(*unit, wave); }
    const auto values = result(*unit);
    for (uint32_t index = 0; index < 16; ++index) require(values[index] == (index/4%2 ? 0u : 120u), "mesh result");
    const auto& stats = unit->stats();
    require(stats.waves == 20 && stats.products == 160 && stats.skipped == 160, "mesh work accounting");
    require(stats.queue_stalls > 0, "finite physical FIFO did not apply backpressure");
}

void rejected_commands_preserve_engine() {
    TensorConfig config; config.rtl = true; config.latency = 1;
    rejects([&] { make_tensor_engine(config); }, "tensor_rtl_requires_latency_4_and_queue_2");
    config.latency = 4; config.input_capacity = 1;
    rejects([&] { make_tensor_engine(config); }, "tensor_rtl_requires_latency_4_and_queue_2");
    auto unit = engine();
    rejects([&] { unit->begin(0, 1, 0, false, {}); }, "invalid_tensor_array_shape");
    unit->begin(1, 1, 0, true, {});
    TensorInput wave; wave.last = true;
    rejects([&] { unit->push(wave); }, "tensor_array_missing_block_end");
    require(unit->cycle() == 0, "failed command advanced clock");
    wave.block_end = true; feed(*unit, wave); require(result(*unit)[0] == 0, "failed command corrupted state");
}

void run_cases() {
    run("RTL bridge explicit clock and physical events", clock_and_event_contract);
    run("RTL bridge raw formats and pending result/start reuse", raw_formats_and_immediate_reuse);
    run("RTL bridge independent SM instances", independent_instances);
    run("RTL bridge finite input and work counts", bounded_input_and_actual_work);
    run("RTL bridge validation without partial mutation", rejected_commands_preserve_engine);
}
}

int run_tests() {
    try { run_cases(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main() { return simt::hw::test::run_tests(); }
