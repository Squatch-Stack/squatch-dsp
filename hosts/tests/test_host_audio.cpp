// SPDX-License-Identifier: MIT
// The CoreAudio host's audio path, tested offline with no audio device and no CoreAudio (so any CI machine
// runs it): a test tone goes through the same TunerRender that the macOS IOProc calls, in the buffer shapes
// CoreAudio delivers (interleaved, one stream per channel, odd sizes), and the detected pitch, the channel
// pick, the outputs of --out none|mute|thru and the real-time rule (no allocation) are checked.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>
#include <vector>

#include "../common/tone_gen.hpp"
#include "../common/tuner_host.hpp"
#include "../coreaudio/audio_chunks.hpp"
#include "../coreaudio/tuner_render.hpp"

using namespace squatch::host;
using namespace squatch::host::live;
using squatch::host::audio::Chunk;

static int gFailures = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (!(cond)) {                                                \
      ++gFailures;                                                \
      std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                   \
      std::printf("\n");                                          \
    }                                                             \
  } while (0)

// Every global allocation made while armed is counted: the audio callback must make none.
static bool gArmed = false;
static long gNews = 0;
// noinline: GCC 13 otherwise sees malloc paired with free through the inlined new/delete and warns.
[[gnu::noinline]] void* operator new(std::size_t n) {
  if (gArmed) ++gNews;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
[[gnu::noinline]] void operator delete(void* p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr double kRate = 48000.0;

Core& freshCore(float a4 = 440.0f) {
  static Core* core = nullptr;
  delete core;  // a Core is ~130 kB: heap, not stack
  core = new Core;
  core->configure(static_cast<float>(kRate), a4, 38.0f);
  return *core;
}

// `seconds` of a test tone on channel `tone` of a `channels`-wide interleaved device; the other channels
// carry a different tone, so picking the wrong one is visible.
std::vector<float> deviceInput(uint32_t channels, uint32_t tone, double hz, double seconds) {
  const auto frames = static_cast<uint32_t>(seconds * kRate);
  std::vector<float> buf(static_cast<size_t>(frames) * channels, 0.0f);
  ToneGen want, other;
  want.configure(hz, -12.0, kRate);
  other.configure(hz * 1.5, -12.0, kRate);
  want.target.store(1.0f);
  other.target.store(1.0f);
  for (uint32_t c = 0; c < channels; ++c) (c == tone ? want : other).fill(buf.data() + c, frames, channels);
  return buf;
}

struct Result {
  squatch::host::DisplayReading shown;
  bool valid = false;
};

Result lastReading(Core& core) {
  core.latest.update();
  const auto& r = core.latest.front().reading;
  return {toDisplay(r, 440.0f), r.valid};
}

// The device's shape: input channels, output channels (0: an input-only device), frames per callback.
struct Shape {
  uint32_t in, out, block;
};

// Runs the interleaved `in` through `render` in callbacks of `shape.block` frames (the last one shorter).
void feed(TunerRender& render, std::vector<float>& in, Shape shape, std::vector<float>* out = nullptr) {
  const auto frames = static_cast<uint32_t>(in.size() / shape.in);
  for (uint32_t at = 0; at < frames; at += shape.block) {
    const uint32_t n = std::min(shape.block, frames - at);
    Chunk ic{shape.in, static_cast<uint32_t>(n * shape.in * sizeof(float)), in.data() + static_cast<size_t>(at) * shape.in};
    Chunk oc{shape.out, static_cast<uint32_t>(n * shape.out * sizeof(float)), out ? out->data() + static_cast<size_t>(at) * shape.out : nullptr};
    render.render({&ic, 1}, {out ? &oc : nullptr, out ? 1u : 0u}, 1000 + at);
  }
}

void testPitchThroughTheHostLayer() {
  for (const uint32_t block : {512u, 64u, 37u, 4100u}) {  // 4100 is past the render scratch: the block loop splits it
    Core& core = freshCore();
    TunerRender render(core, 1, OutMode::None, kRate);
    auto in = deviceInput(4, 1, 110.0, 2.0);
    feed(render, in, {4, 0, block});
    const Result r = lastReading(core);
    CHECK(r.valid, "block %u: no pitch found", block);
    CHECK(r.shown.midi == 45, "block %u: midi %d, want 45 (A2)", block, r.shown.midi);
    CHECK(std::fabs(r.shown.hz - 110.0) < 0.01, "block %u: %.4f Hz, want 110", block, r.shown.hz);
    CHECK(std::fabs(r.shown.cents) < 0.2, "block %u: %.3f cents off", block, r.shown.cents);
  }
}

void testA4Reference() {
  Core& core = freshCore(432.0f);
  TunerRender render(core, 0, OutMode::None, kRate);
  auto in = deviceInput(1, 0, 108.0, 2.0);  // A2 at A4 = 432 is 108 Hz
  feed(render, in, {1, 0, 256});
  core.latest.update();
  const auto d = toDisplay(core.latest.front().reading, 432.0f);
  CHECK(d.midi == 45 && std::fabs(d.cents) < 0.2, "A4=432: midi %d, %.3f cents", d.midi, d.cents);
}

void testChannelPick() {
  Core& core = freshCore();
  TunerRender wrong(core, 0, OutMode::None, kRate);  // channel 0 carries 165 Hz, not the 110 Hz on channel 1
  auto in = deviceInput(2, 1, 110.0, 2.0);
  feed(wrong, in, {2, 0, 512});
  const Result r = lastReading(core);
  CHECK(!r.valid || std::fabs(r.shown.hz - 165.0) < 0.1, "channel 0 gave %.3f Hz", r.shown.hz);
  Core& core2 = freshCore();
  TunerRender missing(core2, 7, OutMode::None, kRate);  // a channel the device does not have reads as silence
  feed(missing, in, {2, 0, 512});
  CHECK(!lastReading(core2).valid, "a missing channel produced a pitch");
}

void testOutputs() {
  const OutMode modes[] = {OutMode::None, OutMode::Mute, OutMode::Thru};
  for (const OutMode mode : modes) {
    Core& core = freshCore();
    TunerRender render(core, 1, mode, kRate);
    auto in = deviceInput(4, 1, 110.0, 0.5);
    std::vector<float> out(in.size() / 4 * 4, 9.0f);  // 4 outputs, pre-filled: the host must not leave them
    feed(render, in, {4, 4, 512}, &out);
    long wrong = 0;
    for (size_t i = 0; i < out.size() / 4; ++i) {
      const float want = mode == OutMode::Thru ? in[i * 4 + 1] : 0.0f;
      wrong += out[i * 4 + 0] != want || out[i * 4 + 1] != want || out[i * 4 + 2] != 0.0f || out[i * 4 + 3] != 0.0f;
    }
    CHECK(wrong == 0, "mode %d: %ld frames with a wrong output", static_cast<int>(mode), wrong);
  }
}

void testOneStreamPerChannel() {  // some devices present each channel as its own buffer
  Core& core = freshCore();
  TunerRender render(core, 1, OutMode::Thru, kRate);
  const auto frames = static_cast<uint32_t>(2.0 * kRate);
  std::vector<float> a(frames, 0.0f), b(frames), oa(frames, 9.0f), ob(frames, 9.0f);
  ToneGen t;
  t.configure(110.0, -12.0, kRate);
  t.target.store(1.0f);
  t.fill(b.data(), frames);
  for (uint32_t at = 0; at < frames; at += 480) {
    const uint32_t n = std::min(480u, frames - at), bytes = n * 4;
    Chunk ic[2] = {{1, bytes, a.data() + at}, {1, bytes, b.data() + at}};
    Chunk oc[2] = {{1, bytes, oa.data() + at}, {1, bytes, ob.data() + at}};
    render.render({ic, 2}, {oc, 2}, 0);
  }
  const Result r = lastReading(core);
  CHECK(r.valid && r.shown.midi == 45 && std::fabs(r.shown.hz - 110.0) < 0.01, "split streams: %.4f Hz", r.shown.hz);
  CHECK(oa == b && ob == b, "thru over split streams did not copy input 2 to both outputs");
}

void testSameDspAsTheFilePath() {  // runSamples is what --file runs; the live path must reach the same reading
  const auto in = deviceInput(1, 0, 196.0, 2.0);
  Core& live = freshCore();
  std::vector<float> copy(in);
  TunerRender render(live, 0, OutMode::None, kRate);
  feed(render, copy, {1, 0, 64});
  live.latest.update();
  const auto la = live.latest.front().reading;
  Core& file = freshCore();
  Options o;
  runSamples(in, kRate, o, file, [](const Frame&, uint64_t, double) {});
  file.latest.update();
  const auto lb = file.latest.front().reading;
  CHECK(la.hz == lb.hz && la.steadyHz == lb.steadyHz && la.level == lb.level, "live %.6f Hz vs file %.6f Hz", la.hz, lb.hz);
}

void testStampAndCounters() {
  Core& core = freshCore();
  TunerRender render(core, 0, OutMode::None, kRate);
  std::vector<float> in(480, 0.0f);
  Chunk ic{1, 480 * 4, in.data()};
  render.render({&ic, 1}, {}, 5000000);
  core.latest.update();
  const auto& f = core.latest.front();
  CHECK(f.samples == 480 && f.callbacks == 1, "samples %llu callbacks %llu", static_cast<unsigned long long>(f.samples),
        static_cast<unsigned long long>(f.callbacks));
  CHECK(f.cycleStamp == 5000000 + 10000000, "stamp %llu, want the last frame's time 15000000", static_cast<unsigned long long>(f.cycleStamp));
}

void testNoAllocationInTheCallback() {
  Core& core = freshCore();
  TunerRender render(core, 1, OutMode::Thru, kRate);
  auto in = deviceInput(4, 1, 110.0, 1.0);
  std::vector<float> out(in.size(), 0.0f);
  gNews = 0;
  gArmed = true;
  feed(render, in, {4, 4, 512}, &out);
  gArmed = false;
  CHECK(gNews == 0, "the audio callback allocated %ld times", gNews);
  gNews = 0;  // negative control for the counter itself: an allocation while armed must be seen
  gArmed = true;
  delete new int(1);
  gArmed = false;
  CHECK(gNews == 1, "the allocation counter did not count (%ld)", gNews);
}

void testGatherScatterEdges() {
  float dst[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  CHECK(audio::gatherChannel({}, 0, 0, dst, 8) == 0, "gather from nothing");
  CHECK(audio::framesOf({}) == 0 && audio::channelsOf({}) == 0, "empty list");
  float data[6] = {1, 2, 3, 4, 5, 6};  // 3 frames of 2 channels
  Chunk c{2, sizeof data, data};
  CHECK(audio::gatherChannel({&c, 1}, 1, 1, dst, 8) == 2 && dst[0] == 4 && dst[1] == 6, "gather offset 1 channel 1: %g %g", dst[0], dst[1]);
  CHECK(audio::gatherChannel({&c, 1}, 0, 3, dst, 8) == 0, "gather past the end");
  Chunk empty{2, 8, nullptr};
  audio::clearAll({&empty, 1});  // a stream with no data pointer is skipped, not dereferenced
}

void testOptions() {
  char prog[] = "squatch-tuner-live", dev[] = "--device", name[] = "umc", list[] = "--list-devices", in[] = "--input", two[] = "2";
  char* withDevice[] = {prog, dev, name, in, two};
  Options o;
  CHECK(parseArgs(5, withDevice, o, true) && std::string_view(o.device) == "umc" && o.input == 2, "--device and --input");
  Options jack;
  CHECK(!parseArgs(5, withDevice, jack, false), "the JACK host must refuse --device");
  char* withList[] = {prog, list};
  Options l;
  CHECK(parseArgs(2, withList, l, true) && l.listDevices, "--list-devices");
  Options l2;
  CHECK(!parseArgs(2, withList, l2, false), "the JACK host must refuse --list-devices");
  char a4[] = "--a4", bad[] = "460";
  char* badA4[] = {prog, a4, bad};
  Options b;
  CHECK(!parseArgs(3, badA4, b, true), "--a4 460 is out of range");
}

}  // namespace

int main() {
  testPitchThroughTheHostLayer();
  testA4Reference();
  testChannelPick();
  testOutputs();
  testOneStreamPerChannel();
  testSameDspAsTheFilePath();
  testStampAndCounters();
  testNoAllocationInTheCallback();
  testGatherScatterEdges();
  testOptions();
  std::printf(gFailures ? "host audio: %d failure(s)\n" : "host audio: all tests passed\n", gFailures);
  return gFailures ? 1 : 0;
}
