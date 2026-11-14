// SPDX-License-Identifier: GPL-3.0-only
// The reference for tests/run_compare.sh: Synth2's own engine (third_party/oxo_synth2/EngineCore: MIT, Copyright (c)
// 2026 Francois Chabloz), compiled as it is, playing a script. The computer only: never part of the firmware.
//   ref_render SCRIPT OUT.f32        (stereo float32, 44100 Hz)
// A script (tools/compare/cases.py writes them; tests/vs_render.c plays the same file through the firmware's engine):
//   p ID VALUE          a patch value (all before the first event)
//   on FRAME NOTE VEL   off FRAME NOTE   bend FRAME VALUE14   pedal FRAME 0|1      (FRAME: a multiple of 16)
//   len FRAMES          the length (a multiple of 256)
// It plays as the FM-1 does: host blocks of 256 frames, events on the 16-frame grid.
#include "Synth.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Ev { long frame; int kind, a, b; };

int main(int argc, char **argv) {
    if (argc != 3) return std::fprintf(stderr, "usage: ref_render SCRIPT OUT.f32\n"), 2;
    std::ifstream in(argv[1]);
    oxo::s2::Synth synth;
    synth.SetSampleRate(44100.0);
    std::vector<Ev> evs;
    long len = 0;
    for (std::string line; std::getline(in, line);) {
        std::istringstream s(line);
        std::string w;
        long f = 0;
        int a = 0, b = 0;
        if (!(s >> w) || w[0] == '#') continue;
        if (w == "p") { s >> a >> b; synth.SetParam(a, b); }
        else if (w == "len") s >> len;
        else if (w == "on") { s >> f >> a >> b; evs.push_back({f, 0, a, b}); }
        else if (w == "off") { s >> f >> a; evs.push_back({f, 1, a, 0}); }
        else if (w == "bend") { s >> f >> a; evs.push_back({f, 2, a, 0}); }
        else if (w == "pedal") { s >> f >> a; evs.push_back({f, 3, a, 0}); }
        else return std::fprintf(stderr, "ref_render: %s?\n", w.c_str()), 2;
    }
    std::vector<float> l(size_t(len), 0.f), r(size_t(len), 0.f), out(size_t(len) * 2);
    size_t next = 0;
    for (long pos = 0; pos < len; pos += 16) {
        if (pos % 256 == 0) synth.BeginBlock(256);
        for (; next < evs.size() && evs[next].frame <= pos; ++next) {
            const Ev &e = evs[next];
            if (e.kind == 0) synth.NoteOn(e.a, e.b);
            else if (e.kind == 1) synth.NoteOff(e.a);
            else if (e.kind == 2) synth.PitchBend(e.a);
            else synth.SetSustain(e.a != 0);
        }
        synth.Render(l.data() + pos, r.data() + pos, 16);
    }
    for (long i = 0; i < len; ++i) out[size_t(2 * i)] = l[size_t(i)], out[size_t(2 * i + 1)] = r[size_t(i)];
    FILE *f = std::fopen(argv[2], "wb");
    if (!f) return std::perror(argv[2]), 1;
    std::fwrite(out.data(), sizeof(float), out.size(), f);
    std::fclose(f);
    return 0;
}
