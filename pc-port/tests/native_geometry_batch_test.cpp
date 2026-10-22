#include "native_geometry_batch.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace iss::native_geometry;

namespace {

constexpr std::array<std::uint32_t, 3> formats{418732, 299948, 193068};
constexpr std::array<std::uint32_t, 3> dwords{0, 8, 4};

struct Fixture {
    std::array<std::vector<std::byte>, 3> streams;
    std::vector<std::byte> indices;
    std::array<std::byte, 8> state{};
    Draw draw{};

    Fixture(std::mt19937& rng, std::uint32_t vertices, std::uint32_t count,
            std::int32_t vertex_offset) {
        draw.vertex_hash = 0x0BEB7156;
        draw.fragment_hash = 0x6353E48B;
        draw.graphics_pipeline_key_hash = 0x6794975E42E32374ull;
        draw.vertex_permutation = 0;
        draw.queue = 0;
        draw.primitive_type = 4;
        draw.index_type = 0;
        draw.instance_count = 1;
        draw.first_instance = 0;
        draw.binding_count = 3;
        draw.attribute_count = 3;
        draw.fetch_inputs_only = true;
        draw.vertex_index_unobserved = true;
        draw.vertex_offset = vertex_offset;
        state[0] = std::byte{0x31};
        state[1] = std::byte{0x72};
        draw.state = {{state.data(), state.size()}, true, true};
        for (std::size_t s = 0; s < 3; ++s) {
            streams[s].resize(vertices * 8);
            for (auto& byte : streams[s]) byte = std::byte(rng() & 0xff);
            draw.streams[s] = {{streams[s].data(), streams[s].size()},
                               0x20000000 + std::uint64_t(s) * 0x10000,
                               std::uint32_t(streams[s].size()), 8, 524290,
                               formats[s], 2, dwords[s], 0, true, true, true};
        }
        indices.resize(count * 2);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint16_t safe = vertex_offset < 0
                ? std::uint16_t(1 + rng() % (vertices - 1))
                : std::uint16_t(rng() % (vertices - vertex_offset));
            indices[2 * i] = std::byte(safe & 0xff);
            indices[2 * i + 1] = std::byte(safe >> 8);
        }
        draw.index_bytes = {indices.data(), indices.size()};
        draw.index_count = count;
        draw.index_cpu_owned = true;
        draw.index_mapped = true;
        draw.index_snapshot_stable = true;
    }
};

std::uint16_t Index(const std::byte* p) {
    return std::uint16_t(std::to_integer<unsigned char>(p[0])) |
           std::uint16_t(std::to_integer<unsigned char>(p[1])) << 8;
}

void Differential() {
    std::mt19937 rng{0x0BEB7156};
    for (int iteration = 0; iteration < 512; ++iteration) {
        Fixture first{rng, std::uint32_t(4 + rng() % 200), 3 + rng() % 60, 0};
        Fixture second{rng, std::uint32_t(4 + rng() % 200), 3 + rng() % 60,
                       (iteration & 1) ? -1 : 2};
        const std::array<Draw, 2> draws{first.draw, second.draw};
        const auto result = Build(draws);
        assert(result);
        assert(result.batch.draw_count == 2);
        const std::array<const Fixture*, 2> sources{&first, &second};
        std::array<std::vector<std::byte>, 3> expected_vertices;
        std::vector<std::byte> expected_indices;
        std::size_t vertex_base = 0;
        std::size_t index_base = 0;
        for (std::size_t d = 0; d < 2; ++d) {
            const auto& cmd = result.batch.commands[d];
            const auto& original = *sources[d];
            assert(cmd.index_count == original.draw.index_count);
            assert(cmd.instance_count == 1 && cmd.first_instance == 0);
            assert(cmd.first_index == index_base);
            assert(cmd.vertex_offset ==
                   std::int32_t(vertex_base + original.draw.vertex_offset));
            for (std::size_t s = 0; s < 3; ++s) {
                expected_vertices[s].insert(expected_vertices[s].end(),
                                            original.streams[s].begin(),
                                            original.streams[s].end());
            }
            expected_indices.insert(expected_indices.end(), original.indices.begin(),
                                    original.indices.end());
            vertex_base += original.streams[0].size() / 8;
            index_base += original.indices.size() / 2;
            for (std::uint32_t i = 0; i < cmd.index_count; ++i) {
                const auto source_index = Index(original.indices.data() + i * 2);
                const auto packed_index = Index(result.batch.indices.data() +
                                                (cmd.first_index + i) * 2);
                assert(source_index == packed_index);
                const std::int64_t source_vertex = source_index +
                                                   original.draw.vertex_offset;
                const std::int64_t packed_vertex = packed_index + cmd.vertex_offset;
                for (std::size_t s = 0; s < 3; ++s) {
                    const auto* source = original.streams[s].data() + source_vertex * 8;
                    const auto* packed = result.batch.vertices[s].data() +
                                         packed_vertex * 8;
                    assert(std::memcmp(source, packed, 8) == 0);
                }
            }
        }
        for (std::size_t s = 0; s < 3; ++s) {
            assert(result.batch.vertices[s] == expected_vertices[s]);
        }
        assert(result.batch.indices == expected_indices);
        assert(!MayAttemptRecord(result, {}));
        assert(MayAttemptRecord(result, {true, true}));
    }
}

void Gates() {
    std::mt19937 rng{9};
    Fixture a{rng, 20, 30, 0};
    Fixture b{rng, 20, 30, 0};
    std::array<Draw, 2> draws{a.draw, b.draw};
    b.state[2] = std::byte{1};
    assert(Build(draws).reject == Reject::DifferentState);
    b.state[2] = std::byte{0};
    draws[1].streams[1].format_word ^= 1;
    const auto rejected = Build(draws);
    assert(rejected.reject == Reject::UnsupportedShaderOrLayout);
    assert(rejected.batch.commands.empty());
    draws[1] = b.draw;
    draws[1].streams[0].cpu_owned = false;
    assert(Build(draws).reject == Reject::UnstableSource);
    draws[1] = b.draw;
    draws[1].vertex_offset = 1000;
    assert(Build(draws).reject == Reject::OutOfBoundsIndex);
    draws[1] = b.draw;
    draws[1].state.complete = false;
    assert(Build(draws).reject == Reject::UnverifiedState);
    draws[1] = b.draw;
    draws[1].index_bytes.size -= 2;
    assert(Build(draws).reject == Reject::InvalidRange);
    draws[1] = b.draw;
    draws[1].first_instance = 1;
    assert(Build(draws).reject == Reject::UnsupportedShaderOrLayout);
    draws[1] = b.draw;
    draws[1].vertex_index_unobserved = false;
    assert(Build(draws).reject == Reject::UnsupportedShaderOrLayout);
    draws[1] = b.draw;
    draws[1].graphics_pipeline_key_hash ^= 1;
    assert(Build(draws).reject == Reject::UnsupportedShaderOrLayout);
    assert(Build({}).reject == Reject::EmptyOrTooManyDraws);
}

void CapturedGroupSize() {
    std::mt19937 rng{136};
    std::vector<Fixture> fixtures;
    fixtures.reserve(136);
    for (int i = 0; i < 136; ++i) fixtures.emplace_back(rng, 20, 30, 0);
    std::vector<Draw> draws;
    draws.reserve(fixtures.size());
    for (const auto& fixture : fixtures) draws.push_back(fixture.draw);
    const auto result = Build(draws);
    assert(result && result.batch.commands.size() == 136);
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const auto& cmd = result.batch.commands[i];
        assert(cmd.first_index == i * 30);
        assert(cmd.vertex_offset == std::int32_t(i * 20));
        for (std::size_t stream = 0; stream < 3; ++stream) {
            const auto* original = fixtures[i].streams[stream].data();
            const auto* packed = result.batch.vertices[stream].data() + i * 160;
            assert(std::memcmp(original, packed, 160) == 0);
        }
    }
}

} // namespace

int main() {
    Differential();
    Gates();
    CapturedGroupSize();
}
