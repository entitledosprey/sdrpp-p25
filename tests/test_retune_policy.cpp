// Unit test for the retune policy (header-only, no SDR++ needed):
//   g++ -std=c++17 -I src tests/test_retune_policy.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include "retune_policy.h"

using p25::planRetune;

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { failures++; }
}

int main() {
    const double fs = 2.4e6, half = 0.42 * fs, guard = 60e3;

    // A site that fits one window: every frequency should be reachable from the home centre.
    std::vector<double> small = { 851.0125e6, 851.5125e6, 851.9375e6, 852.2625e6, 852.7625e6 };
    double cc = small[0];
    auto p = planRetune(small[3], 860e6, half, guard, cc, small);
    check(p.retune, "far away: retunes");
    bool homeCovers = p25::allUsableAt(small, p.center, half, guard);
    check(homeCovers, "fitting site: home centre covers every site frequency");
    bool noFurther = true;
    for (double f : small) { noFurther &= !planRetune(f, p.center, half, guard, cc, small).retune; }
    check(noFurther, "fitting site: no further retunes once home");

    // A 4.5 MHz simulcast site: can't cover everything, but each target must be usable.
    std::vector<double> wide = { 851.0125e6, 852.5e6, 853.3e6, 854.1e6, 855.5125e6 };
    cc = wide[0];
    double center = 851.5e6;
    bool usable = true;
    for (double f : wide) {
        auto q = planRetune(f, center, half, guard, cc, wide);
        usable &= p25::usableAt(f, q.center, half, guard);
    }
    check(usable, "wide site: every voice frequency usable after its plan");
    auto near = planRetune(852.6e6, center, half, guard, cc, wide);
    check(p25::usableAt(cc, near.center, half, guard), "wide site: nearby voice channel keeps the CC in view");

    // DC guard: a target exactly on the centre must move the centre.
    auto dc = planRetune(851.5e6, 851.5e6, half, guard, 0, {});
    check(dc.retune && p25::usableAt(851.5e6, dc.center, half, guard), "target on the DC spike is moved off it");

    // Already usable: no retune.
    check(!planRetune(851.8e6, 851.5e6, half, guard, 0, {}).retune, "usable target does not retune");

    printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
