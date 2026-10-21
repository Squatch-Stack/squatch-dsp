// SPDX-License-Identifier: MIT
// The FFT against a double-precision DFT, and the cabinet against direct convolution in double:
// every response length around the partition boundaries, several partitions, random call sizes
// and in-place calls. Plus: no latency, pass-through without a response, silence without
// memory, a smoothed level, and a golden render.
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/fft.hpp>

#include "check.hpp"
#include "glitch.hpp"
#include "golden.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

#if __has_include("golden/cabinet.inc")
static const double kGolden[golden::kSegments] =
#include "golden/cabinet.inc"
    ;
static const double* const kGoldenRender = kGolden;
#else
static const double* const kGoldenRender = nullptr;
#endif

namespace {

constexpr double kRate = 48000.0;
alignas(64) unsigned char gMem[1 << 22];

// A cabinet-like response: a decaying noise burst with a strong first tap.
std::vector<float> response(int length, std::uint32_t seed) {
  std::vector<float> h = sig::noise(length, 0.5f, seed);
  for (int i = 0; i < length; ++i) h[i] *= static_cast<float>(std::exp(-6.0 * i / length));
  h[0] = 0.8f;
  return h;
}

std::vector<double> convolve(const std::vector<float>& x, const std::vector<float>& h) {
  std::vector<double> y(x.size(), 0.0);
  for (std::size_t t = 0; t < x.size(); ++t)
    for (std::size_t k = 0; k < h.size() && k <= t; ++k) y[t] += double(h[k]) * x[t - k];
  return y;
}

// The block over x in random call sizes up to maxBlock, alternately in place.
std::vector<float> run(Cabinet& cab, const std::vector<float>& x, int maxBlock, std::uint32_t seed) {
  sig::Rng rng{seed};
  std::vector<float> y(x.size()), scratch(static_cast<std::size_t>(maxBlock));
  for (std::size_t done = 0; done < x.size();) {
    const int n = std::min<int>(rng.range(1, maxBlock), static_cast<int>(x.size() - done));
    if (rng.next() % 2) {
      copy(&x[done], &y[done], n);
      cab.process(&y[done], &y[done], n);
    } else {
      cab.process(&x[done], &y[done], n);
    }
    done += static_cast<std::size_t>(n);
  }
  return y;
}

double worstError(const std::vector<float>& y, const std::vector<double>& ref) {
  double worst = 0.0, peak = 0.0;
  for (std::size_t i = 0; i < y.size(); ++i) worst = std::fmax(worst, std::fabs(y[i] - ref[i])), peak = std::fmax(peak, std::fabs(ref[i]));
  return worst / peak;
}

void dft(const std::vector<double>& re, const std::vector<double>& im, std::vector<double>& outRe, std::vector<double>& outIm) {
  const std::size_t n = re.size();
  outRe.assign(n, 0.0), outIm.assign(n, 0.0);
  for (std::size_t k = 0; k < n; ++k)
    for (std::size_t t = 0; t < n; ++t) {
      const double a = -2.0 * sig::kPi * double((k * t) % n) / double(n);
      outRe[k] += re[t] * std::cos(a) - im[t] * std::sin(a);
      outIm[k] += re[t] * std::sin(a) + im[t] * std::cos(a);
    }
}

double fftError(int size) {
  Arena mem(gMem, sizeof gMem);
  Fft fft;
  if (!fft.prepare(size, mem)) return 1e9;
  std::vector<float> re = sig::noise(size, 1.0f, 5u + size), im = sig::noise(size, 1.0f, 9u + size);
  std::vector<double> dRe(re.begin(), re.end()), dIm(im.begin(), im.end()), wantRe, wantIm;
  dft(dRe, dIm, wantRe, wantIm);
  fft.forward(re.data(), im.data());
  double worst = 0.0;
  for (int k = 0; k < size; ++k) worst = std::fmax(worst, std::hypot(re[k] - wantRe[k], im[k] - wantIm[k]));
  fft.inverse(re.data(), im.data());
  for (int t = 0; t < size; ++t) worst = std::fmax(worst, std::hypot(re[t] / size - dRe[t], im[t] / size - dIm[t]));
  return worst / std::sqrt(double(size));
}

}  // namespace

TEST(fftMatchesDft) {
  for (int size = 2; size <= 1024; size *= 2) {
    const double e = fftError(size);
    CHECK(e < 1e-6, "size %d: error %.3g", size, e);
  }
  Arena mem(gMem, sizeof gMem);
  Fft fft;
  CHECK(!fft.prepare(96, mem) && !fft.prepare(1, mem) && !fft.ready(), "not a power of two: refused");
}

TEST(cabinetMatchesDirectConvolution) {
  static Cabinet cab;
  const std::vector<float> x = sig::noise(6000, 0.7f, 17);
  const int lengths[] = {1, 3, 15, 16, 17, 64, 65, 127, 128, 129, 500, 2400, 4801};
  double worst = 0.0;
  for (int length : lengths) {
    const std::vector<float> h = response(length, 31u + length);
    const std::vector<double> ref = convolve(x, h);
    for (int partition : {4, 16, 64, 256}) {
      Arena mem(gMem, sizeof gMem);
      cab.configure(h.data(), length, partition, false);  // exact convolution: no cuts
      cab.prepare(Setup{kRate, 128}, mem);
      const double e = worstError(run(cab, x, 128, 3u + length), ref);
      CHECK(cab.ready() && e < 2e-6, "partition %d, length %d: worst error %.3g of peak", partition, length, e);
      worst = std::fmax(worst, e);
    }
  }
  std::printf("     cabinet vs direct convolution in double: worst error %.3g of peak\n", worst);
}

TEST(cabinetHasNoLatency) {
  static Cabinet cab;
  const std::vector<float> h = response(1000, 4);
  Arena mem(gMem, sizeof gMem);
  cab.configure(h.data(), 1000, Cabinet::kDefaultPartition, false);
  cab.prepare(Setup{kRate, 64}, mem);
  std::vector<float> x(1200, 0.0f);
  x[0] = 1.0f;
  const std::vector<float> y = run(cab, x, 64, 8);
  double worst = 0.0;
  for (int i = 0; i < 1200; ++i) worst = std::fmax(worst, std::fabs(y[i] - (i < 1000 ? h[i] : 0.0f)));
  CHECK(cab.latency() == 0 && worst < 1e-6, "an impulse returns the response from sample 0 (error %.3g)", worst);
}

TEST(cabinetWithoutResponsePassesThrough) {
  static Cabinet cab;
  Arena mem(gMem, sizeof gMem);
  cab.configure(nullptr, 0);
  cab.prepare(Setup{kRate, 64}, mem);
  const std::vector<float> x = sig::noise(640, 0.5f, 6);
  const std::vector<float> y = run(cab, x, 64, 2);
  CHECK(y == x && cab.length() == 1, "no response: the input, bit for bit");
  const float h[3] = {1.0f, 0.5f, 0.25f};
  cab.configure(h, 3, 48);
  cab.prepare(Setup{kRate, 64}, mem);
  CHECK(cab.length() == 1, "a partition that is not a power of two is refused");
}

TEST(cabinetStaysSilentWithoutMemory) {
  static Cabinet cab;
  const std::vector<float> h = response(3000, 5);
  cab.configure(h.data(), 3000);
  Arena measure;
  cab.prepare(Setup{kRate, 64}, measure);
  CHECK(!cab.ready() && measure.required() > 3000 * 4 * sizeof(float), "needs %zu bytes", measure.required());
  Arena small(gMem, measure.required() / 2);
  cab.prepare(Setup{kRate, 64}, small);
  std::vector<float> x = sig::noise(256, 0.5f, 7);
  cab.process(x.data(), x.data(), 256);
  CHECK(!cab.ready() && sig::peak(x) == 0.0f, "out of memory: silence");
  Arena exact(gMem, measure.required());
  cab.prepare(Setup{kRate, 64}, exact);
  CHECK(cab.ready(), "the measured size is enough");
}

TEST(cabinetLevelIsSmoothed) {
  static Cabinet cab;
  Arena mem(gMem, sizeof gMem);
  cab.configure(nullptr, 0);
  cab.prepare(Setup{kRate, 64}, mem);
  cab.set(Cabinet::Level, -6.0f);
  std::vector<float> x(2400, 1.0f);
  cab.process(x.data(), x.data(), 2400);
  float jump = 0.0f;
  for (int i = 1; i < 2400; ++i) jump = std::fmax(jump, std::fabs(x[i] - x[i - 1]));
  CHECK(std::fabs(x[2399] - 0.501187f) < 1e-4f && jump < 1e-3f, "reaches -6 dB (%.5f) in steps of at most %.5f", x[2399], jump);
  cab.set(Cabinet::Level, NAN);
  cab.process(x.data(), x.data(), 1);
}

TEST(cabinetGolden) {
  static Cabinet cab;
  const std::vector<float> h = response(2400, 12);
  Arena mem(gMem, sizeof gMem);
  cab.configure(h.data(), 2400, Cabinet::kDefaultPartition, false);
  cab.prepare(Setup{kRate, 64}, mem);
  std::vector<float> x = sig::noise(16384, 0.3f, 13);
  const std::vector<float> tone = sig::sine(110.0, 16384, kRate, 0.8);
  x.insert(x.end(), tone.begin(), tone.end());
  const std::vector<float> y = run(cab, x, 64, 14);
  golden::check("cabinet", y, kGoldenRender, 1e-5);
}

TEST(cabinetLevelChangePassesTheGlitchOracle) {
  static Cabinet cab;
  const std::vector<float> h = response(480, 3);
  Arena mem(gMem, sizeof gMem);
  cab.configure(h.data(), 480);
  cab.prepare(Setup{kRate, 64}, mem);
  std::vector<float> x = sig::sine(196.0, 24000, kRate, 0.4);
  const std::vector<float> hiss = sig::noise(24000, 0.1f, 6);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] += hiss[i];
  const std::vector<float> loud = run(cab, x, 64, 5);  // the oracle before: 0 dB, and the cabinet is linear,
  std::vector<float> soft(loud);                       // so the oracle after is the same signal at -6 dB
  for (float& v : soft) v *= 0.501187f;
  cab.reset();
  std::vector<float> y(x.size());
  cab.process(x.data(), y.data(), 6000);
  cab.set(Cabinet::Level, -6.0f);
  cab.process(x.data() + 6000, y.data() + 6000, static_cast<int>(x.size()) - 6000);
  const glitch::Report r = glitch::analyse(y, loud, soft, {{6000, 6000 + 9600}});
  CHECK(r.ok && r.error.empty(), "the level ramp is clean: %s%s", r.error.c_str(), r.findings.empty() ? "" : r.findings[0].c_str());
  // Negative control: the same change without the smoother (a jump at the same place), on a pure tone.
  const std::vector<float> tone = sig::sine(196.0, 24000, kRate, 0.5);
  std::vector<float> a(tone), b(tone), c(tone);
  for (std::size_t i = 0; i < tone.size(); ++i) b[i] *= 0.501187f, c[i] = i < 6000 ? a[i] : b[i];
  CHECK(glitch::analyse(c, a, b, {{6000, 6000 + 9600}}).failed("continuity"), "an unsmoothed level jump is a continuity finding");
}
