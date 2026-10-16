// SPDX-License-Identifier: MIT
// Block interface v2, for every registered block: descriptors from the contracts (ids, keys,
// roles, tapers), get() after set(), the heap-free factory, channel layouts (a linked stereo path
// fed the same signal on both sides must equal the mono path bit for bit), tails and meters.
#include <cstring>
#include <squatch/dsp/registry.hpp>

#include "check.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

constexpr double kRate = 48000.0;
alignas(kBlockAlign) unsigned char gAt[kBlockSize];
alignas(kBlockAlign) unsigned char gAt2[kBlockSize];
alignas(64) unsigned char gMem[1 << 20];
alignas(64) unsigned char gMem2[1 << 20];

int count(Span<const ParamDesc> ps, Role role) {
  int n = 0;
  for (const ParamDesc& p : ps) n += p.role == role;
  return n;
}

bool uniqueKeysAndIds(Span<const ParamDesc> ps) {
  for (std::size_t i = 0; i < ps.size(); ++i)
    for (std::size_t j = i + 1; j < ps.size(); ++j)
      if (ps[i].stableId == ps[j].stableId || sameKey(ps[i].key, ps[j].key)) return false;
  return true;
}

// The contract's order: settable parameters (triggers among them), then the bypass, then meters.
int rank(Role r) { return r == Role::Trigger ? 0 : static_cast<int>(r); }

void checkTable(const BlockType& t) {
  const Span<const ParamDesc> ps = t.params;
  bool dense = true, ordered = true;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    dense = dense && ps[i].id == i && ps[i].key[0] != '\0' && ps[i].stableId != 0;
    ordered = ordered && (i == 0 || rank(ps[i - 1].role) <= rank(ps[i].role));
  }
  CHECK(dense && ordered, "%s: dense ids, keys and stable ids; params, then bypass, then meters", t.info->typeId);
  CHECK(count(ps, Role::Bypass) == 1 && uniqueKeysAndIds(ps), "%s: one bypass, unique keys and ids", t.info->typeId);
  CHECK(std::strncmp(t.info->typeId, "cc.squatch.", 11) == 0 && t.info->stateVersion >= 1, "%s: identity", t.info->typeId);
}

// Every settable parameter starts at its default, and get() returns what set() kept (clamped).
void checkGetSet(Block& b) {
  const char* id = b.info().typeId;
  for (const ParamDesc& p : b.parameters()) {
    if (!p.settable() || p.role == Role::Bypass) {
      if (p.role == Role::Bypass) CHECK(std::isnan(b.get(p.id)), "%s: the bypass is the host's", id);
      continue;
    }
    CHECK(b.get(p.id) == p.def, "%s.%s starts at %g, got %g", id, p.key, p.def, b.get(p.id));
    const float mid = p.denormalize(0.37f);
    b.set(p.id, mid);
    CHECK(b.get(p.id) == p.clamp(mid), "%s.%s get after set", id, p.key);
    b.set(p.id, p.max * 4.0f + 100.0f);
    CHECK(b.get(p.id) == p.max, "%s.%s clamped to max", id, p.key);
    b.set(p.id, p.def);
  }
}

void checkTapers(const BlockType& t) {
  for (const ParamDesc& p : t.params) {
    float worst = 0.0f;
    for (float n = 0.0f; n <= 1.0f; n += 0.125f) {
      const float v = p.denormalize(n);
      worst = std::fmax(worst, p.steps ? std::fabs(p.denormalize(p.normalize(v)) - v) : std::fabs(p.normalize(v) - n));
    }
    CHECK(worst < 1e-4f, "%s.%s: normalize and denormalize are inverses (%g)", t.info->typeId, p.key, worst);
    CHECK(p.denormalize(0.0f) == p.min && p.denormalize(1.0f) == p.max, "%s.%s ends", t.info->typeId, p.key);
  }
}

Block* made(const BlockType& t, unsigned char* at, unsigned char* mem, Layout layout) {
  Block* b = createBlock(t.info->typeId, at, kBlockSize);
  Arena arena(mem, sizeof gMem);
  if (b) b->prepare(Setup{kRate, 256, layout}, arena);
  return b;
}

// Stereo with the same signal on both sides equals mono; MonoToStereo copies mono to the right.
void checkLayouts(const BlockType& t) {
  const std::vector<float> x = sig::noise(4096, 0.5f, 5);
  std::vector<float> mono(x.size()), l(x.size()), r(x.size());
  Block* a = made(t, gAt, gMem, Layout::Mono);
  const Layout wide = a->supports(Layout::Stereo) ? Layout::Stereo : Layout::MonoToStereo;
  Block* b = made(t, gAt2, gMem2, wide);
  for (std::size_t i = 0; i < x.size(); i += 256) {
    a->process(&x[i], &mono[i], 256);
    const float* in[2] = {&x[i], &x[i]};
    float* out[2] = {&l[i], &r[i]};
    b->processChannels(AudioIO{in, out, inputChannels(wide), 2}, 256);
  }
  // A stereoOutput() block (Hall) has its own second output; its first still is the mono path.
  CHECK(l == mono && (b->stereoOutput() || r == mono), "%s: %s with equal sides is the mono path", t.info->typeId,
        wide == Layout::Stereo ? "linked stereo" : "mono to stereo");
  destroyBlock(a), destroyBlock(b);
}

}  // namespace

TEST(interfaceEveryBlockMatchesItsContract) {
  CHECK(blockTypes().size() == kBlockTypes && kBlockTypes >= 5, "the core's five blocks, and any the tree adds");
  for (const BlockType& t : blockTypes()) {
    checkTable(t);
    checkTapers(t);
    Block* b = createBlock(t.info->typeId, gAt, sizeof gAt);
    CHECK(b && &b->info() == t.info && b->parameters().data() == t.params.data(), "%s: instance and type agree",
          t.info->typeId);
    if (b) checkGetSet(*b), destroyBlock(b);
  }
}

TEST(interfaceFactoryRefusesWhatDoesNotFit) {
  CHECK(!createBlock("cc.squatch.nothing", gAt, sizeof gAt), "unknown id");
  CHECK(!createBlock("cc.squatch.gate", gAt, 8), "storage too small");
  CHECK(!createBlock("cc.squatch.gate", gAt + 1, sizeof gAt - 1), "misaligned storage");
  CHECK(!createBlock(nullptr, gAt, sizeof gAt) && !destroyBlock(nullptr), "null");
  CHECK(findBlockType("cc.squatch.tuner")->size <= kBlockSize, "kBlockSize holds the largest block");
}

TEST(interfaceLayouts) {
  for (const BlockType& t : blockTypes()) checkLayouts(t);
}

TEST(interfaceLimiterDriveAndOutput) {
  Block* b = createBlock("cc.squatch.limiter", gAt, sizeof gAt);
  Arena mem(gMem, sizeof gMem);
  b->prepare(Setup{kRate, 256}, mem);
  b->set(Limiter::Drive, 12.0f), b->set(Limiter::Ceiling, -3.0f), b->set(Limiter::Output, -6.0f);
  const std::vector<float> x = sig::sine(440.0, 256 * 188, kRate, 0.5);
  std::vector<float> y(x.size());
  for (std::size_t i = 0; i < x.size(); i += 256) b->process(&x[i], &y[i], 256);
  const float peak = peakAbs(&y[24000], 24000);
  CHECK(std::fabs(gainToDb(peak) - (-9.0f)) < 0.3f, "12 dB into a -3 dB ceiling, -6 dB after: %.2f dB", gainToDb(peak));
  CHECK(b->get(Limiter::Reduction) > 5.0f && b->get(Limiter::Reduction) < 12.0f, "reduction meter %.2f dB",
        b->get(Limiter::Reduction));
  CHECK(b->tail() == b->latency(), "a limiter's tail is its lookahead");
  destroyBlock(b);
}

TEST(interfaceTails) {
  static float ir[3000];
  for (int i = 0; i < 3000; ++i) ir[i] = 0.1f * std::exp(-i / 500.0f);
  Cabinet cab;
  cab.configure(ir, 3000);
  Arena mem(gMem, sizeof gMem);
  cab.prepare(Setup{kRate, 256}, mem);
  Eq eq;
  eq.prepare(Setup{kRate, 256}, mem);
  const int before = eq.tail();
  eq.set(Eq::MidQ, 10.0f), eq.set(Eq::MidFreq, 100.0f);
  CHECK(cab.tail() == 3000 + static_cast<int>(Cabinet::kCutTailSeconds * kRate), "a cabinet rings for its response and its cuts: %d", cab.tail());
  CHECK(before > 0 && eq.tail() > before && eq.tail() < int(kRate), "EQ tail %d then %d samples", before, eq.tail());
}

TEST(interfaceTunerMeters) {
  static TunerBlock t;
  Arena mem(gMem, sizeof gMem);
  t.prepare(Setup{kRate, 64, Layout::Stereo}, mem);
  const double hz = 110.0 * std::exp2(13.0 / 1200.0);  // A2, 13 cents sharp
  const std::vector<float> x = sig::sine(hz, 28800, kRate, 0.3);
  std::vector<float> l(x.size()), r(x.size());
  for (size_t i = 0; i < x.size(); i += 64) {
    const float* in[2] = {&x[i], &x[i]};
    float* out[2] = {&l[i], &r[i]};
    t.processChannels(AudioIO{in, out, 2, 2}, 64);
  }
  CHECK(t.get(TunerBlock::Signal) == 1.0f && t.get(TunerBlock::Note) == 9.0f && t.get(TunerBlock::Octave) == 2.0f,
        "reads A2 from the mid: note %g octave %g", t.get(TunerBlock::Note), t.get(TunerBlock::Octave));
  CHECK(std::fabs(t.get(TunerBlock::Cents) - 13.0f) < 0.05f, "13 cents: %.3f", t.get(TunerBlock::Cents));
  t.set(TunerBlock::A4, 442.0f);
  const float want = 13.0f - 1200.0f * std::log2(442.0f / 440.0f);
  CHECK(std::fabs(t.get(TunerBlock::Cents) - want) < 0.05f, "against A4 = 442 Hz: %.3f, want %.3f",
        t.get(TunerBlock::Cents), want);
  CHECK(l == x && r == x, "unmuted, both sides pass bit for bit");
  CHECK(std::strcmp(TunerBlock::kParams[TunerBlock::Note].label(t.get(TunerBlock::Note)), "A") == 0, "labelled A");
}
