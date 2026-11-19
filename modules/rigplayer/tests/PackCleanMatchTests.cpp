// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fitPackCleanMatches — the clean match a pack carries (namz 5.2's `files[].clean_match`), fitted for every file entry
// of a NAM stage:
//   · cleanMatchReadable is namz's rule: a match it reads is one namz's writer writes and its loader reads back, and no
//     other is;
//   · every entry of a stage of NAM's example models gets a readable match — measureCleanMatch's where it states one —
//     and two entries of one file the same one;
//   · where the fit beats nothing (a capture that is a delay, stated right) the entry states what the fit started from:
//     its input_lag_samples and polarity, or its lag_samples against the first file, and no corners;
//   · refused, naming the file, with no entry of the stage changed: a start lag before the DI where the fit beats nothing,
//     bytes that do not load, no bytes at all, an entry with no file, no source;
//   · a match already on the entries is not a starting point: a second run, and a run over planted matches, give the
//     same matches;
//   · the fitted stage round-trips through namz's writer and loader with the same matches.

#include <felitronics_test.h>

#include <felitronics/core/Math.h>
#include <felitronics/rigplayer/PackCleanMatch.h>
#include <namz_rig_load.h>
#include <namz_rig_write.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using felitronics::test::group;
using namespace felitronics::rigplayer;
namespace core = felitronics::core;
using PackMatch = namz::rig::FileEntry::CleanMatch;

namespace {

// The checks here carry their numbers in the message; FELITRONICS_TEST_VERBOSE=1 prints the passing ones as well.
void ok(bool cond, const std::string& msg) {
    felitronics::test::ok(cond, msg);
    if (cond && std::getenv("FELITRONICS_TEST_VERBOSE") != nullptr) std::printf("    ok: %s\n", msg.c_str());
}

std::string gainModel(double w) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
        R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":1,"bias":false,"implementation":"direct"},"weights":[%.6f],"sample_rate":48000})", w);
    return buf;
}

// A capture that is a delay of `d` samples and a gain of `w`: its low end does not turn, so a lag and a sign are its
// whole phase, and the fit beats nothing on a start that states them.
std::string delayModel(int d, double w) {
    std::string s = "[";
    for (int i = 0; i < d; ++i) s += "0.0,";
    s += std::to_string(w) + "]";
    return R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":)" + std::to_string(d + 1)
         + R"(,"bias":false,"implementation":"direct"},"weights":)" + s + R"(,"sample_rate":48000})";
}

std::string fileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}
std::string exampleModel(const char* name) { return fileText(std::string(FELITRONICS_NAM_EXAMPLE_MODELS) + "/" + name); }

std::vector<std::byte> bytesOf(const std::string& s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return { p, p + s.size() };
}

// One file entry of the stage: its file, where it sits on the gain dial, and the readings it states (none = not stated).
struct Entry { std::string id, at; std::optional<int> inputLag, polarity, lag; };

namz::rig::Stage stageOf(const std::vector<Entry>& entries, double inputDb = 0.0) {
    namz::rig::Stage st; st.kind = namz::rig::StageKind::Nam; st.rawKind = "nam"; st.slot = "pedal"; st.inputDb = inputDb;
    namz::rig::Control g; g.name = "gain"; g.role = namz::rig::Role::Gain; g.sweep = 300;
    for (const auto& e : entries) {
        g.values.push_back(e.at);
        namz::rig::FileEntry fe; fe.id = e.id; fe.settings = { { "gain", e.at } };
        fe.inputLagSamples = e.inputLag; fe.polarity = e.polarity; fe.lagSamples = e.lag;
        st.device.files.push_back(fe);
    }
    st.device.controls = { g };
    return st;
}

struct Files {
    std::map<std::string, std::vector<std::byte>> bytes;
    ModelSource source() const {
        return [this](const std::string& id) {
            const auto b = bytes.find(id);
            return b == bytes.end() ? std::vector<std::byte> {} : b->second;
        };
    }
};

PackMatch packMatch(int lag, int polarity, std::vector<double> hz) {
    PackMatch m;
    m.lagSamples = lag; m.polarity = polarity; m.allpassHz = std::move(hz);
    return m;
}
// measureCleanMatch's match as a pack states it: its corners that are sections, in order.
PackMatch asPack(const CleanMatch& c) {
    PackMatch m;
    m.lagSamples = c.lag; m.polarity = c.inverted ? -1 : 1;
    for (const double hz : c.allpassHz) if (hz > 0.0) m.allpassHz.push_back(hz);
    return m;
}

bool same(const std::optional<PackMatch>& a, const std::optional<PackMatch>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (! a) return true;
    if (a->lagSamples != b->lagSamples || a->polarity != b->polarity || a->allpassHz.size() != b->allpassHz.size()) return false;
    for (std::size_t k = 0; k < a->allpassHz.size(); ++k)
        if (! core::exactlyEqual(a->allpassHz[k], b->allpassHz[k])) return false;
    return true;
}
std::string spell(const std::optional<PackMatch>& m) {
    if (! m) return "none";
    std::string s = "lag " + std::to_string(m->lagSamples) + ", polarity " + std::to_string(m->polarity) + ", corners [";
    for (std::size_t k = 0; k < m->allpassHz.size(); ++k) {
        char b[32]; std::snprintf(b, sizeof b, "%s%.4f", k > 0 ? ", " : "", m->allpassHz[k]); s += b;
    }
    return s + "]";
}

// Every entry's match as it is in both stages, bit for bit — the whole of what the fit may write.
bool sameMatches(const namz::rig::Stage& a, const namz::rig::Stage& b) {
    if (a.device.files.size() != b.device.files.size()) return false;
    for (std::size_t i = 0; i < a.device.files.size(); ++i)
        if (! same(a.device.files[i].cleanMatch, b.device.files[i].cleanMatch)) return false;
    return true;
}
std::string spellAll(const namz::rig::Stage& st) {
    std::string s;
    for (const auto& f : st.device.files) s += (s.empty() ? "" : "; ") + f.id + ": " + spell(f.cleanMatch);
    return s;
}

bool names(const std::string& why, const std::string& file) { return why.find(file) != std::string::npos; }

// ------------------------------------------------------------------------------------------------------------------

void readableIsNamzRule() {
    group("cleanMatchReadable is namz's rule: what it reads namz writes and reads back, and nothing else");
    const double under = std::nextafter(namz::rig::kMinCleanMatchHz, 0.0);
    const struct { const char* what; PackMatch m; bool readable; } cases[] = {
        { "lag 0, no corner",                     packMatch(0, 1, {}),                                       true },
        { "the edges: kMaxLagSamples, kMinCleanMatchHz", packMatch(namz::rig::kMaxLagSamples, -1, { namz::rig::kMinCleanMatchHz }), true },
        { "two corners",                          packMatch(25, -1, { 39.03, 132.2 }),                       true },
        { "a lag before the DI (-1)",             packMatch(-1, 1, {}),                                      false },
        { "a lag past kMaxLagSamples",            packMatch(namz::rig::kMaxLagSamples + 1, 1, {}),           false },
        { "a polarity of 0",                      packMatch(25, 0, {}),                                      false },
        { "a polarity of 2",                      packMatch(25, 2, {}),                                      false },
        { "three corners",                        packMatch(30, 1, { 40.0, 80.0, 160.0 }),                   false },
        { "a corner under kMinCleanMatchHz",      packMatch(12, 1, { 40.0, under }),                         false },
        { "a corner that is infinite",            packMatch(12, 1, { std::numeric_limits<double>::infinity() }), false },
        { "a corner that is NaN",                 packMatch(12, 1, { std::numeric_limits<double>::quiet_NaN() }), false },
    };
    for (const auto& c : cases) {
        auto st = stageOf({ { "a.nam", "60", 12, 1, std::nullopt } });
        st.device.files[0].cleanMatch = c.m;
        namz::rig::Rig rig; rig.chain = { st };
        bool loaded = false;
        const auto back = namz::rig::loadRigManifest(namz::rig::writeManifest(rig), &loaded);
        const bool read = loaded && back.chain.size() == 1 && back.chain[0].device.files.size() == 1
                       && back.chain[0].device.files[0].cleanMatch.has_value();
        ok(cleanMatchReadable(c.m) == c.readable && read == c.readable,
           std::string(c.what) + ": cleanMatchReadable " + (cleanMatchReadable(c.m) ? "yes" : "no") + ", through namz's writer "
           "and loader " + (read ? "read" : "dropped") + ", want " + (c.readable ? "read" : "dropped"));
    }
}

void everyEntryMatched() {
    group("every entry of a stage of NAM's example models gets a readable match; two entries of one file the same one");
    Files files;
    files.bytes["a2.nam"] = bytesOf(exampleModel("A2.nam"));
    files.bytes["wavenet.nam"] = bytesOf(exampleModel("wavenet.nam"));
    ok(files.bytes["a2.nam"].size() > 100 && files.bytes["wavenet.nam"].size() > 100, "precondition: NAM's A2.nam and wavenet.nam");
    auto st = stageOf({ { "a2.nam", "60", std::nullopt, std::nullopt, std::nullopt },
                        { "wavenet.nam", "150", std::nullopt, std::nullopt, std::nullopt },
                        { "a2.nam", "240", std::nullopt, std::nullopt, std::nullopt } }, -6.0);
    st.device.files[2].inputDb = 4.0;             // the second entry of a file is not fitted again: the first's level stands
    const auto bare = st;
    const auto why = fitPackCleanMatches(st, files.source());
    ok(why.empty(), "fitted: \"" + why + "\"");
    bool all = true;
    for (const auto& f : st.device.files) all = all && f.cleanMatch && cleanMatchReadable(*f.cleanMatch);
    ok(all, "every entry has a match the format reads: " + spellAll(st));
    ok(same(st.device.files[0].cleanMatch, st.device.files[2].cleanMatch), "a2.nam's two entries carry one match: "
       + spell(st.device.files[0].cleanMatch) + " and " + spell(st.device.files[2].cleanMatch));
    // The fit is measureCleanMatch's, over the whole stage at once: where it states a match, that is the entry's.
    const auto found = measureCleanMatch(bare.device, files.source(), AlignmentTable::fromDevice(bare.device), bare.inputDb);
    for (const char* id : { "a2.nam", "wavenet.nam" }) {
        const auto hit = found.find(id);
        ok(hit != found.end() && same(st.device.files[std::string(id) == "a2.nam" ? 0 : 1].cleanMatch, asPack(hit->second)),
           std::string(id) + ": measureCleanMatch states " + (hit == found.end() ? "none" : spell(asPack(hit->second)))
           + ", the stage carries " + spell(st.device.files[std::string(id) == "a2.nam" ? 0 : 1].cleanMatch));
    }
}

void fallbackIsTheStart() {
    group("where the fit beats nothing the entry states what it started from: its own reading, no corners");
    Files files;
    files.bytes["on"] = bytesOf(delayModel(12, 1.0));
    files.bytes["inv"] = bytesOf(delayModel(20, -1.0));
    auto st = stageOf({ { "on", "60", 12, 1, std::nullopt }, { "inv", "240", 20, -1, std::nullopt } });
    const auto found = measureCleanMatch(st.device, files.source(), AlignmentTable::fromDevice(st.device));
    ok(found.empty(), "precondition: measureCleanMatch states no match on two delays stated right ("
       + std::to_string(found.size()) + ")");
    const auto why = fitPackCleanMatches(st, files.source());
    ok(why.empty() && same(st.device.files[0].cleanMatch, packMatch(12, 1, {}))
       && same(st.device.files[1].cleanMatch, packMatch(20, -1, {})),
       "input_lag_samples and polarity stated: " + spellAll(st) + (why.empty() ? "" : " (\"" + why + "\")"));

    // A pack with only `lag_samples`: the start is the lag against the first file, which the player taps by.
    Files rel;
    rel.bytes["a"] = bytesOf(gainModel(1.0));
    rel.bytes["b"] = bytesOf(delayModel(9, 1.0));
    auto r = stageOf({ { "a", "60", std::nullopt, std::nullopt, 0 }, { "b", "240", std::nullopt, std::nullopt, 9 } });
    const auto relFound = measureCleanMatch(r.device, rel.source(), AlignmentTable::fromDevice(r.device));
    ok(relFound.empty(), "precondition: measureCleanMatch states no match on a wire and a delay by their lag_samples ("
       + std::to_string(relFound.size()) + ")");
    const auto relWhy = fitPackCleanMatches(r, rel.source());
    ok(relWhy.empty() && same(r.device.files[0].cleanMatch, packMatch(0, 1, {})) && same(r.device.files[1].cleanMatch, packMatch(9, 1, {})),
       "lag_samples alone: " + spellAll(r) + (relWhy.empty() ? "" : " (\"" + relWhy + "\")"));
}

void refusals() {
    group("refused, naming the file, and no entry of the stage changed");
    Files files;
    files.bytes["d"] = bytesOf(delayModel(12, 1.0));
    files.bytes["early"] = bytesOf(gainModel(1.0));
    files.bytes["a2.nam"] = bytesOf(exampleModel("A2.nam"));
    files.bytes["broken.nam"] = bytesOf("not a model");
    const auto planted = packMatch(3, -1, { 50.0 });

    // A start before the DI where the fit beats nothing: the fit starts on time (it never taps early), and a wire is on
    // time — so the start, -5, would be the match, and the format reads no lag below 0.
    auto st = stageOf({ { "d", "60", 12, 1, std::nullopt }, { "early", "240", -5, 1, std::nullopt } });
    st.device.files[0].cleanMatch = planted;
    const auto before = st;
    const auto found = measureCleanMatch(st.device, files.source(), AlignmentTable::fromDevice(st.device));
    ok(found.count("early") == 0, "precondition: measureCleanMatch states no match on the wire stated -5 samples late");
    const auto why = fitPackCleanMatches(st, files.source());
    ok(names(why, "early") && sameMatches(st, before), "a start lag of -5 where the fit beats nothing: \"" + why
       + "\", the stage as it came (" + spellAll(st) + ")");

    // Bytes that do not load, after a file that was fitted: nothing of that fit is written either.
    auto bad = stageOf({ { "a2.nam", "60", std::nullopt, std::nullopt, std::nullopt },
                         { "broken.nam", "150", std::nullopt, std::nullopt, std::nullopt },
                         { "a2.nam", "240", std::nullopt, std::nullopt, std::nullopt } });
    bad.device.files[1].cleanMatch = planted;
    const auto badBefore = bad;
    const auto badWhy = fitPackCleanMatches(bad, files.source());
    ok(names(badWhy, "broken.nam") && names(badWhy, "does not load") && sameMatches(bad, badBefore),
       "bytes that do not load: \"" + badWhy + "\", the stage as it came (" + spellAll(bad) + ")");

    // No bytes at all, an entry with no file, no source.
    auto missing = stageOf({ { "d", "60", 12, 1, std::nullopt }, { "gone.nam", "240", std::nullopt, std::nullopt, std::nullopt } });
    const auto missingBefore = missing;
    const auto missingWhy = fitPackCleanMatches(missing, files.source());
    ok(names(missingWhy, "gone.nam") && sameMatches(missing, missingBefore), "no bytes for a file: \"" + missingWhy + "\"");
    auto noFile = stageOf({ { "d", "60", 12, 1, std::nullopt }, { "", "240", 0, 1, std::nullopt } });
    const auto noFileBefore = noFile;
    const auto noFileWhy = fitPackCleanMatches(noFile, files.source());
    ok(names(noFileWhy, "no file") && sameMatches(noFile, noFileBefore), "an entry with no file: \"" + noFileWhy + "\"");
    auto noSource = stageOf({ { "d", "60", 12, 1, std::nullopt } });
    const auto noSourceBefore = noSource;
    const auto noSourceWhy = fitPackCleanMatches(noSource, ModelSource {});
    ok(! noSourceWhy.empty() && sameMatches(noSource, noSourceBefore), "no source: \"" + noSourceWhy + "\"");
}

// One stage both tests below fit: a real capture, which the fit matches, and a delay stated right, which it does not.
namz::rig::Stage mixedStage() {
    return stageOf({ { "a2.nam", "60", std::nullopt, std::nullopt, std::nullopt }, { "d", "240", 12, -1, std::nullopt } });
}
Files mixedFiles() {
    Files files;
    files.bytes["a2.nam"] = bytesOf(exampleModel("A2.nam"));
    files.bytes["d"] = bytesOf(delayModel(12, -1.0));
    return files;
}

void matchesAreNotAStart() {
    group("a clean match already on the entries is not a starting point: a second run gives the same matches");
    const auto files = mixedFiles();
    auto first = mixedStage();
    const auto why = fitPackCleanMatches(first, files.source());
    ok(why.empty() && first.device.files[0].cleanMatch && ! first.device.files[0].cleanMatch->allpassHz.empty()
       && same(first.device.files[1].cleanMatch, packMatch(12, -1, {})),
       "precondition: A2 fitted with corners, the delay by its reading: " + spellAll(first));
    auto again = first;
    const auto againWhy = fitPackCleanMatches(again, files.source());
    ok(againWhy.empty() && sameMatches(again, first), "run again on its own matches: " + spellAll(again));
    auto planted = mixedStage();
    for (auto& f : planted.device.files) f.cleanMatch = packMatch(300, 1, { 40.0, 80.0 });
    const auto plantedWhy = fitPackCleanMatches(planted, files.source());
    ok(plantedWhy.empty() && sameMatches(planted, first), "run over planted matches (lag 300, polarity 1, 40 + 80 Hz): "
       + spellAll(planted));
}

void roundTrip() {
    group("the fitted stage round-trips through namz's writer and loader with the same matches");
    const auto files = mixedFiles();
    auto st = mixedStage();
    const auto why = fitPackCleanMatches(st, files.source());
    namz::rig::Rig rig; rig.chain = { st };
    bool loaded = false;
    const auto back = namz::rig::loadRigManifest(namz::rig::writeManifest(rig), &loaded);
    ok(why.empty() && st.device.files[0].cleanMatch && st.device.files[1].cleanMatch && loaded && back.chain.size() == 1
       && sameMatches(back.chain[0], st),
       "written and read back: " + (back.chain.empty() ? std::string("no stage") : spellAll(back.chain[0])));
    // …and the player reads them as the matches they are, in the file's own rate.
    const auto t = back.chain.empty() ? AlignmentTable {} : AlignmentTable::fromDevice(back.chain[0].device);
    ok(t.cleanMatchByFile.size() == 2 && t.lagOf("d", 48000.0, 48000.0) == 12 && t.invertsClean("d"),
       "fromDevice reads both; d tapped 12 late, inverted");
}

} // namespace

int main() {
    std::printf("fitPackCleanMatches — the clean match a pack carries, fitted for every entry\n");
    readableIsNamzRule();
    everyEntryMatched();
    fallbackIsTheStart();
    refusals();
    matchesAreNotAStart();
    roundTrip();
    return felitronics::test::report();
}
