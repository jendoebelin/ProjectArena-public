// AUTO ROTATION's list (app/src/auto_rotation.h): until the streamer changes it, AUTO plays what it played before;
// once changed, the ticked rows in the list's order. Built and run by tools/run_tests.bat
#include "../src/auto_rotation.h"
#include <cstdio>

static int fails = 0, passes = 0;
static void check(bool ok, const char* what) {
    if (ok) ++passes;
    else { ++fails; printf("FAIL: %s\n", what); }
}

using autorot::Item;

// the views AUTO shows, `n` turns from row `from` (presets as their view number, my cameras as 100 + number)
static std::vector<int> play(const std::vector<Item>& L, int from, int n) {
    std::vector<int> v;
    for (int k = 0; k < n; ++k) {
        from = autorot::next(L, from);
        if (from < 0) break;
        v.push_back(L[from].mine ? 100 + L[from].id : L[from].id);
    }
    return v;
}

int main() {
    {   // no cameras of yours: the presets in AUTO's old order, 9 s each, TOP not played
        std::vector<Item> L;
        check(autorot::sync(L, false, {}), "a new list is made");
        check(L.size() == 12, "twelve presets");
        check(play(L, autorot::rowOf(L, false, 1), 8) == std::vector<int>({6, 3, 2, 7, 4, 8, 9, 1}),
              "side, blue's eyes, blue end, high, orange's eyes, orange end, corner, courtside");
        check(!L[autorot::rowOf(L, false, 5)].on, "TOP is in the list, not ticked");
        check(!L[autorot::rowOf(L, false, 10)].on && !L[autorot::rowOf(L, false, 11)].on && !L[autorot::rowOf(L, false, 12)].on,
              "the follow views are in the list, not ticked");
        bool nine = true;
        for (const Item& it : L) nine &= it.hold == 9.f;
        check(nine, "each preset 9 s");
        check(!autorot::sync(L, false, {}), "nothing changes the second time");
    }
    {   // only action cameras: still the presets (the action cameras cut in on hits, not in the list)
        std::vector<Item> L;
        autorot::sync(L, false, {1, 1});
        check(L.size() == 12 && play(L, -1, 1) == std::vector<int>({1}), "action cameras are not in the list");
    }
    {   // a camera of yours in turn: from then on only yours, by number, as before
        std::vector<Item> L;
        autorot::sync(L, false, {});
        autorot::sync(L, false, {0, 1, 2});
        check(L.size() == 14, "two of yours added (not the action one)");
        check(play(L, -1, 4) == std::vector<int>({100, 102, 100, 102}), "yours only, in turn");
        autorot::sync(L, false, {});
        check(play(L, -1, 1) == std::vector<int>({1}), "cleared: the presets again");
    }
    {   // not changed yet: a camera turned from action to timer goes back to its number's place
        std::vector<Item> L;
        autorot::sync(L, false, {0, 1, 0});
        autorot::sync(L, false, {0, 0, 0});
        check(play(L, -1, 3) == std::vector<int>({100, 101, 102}), "by number");
    }
    {   // the streamer's list: a preset and a camera, then a new camera (at the end, ticked)
        std::vector<Item> L;
        autorot::sync(L, false, {0});
        for (Item& it : L) it.on = (!it.mine && it.id == 3) || (it.mine && it.id == 0);
        autorot::sync(L, true, {0});
        check(play(L, -1, 4) == std::vector<int>({3, 100, 3, 100}), "blue end and camera 1 in turn");
        autorot::sync(L, true, {0, 2});
        check(L.back().mine && L.back().id == 1 && L.back().on, "a new camera at the end, ticked");
        check(play(L, autorot::rowOf(L, true, 0), 3) == std::vector<int>({101, 3, 100}), "and in the turn");
        std::swap(L[0], L[autorot::rowOf(L, false, 3)]);
        check(play(L, -1, 1) == std::vector<int>({3}), "moved to the top: first");
        autorot::sync(L, true, {1, 2});
        check(autorot::rowOf(L, true, 0) < 0, "made an action camera: out of the list");
        autorot::sync(L, true, {0, 2});
        check(L.back().mine && L.back().id == 0, "a timer again: back, at the end");
        for (Item& it : L) it.on = false;
        check(autorot::next(L, -1) < 0, "nothing ticked: nothing to play");
    }
    {   // saved and read back; a bad line or a hold out of range is not taken
        std::vector<Item> L;
        autorot::sync(L, false, {0, 0});
        L[2].on = false; L[0].hold = 15.f;
        std::swap(L[1], L[8]);
        std::vector<Item> R = autorot::load(autorot::save(L) + "x 1 1\nm\np 2 1 500\n");
        check(R.size() == 15, "every row back, the bad ones skipped but the hold kept to a sane one");
        R.pop_back();
        check(autorot::same(L, R), "the same list");
        check(autorot::load("p 2 1 500\n")[0].hold == 9.f, "a hold out of range: 9 s");
        std::vector<Item> D = autorot::load("p 1 1 9\np 1 0 9\nm 7 1\n");
        autorot::sync(D, true, {0});
        check(autorot::rowOf(D, false, 1) == 0 && D[0].on && autorot::rowOf(D, true, 7) < 0, "a row twice: the first; a camera that is gone: out");
        check(D.size() == 13, "the missing presets and camera added");
    }
    {   // a list the streamer made before CORNER and COURTSIDE: the two at its end, unticked (their AUTO stays as it was)
        std::vector<Item> O = autorot::load("p 3 1 9\np 1 1 4\np 6 0 9\np 2 1 9\np 7 0 9\np 4 1 9\np 5 0 9\n");
        check(autorot::sync(O, true, {}), "the two added");
        check(O.size() == 12 && O[7].id == 8 && O[8].id == 9 && !O[7].on && !O[8].on, "at the end, unticked");
        check(O[9].id == 10 && O[10].id == 11 && O[11].id == 12 && !O[9].on && !O[10].on && !O[11].on, "the follow views too");
        check(play(O, -1, 4) == std::vector<int>({3, 1, 2, 4}), "AUTO as the streamer had it");
    }
    check(autorot::nextHold(2) == 4 && autorot::nextHold(4) == 9 && autorot::nextHold(9) == 15 && autorot::nextHold(15) == 2, "holds 2, 4, 9, 15 s");
    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
