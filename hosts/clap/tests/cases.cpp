// SPDX-License-Identifier: MIT
#include "cases.hpp"

#include <cmath>
#include <set>
#include <string>

using host::Host;
using host::Plugin;

namespace cases {
namespace {

void checkInfo(const clap_param_info_t& i, std::set<clap_id>& ids, int& bypass) {
  HCHECK(std::isfinite(i.min_value) && std::isfinite(i.max_value) && i.min_value < i.max_value, "%s: range", i.name);
  HCHECK(i.default_value >= i.min_value && i.default_value <= i.max_value, "%s: default in range", i.name);
  HCHECK(ids.insert(i.id).second && i.name[0], "%s: unique id %u and a name", i.name, i.id);
  const bool readonly = i.flags & CLAP_PARAM_IS_READONLY, automatable = i.flags & CLAP_PARAM_IS_AUTOMATABLE;
  HCHECK(readonly != automatable, "%s: a meter is read-only, anything else automatable", i.name);
  if (i.flags & CLAP_PARAM_IS_STEPPED)
    HCHECK(i.min_value == std::round(i.min_value) && i.max_value == std::round(i.max_value), "%s: whole steps", i.name);
  bypass += (i.flags & CLAP_PARAM_IS_BYPASS) != 0;
}

// value -> text -> value lands where it started, for the default and both ends.
void checkText(const Plugin& pl, const clap_param_info_t& i) {
  for (double v : {i.default_value, i.min_value, i.max_value}) {
    char text[64] = {};
    double back = -1e300;
    const bool ok = pl.params->value_to_text(pl.p, i.id, v, text, sizeof text) &&
                    pl.params->text_to_value(pl.p, i.id, text, &back);
    const double tol = 1e-3 * (i.max_value - i.min_value) + 0.05 * std::fabs(v) * (i.flags & CLAP_PARAM_IS_STEPPED ? 0 : 1);
    HCHECK(ok && std::fabs(back - v) <= tol, "%s: %g -> '%s' -> %g", i.name, v, text, back);
  }
}

bool everyExtension(const Plugin& pl) {
  return pl.p && pl.params && pl.state && pl.latency && pl.tail && pl.ports && pl.configs && pl.configurable;
}

}  // namespace

void descriptorAndParams(const Subject& s) {
  const auto* f = static_cast<const clap_plugin_factory_t*>(s.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  HCHECK(f && f->get_plugin_count(f) == 1, "%s: one plugin in the factory", s.key);
  HCHECK(!s.entry->get_factory("clap.nonexistent-factory"), "%s: unknown factory is null", s.key);
  Host h;
  HCHECK(!f->create_plugin(f, &h.clap, "cc.squatch.gate-and-more"), "%s: an id with trailing garbage", s.key);
  Plugin pl(s.entry, h);
  HCHECK(everyExtension(pl), "%s: every extension", s.key);
  if (!pl.params) return;
  std::set<clap_id> ids;
  int bypass = 0;
  for (uint32_t k = 0; k < pl.count(); ++k) {
    const clap_param_info_t i = pl.info(k);
    checkInfo(i, ids, bypass);
    checkText(pl, i);
    HCHECK(pl.value(i.id) == i.default_value, "%s: starts at its default", i.name);
  }
  HCHECK(bypass == 1, "%s: exactly one bypass parameter, found %d", s.key, bypass);
}

// Logic restores before Initialize (clap-wrapper #490): a state loads, and reads back the same,
// with no activate() at all, through streams that move one byte per call.
void stateBeforeActivation(const Subject& s) {
  Host h;
  Plugin a(s.entry, h);
  host::Events ev;
  for (uint32_t k = 0; k < a.count(); ++k) {
    const clap_param_info_t i = a.info(k);
    if (i.flags & CLAP_PARAM_IS_AUTOMATABLE) ev.add(0, i.id, i.min_value + 0.3 * (i.max_value - i.min_value));
  }
  a.params->flush(a.p, &ev.in, &ev.out);
  const std::string saved = a.save(1);
  HCHECK(saved.rfind("squatch-state 1\n", 0) == 0 && saved.find("\nend\n") != std::string::npos, "%s: state text", s.key);
  Plugin b(s.entry, h);
  HCHECK(b.load(saved, 1) && h.rescans == 1, "%s: loads before activate, one byte per read, and asks for a rescan",
         s.key);
  bool same = true;
  for (uint32_t k = 0; k < b.count(); ++k)
    if (!(b.info(k).flags & CLAP_PARAM_IS_READONLY)) same = same && b.value(b.info(k).id) == a.value(b.info(k).id);
  HCHECK(same, "%s: every value survives the round trip", s.key);
  HCHECK(b.save(3) == saved, "%s: saved again (three bytes per write), the same text", s.key);
  HCHECK(!b.load("squatch-state 1\ntype cc.squatch.other 1\nend\n", 7), "%s: refuses another type", s.key);
  HCHECK(!b.load("squatch-state 1\ntype " + std::string(b.p->desc->id) + " 99\nend\n", 7), "%s: refuses a newer state",
         s.key);
  HCHECK(!b.load("", 7) && b.save(64) == saved, "%s: refuses an empty stream and keeps its values", s.key);
}

// The host may send parameter changes while the plugin is inactive (clap-wrapper #538 #551).
void flushWhileInactive(const Subject& s) {
  Host h;
  Plugin pl(s.entry, h);
  const clap_param_info_t first = pl.info(0);
  host::Events ev;
  ev.add(0, first.id, first.max_value);
  pl.params->flush(pl.p, &ev.in, &ev.out);
  HCHECK(pl.value(first.id) == first.max_value, "%s: flush before any activate reaches %s", s.key, first.name);
  HCHECK(pl.p->activate(pl.p, 48000, 1, 256), "%s: activates", s.key);
  pl.p->deactivate(pl.p);
  ev.clear();
  ev.add(0, first.id, first.min_value);
  pl.params->flush(pl.p, &ev.in, &ev.out);
  HCHECK(pl.value(first.id) == first.min_value, "%s: flush after deactivate", s.key);
}

void portConfigurations(const Subject& s) {
  Host h;
  Plugin pl(s.entry, h);
  clap_audio_port_info_t in{}, out{};
  HCHECK(pl.ports->count(pl.p, true) == 1 && pl.ports->get(pl.p, 0, true, &in) && pl.ports->get(pl.p, 0, false, &out),
         "%s: one main input and output", s.key);
  HCHECK(in.channel_count == 2 && out.channel_count == 2 && in.in_place_pair == out.id, "%s: stereo by default", s.key);
  HCHECK(pl.configs->count(pl.p) == 3, "%s: three configurations", s.key);
  const clap_audio_port_configuration_request monoIn{true, 0, 1, CLAP_PORT_MONO, nullptr};
  const clap_audio_port_configuration_request monoOut{false, 0, 1, CLAP_PORT_MONO, nullptr};
  const clap_audio_port_configuration_request stereoOut{false, 0, 2, CLAP_PORT_STEREO, nullptr};
  const clap_audio_port_configuration_request second{true, 1, 1, CLAP_PORT_MONO, nullptr};
  const clap_audio_port_configuration_request mono[] = {monoIn, monoOut}, wide[] = {monoIn, stereoOut};
  HCHECK(pl.configurable->can_apply_configuration(pl.p, mono, 2), "%s: 1 -> 1", s.key);
  HCHECK(pl.configurable->can_apply_configuration(pl.p, wide, 2), "%s: 1 -> 2", s.key);
  HCHECK(!pl.configurable->can_apply_configuration(pl.p, &monoOut, 1), "%s: not 2 -> 1", s.key);
  HCHECK(!pl.configurable->can_apply_configuration(pl.p, &second, 1), "%s: no second port", s.key);
  HCHECK(pl.configurable->apply_configuration(pl.p, mono, 2) && pl.ports->get(pl.p, 0, false, &out) &&
             out.channel_count == 1,
         "%s: applied, the ports say mono", s.key);
  HCHECK(pl.configs->select(pl.p, 1) && pl.ports->get(pl.p, 0, false, &out) && out.channel_count == 2, "%s: select", s.key);
  HCHECK(pl.p->activate(pl.p, 44100, 1, 128), "%s: activates", s.key);
  HCHECK(!pl.configurable->apply_configuration(pl.p, mono, 2) && !pl.configs->select(pl.p, 0), "%s: not while active",
         s.key);
  pl.p->deactivate(pl.p);
}

// clap-wrapper's host-information extension (#560) is read: the plugin says where it runs.
void hostInformation(const Subject& s) {
  Host h;
  h.flavor = CLAP_WRAPPER_HOST_FLAVOR_VST3;
  Plugin pl(s.entry, h);
  HCHECK(h.logged("as vst3") && h.logged("squatch-clap-host"), "%s: reads the wrapper flavour and real host", s.key);
  Host bare;
  Plugin native(s.entry, bare);
  HCHECK(bare.logged("(CLAP)"), "%s: and knows when it is not wrapped", s.key);
}

}  // namespace cases
