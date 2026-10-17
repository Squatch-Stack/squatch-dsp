// SPDX-License-Identifier: MIT
// The negative control for tools/heapcheck: an image whose audio callback allocates, but only
// behind the Block interface's virtual call, the way a careless block would. heapcheck must FAIL
// this image (`make -C hosts/daisy heapcheck-control`); if it passes, the check is blind.
#include <squatch/dsp/block.hpp>

namespace {

using namespace squatch::dsp;

class Careless final : public Block {
 public:
  void prepare(const Setup&, Arena&) override {}
  void process(const float* in, float* out, int n) override {
    float* scratch = new float[static_cast<unsigned>(n)];  // exactly what process() must never do
    for (int i = 0; i < n; ++i) scratch[i] = in[i];
    for (int i = 0; i < n; ++i) out[i] = scratch[i];
    delete[] scratch;
  }
  void reset() override {}
  void set(ParamId, float) override {}
  float get(ParamId) const override { return 0.0f; }
};

Careless careless;
Block* volatile chain = &careless;

void callback(const float* const* in, float** out, unsigned size) { chain->process(in[0], out[0], static_cast<int>(size)); }

}  // namespace

void (*volatile gCallback)(const float* const*, float**, unsigned) = callback;

int main() {
  static float a[32], b[32];
  const float* in[1] = {a};
  float* out[1] = {b};
  gCallback(in, out, 32);
  return 0;
}
