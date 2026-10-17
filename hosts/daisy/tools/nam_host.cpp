// SPDX-License-Identifier: MIT
// The Seed3's A2-Lite kernel (hosts/daisy/nam/a2lite.hpp) built for this machine: reads float32 samples from a file,
// runs them through the model exactly as the board does (the receptive field twice over on silence first, then 32-sample
// blocks), writes float32 samples. nam_check.py uses it to compare the board's output with the same code on the Mac.
//
//   clang++ -std=c++17 -O2 -Iinclude hosts/daisy/tools/nam_host.cpp -o build/nam_host
//   build/nam_host IN.f32 OUT.f32 [BLOCK]
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../nam/a2lite.hpp"

int main(int argc, char** argv) {
  if (argc < 3) return std::fprintf(stderr, "usage: nam_host IN.f32 OUT.f32 [BLOCK]\n"), 2;
  const int block = argc > 3 ? std::atoi(argv[3]) : 32;
  std::FILE* f = std::fopen(argv[1], "rb");
  if (!f) return std::perror(argv[1]), 1;
  std::vector<float> x;
  float v;
  while (std::fread(&v, sizeof v, 1, f) == 1) x.push_back(v);
  std::fclose(f);
  static squatch::nam::A2Lite amp;
  squatch::dsp::Arena none;
  amp.prepare(squatch::dsp::Setup{48000.0, 64}, none);
  std::vector<float> zeros(static_cast<std::size_t>(block)), sink(static_cast<std::size_t>(block));
  for (int i = 0; i < 2 * squatch::nam::A2Lite::receptiveField() / block + 1; ++i) amp.process(zeros.data(), sink.data(), block);
  std::vector<float> y(x.size());
  for (std::size_t i = 0; i < x.size(); i += static_cast<std::size_t>(block))
    amp.process(&x[i], &y[i], static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - i)));
  f = std::fopen(argv[2], "wb");
  if (!f) return std::perror(argv[2]), 1;
  std::fwrite(y.data(), sizeof(float), y.size(), f);
  std::fclose(f);
  return 0;
}
