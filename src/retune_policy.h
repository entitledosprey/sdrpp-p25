#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace p25 {

struct RetunePlan {
    bool retune;   // false: the target is already usable at the current centre
    double center; // SDR centre to use
};

// A frequency is usable at centre c when it sits inside the usable half-window and clear of
// the zero-IF DC spike.
inline bool usableAt(double f, double c, double half, double guard) {
    double off = std::fabs(f - c);
    return off >= guard && off <= half;
}

inline bool allUsableAt(const std::vector<double>& fs, double c, double half, double guard) {
    return std::all_of(fs.begin(), fs.end(), [&](double f) { return usableAt(f, c, half, guard); });
}

// Midpoint of fs, nudged off the DC spike if one of them would land on it.
inline bool centerFor(const std::vector<double>& fs, double half, double guard, double& out) {
    auto [lo, hi] = std::minmax_element(fs.begin(), fs.end());
    double mid = (*lo + *hi) / 2.0;
    for (double nudge : { 0.0, 1.5, -1.5, 3.0, -3.0 }) {
        double c = mid + nudge * guard;
        if (allUsableAt(fs, c, half, guard)) {
            out = c;
            return true;
        }
    }
    return false;
}

// Decide where the SDR centre should be so `target` can be received.
//  1. Leave the centre alone if the target is already usable.
//  2. "Home" centre: one that covers the target, the control channel and every known site
//     frequency, so a site that fits the window never retunes again.
//  3. A centre covering the target and the control channel.
//  4. Otherwise sit the target a quarter-megahertz off centre, towards the control channel.
inline RetunePlan planRetune(double target, double center, double half, double guard, double cc,
                             const std::vector<double>& known) {
    if (usableAt(target, center, half, guard)) { return { false, center }; }

    double c;
    std::vector<double> all = known;
    all.push_back(target);
    if (cc > 0) { all.push_back(cc); }
    if (centerFor(all, half, guard, c)) { return { true, c }; }
    if (cc > 0 && centerFor({ target, cc }, half, guard, c)) { return { true, c }; }

    double toward = (cc > 0 && cc < target) ? -1.0 : 1.0;
    return { true, target + toward * std::min(250e3, half - guard) };
}

}
