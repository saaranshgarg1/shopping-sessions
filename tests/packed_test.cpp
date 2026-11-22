// Standalone invariants for v5::PackedIds: collisions, fallback, 10k repeated keys, zero key, load.
#include <cassert>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "../src/core.hpp"
using namespace ss;
#include "../src/packed_ids.hpp"

static void check_invariants(const v5::PackedIds& p, size_t expect_used) {
    assert(!p.slots.empty());                             // table capacity always nonzero
    assert((p.slots.size() & (p.slots.size()-1)) == 0);   // power of two
    assert(p.slots.size() >= 16);
    assert(p.used == expect_used);
    size_t occupied = 0;
    for (const auto& slot : p.slots) if (slot.value) ++occupied;
    assert(occupied == p.used);                           // zero occupancy marks a free slot
    assert(p.used*3 <= p.slots.size()*2);                 // load stays at or below 2/3
}

int main() {
    // Prefetch must be safe before the table exists; it returns the hash add_prehashed needs.
    {
        v5::PackedIds fresh;
        assert(fresh.prefetch(0x1234) == mix(0x1234));
        auto [value, added] = fresh.add(0x1234, 0, [](uint32_t) { return false; });
        assert(value == 0 && added);
        check_invariants(fresh, 1);
    }

    // Forced collisions: every key starts at slot 0, so probing and equality decide everything.
    {
        v5::PackedIds forced;
        std::vector<std::string> names;
        for (uint32_t i = 0; i < 12; ++i) names.push_back("name-" + std::to_string(i));
        for (uint32_t i = 0; i < 12; ++i) {
            auto [value, added] = forced.add_prehashed(uint64_t(1)<<63, i, 0,
                [&](uint32_t previous) { return names[previous] == names[i]; });
            assert(value == i && added);
        }
        check_invariants(forced, 12);
        // Same fallback key, different exact bytes: the callback keeps them distinct.
        auto [v0, a0] = forced.add_prehashed(uint64_t(1)<<63, 0, 0,
            [&](uint32_t previous) { return names[previous] == "name-0"; });
        assert(v0 == 0 && !a0);
        auto [v1, a1] = forced.add_prehashed(uint64_t(1)<<63, 0, 0,
            [&](uint32_t previous) { return names[previous] == "name-1"; });
        assert(v1 == 1 && !a1);
        check_invariants(forced, 12);
    }

    // A hit at the growth threshold must not grow; the next insertion may.
    {
        v5::PackedIds p;
        for (uint32_t i = 0; i < 21; ++i) p.add(i + 1, i, [](uint32_t) { return false; });
        check_invariants(p, 21);
        size_t size = p.slots.size();
        auto [hit, added] = p.add(1, 0, [](uint32_t) { return false; });
        assert(hit == 0 && !added && p.slots.size() == size);
        auto [value, inserted] = p.add(22, 21, [](uint32_t) { return false; });
        assert(value == 21 && inserted && p.slots.size() > size);
        check_invariants(p, 22);
    }

    // Zero key: a legitimate entry, not an empty slot.
    {
        v5::PackedIds p;
        auto [value, added] = p.add(0, 7, [](uint32_t) { return false; });
        assert(value == 7 && added);
        auto [again, added_again] = p.add(0, 9, [](uint32_t) { return false; });
        assert(again == 7 && !added_again);
        check_invariants(p, 1);
    }

    // 10000 keys, repeated with duplicates, then every key looked up again.
    {
        std::mt19937_64 random(490235);
        std::deque<std::string> ids;
        std::unordered_map<std::string, uint32_t> expected;
        std::vector<SV> exact_names;
        v5::PackedIds p;
        auto touch = [&](std::string id) {
            ids.push_back(std::move(id));
            const std::string& exact = ids.back();
            auto [it, inserted] = expected.emplace(exact, uint32_t(expected.size()));
            if (inserted) exact_names.push_back(exact);
            uint64_t key = v5::alias_key(exact);
            auto [value, added] = p.add(key, it->second,
                [&](uint32_t previous) { return exact_names[previous] == exact; });
            assert(value == it->second && added == inserted);
        };
        for (size_t i = 0; i < 10000; ++i) {
            if (i % 5 == 0 && !ids.empty() && random() % 2) { touch(ids[random() % ids.size()]); continue; }
            std::string id = "u";
            for (size_t n = random() % 25; n; --n) id.push_back(char(random() % 256));
            touch(std::move(id));
        }
        check_invariants(p, expected.size());
        for (const auto& [exact, want] : expected) {
            uint64_t key = v5::alias_key(exact);
            auto [value, added] = p.add(key, UINT32_MAX,
                [&](uint32_t previous) { return exact_names[previous] == exact; });
            assert(value == want && !added);
        }
        check_invariants(p, expected.size());
    }

    return 0;
}
