// SPDX-License-Identifier: MIT
// seed3-notegen: a known note on AUDIO OUT 2 (pin 19, OUT R) for testing a hardware tuner. The tuner under test reads the
// note through its ADC and must say the same note and cents. The inputs
// are ignored and AUDIO OUT 1 is silent.
//
// The note steps through the open strings, 4 s each: E2 82.407 Hz, A2 110.000, D3 146.832, G3 195.998, B3 246.942,
// E4 329.628, and a detuned A2 (110 Hz x 2^(+20/1200), +20 cents) so the tuner's cents reading is tested away from zero.
// Each note is a string-like sum of 6 harmonics (1, .6, .35, .2, .12, .07) at a total peak of SQUATCH_NOTE_AMP
// (default 0.25 = -12 dBFS), a steady tone: the tuner needs a stable pitch, not a decay. Every second the Daisy prints
// the note name, its exact frequency and the cents from concert pitch (A = 440 Hz) over USB serial, so the Mac can log both sides.
//
// Variants: SQUATCH_BLOCK as usual. SQUATCH_NOTE_SECONDS sets the time per note.
#include <cmath>

#include "../common/seed3.hpp"

#ifndef SQUATCH_NOTE_AMP_MILLI
#define SQUATCH_NOTE_AMP_MILLI 250
#endif
#ifndef SQUATCH_NOTE_SECONDS
#define SQUATCH_NOTE_SECONDS 4
#endif

namespace {

using namespace squatch::board;

struct Note {
  const char* name;
  double hz;
  double cents;  // from the nearest equal-tempered note, for the report
};

// Open strings in tune (equal temperament, concert A = 440 Hz), then A2 + 20 cents.
const Note kNotes[] = {
    {"E2", 82.406889, 0.0},  {"A2", 110.0, 0.0},      {"D3", 146.832384, 0.0}, {"G3", 195.997718, 0.0},
    {"B3", 246.941651, 0.0}, {"E4", 329.627557, 0.0}, {"A2+20c", 111.2781, 20.0},
};
constexpr int kNoteCount = sizeof kNotes / sizeof kNotes[0];
constexpr float kHarm[] = {1.0f, 0.6f, 0.35f, 0.2f, 0.12f, 0.07f};
constexpr int kHarmonics = sizeof kHarm / sizeof kHarm[0];
constexpr int kNoteSamples = SQUATCH_NOTE_SECONDS * kSampleRate;

::daisy::DaisySeed hw;
UsbSerial serial;
Meter<2> meter;

// Audio thread state. The phase is a 32-bit fraction of a cycle, so the pitch is exact to ~1e-5 Hz and never drifts.
std::uint32_t phase = 0, inc = 0;
int samplesLeft = kNoteSamples;
int noteIndex = 0;
float norm = 0.0f;
volatile int playing = 0;  // read by the loop to print the note

std::uint32_t incrementFor(double hz) {
  return static_cast<std::uint32_t>(std::llround(hz / kSampleRate * 4294967296.0));
}

float sample() {
  // Six sinf per sample: about 600 cycles of the 10,000 a sample has at 480 MHz.
  const float x = static_cast<float>(phase) * (6.28318530718f / 4294967296.0f);
  float sum = 0.0f;
  for (int h = 0; h < kHarmonics; ++h) sum += kHarm[h] * std::sin(x * static_cast<float>(h + 1));
  phase += inc;
  return sum * norm;
}

void nextNote() {
  noteIndex = (noteIndex + 1) % kNoteCount;
  inc = incrementFor(kNotes[noteIndex].hz);
  samplesLeft = kNoteSamples;
  playing = noteIndex;
}

void callback(::daisy::AudioHandle::InputBuffer, ::daisy::AudioHandle::OutputBuffer out, size_t size) {
  const std::uint32_t start = Dwt::now();
  float peak = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    const float s = sample();
    out[0][i] = 0.0f;
    out[1][i] = s;
    peak = std::fmax(peak, std::fabs(s));
    if (--samplesLeft == 0) nextNote();
  }
  meter.endBlock(start, Dwt::now() - start, 0.0f, peak);
}

}  // namespace

int main() {
  const BoardInfo board = boot(hw, serial);
  meter.configure(board.budget, board.blocksPerSecond);
  float peakSum = 0.0f;
  for (int h = 0; h < kHarmonics; ++h) peakSum += kHarm[h];  // the harmonics can line up: this bounds the peak
  norm = (SQUATCH_NOTE_AMP_MILLI / 1000.0f) / peakSum;
  inc = incrementFor(kNotes[0].hz);
  hw.StartAudio(callback);
  Window<2> w;
  std::uint32_t ungated = 0;
  for (;;) {
    ungated += ungateUsbPhyClock() ? 1 : 0;
    serial.serviceHost();
    heartbeat(hw, false);
    if (!meter.take(w)) continue;
    if (w.seq % 10 == 1) banner(serial, board);
    const Note& n = kNotes[playing];
    const long mhz = std::lround(n.hz * 1000.0), tenths = std::lround(n.cents * 10.0);
    serial.line("# note %s %ld.%03ld Hz (%+ld.%ld cents from the nearest note) on OUT R at %d/1000 FS", n.name, mhz / 1000,
                mhz % 1000, tenths / 10, (tenths < 0 ? -tenths : tenths) % 10, SQUATCH_NOTE_AMP_MILLI);
    reportCommon(serial, w, ungated);
  }
}
