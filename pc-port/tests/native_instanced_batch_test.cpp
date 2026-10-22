#include "native_instanced_batch.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace iss::native_geometry;

namespace {

struct Fixture {
    std::vector<std::byte> vertex;
    std::vector<std::byte> instance;
    std::vector<std::byte> index;
    std::array<std::byte, 8> state{};
    InstancedDraw draw{};

    Fixture(std::mt19937& rng, std::uint32_t vertices, std::uint32_t instances,
            std::uint32_t index_count, std::int32_t vertex_offset,
            std::uint32_t stride = 16) {
        vertex.resize(vertices * stride);
        instance.resize(instances * 72);
        index.resize(index_count * 2);
        for (auto& byte : vertex) byte = std::byte(rng() & 0xff);
        for (auto& byte : instance) byte = std::byte(rng() & 0xff);
        for (std::uint32_t i = 0; i < index_count; ++i) {
            const auto value = vertex_offset < 0 ? 1 + rng() % (vertices - 1)
                : rng() % (vertices - vertex_offset);
            index[2 * i] = std::byte(value & 0xff);
            index[2 * i + 1] = std::byte((value >> 8) & 0xff);
        }
        state[0] = std::byte{0x42};
        draw.vertex_hash = 0xC01162F7;
        draw.fragment_hash = 0x1318DDD4;
        draw.graphics_pipeline_key_hash = 0x3B6D3A6D0DEC8019ull;
        draw.vertex_permutation = 0;
        draw.exact_spirv_verified = true;
        draw.primitive_type = 4;
        draw.index_type = 0;
        draw.vertex_offset = vertex_offset;
        draw.instance_count = instances;
        draw.binding_count = 1;
        draw.attribute_count = 1;
        draw.vertex = {{vertex.data(), vertex.size()}, 0x22000000,
                       std::uint32_t(vertex.size()), stride, 0x00AA0000,
                       0x00012345, 2, 0, 0, true, true, true};
        draw.instance_bytes = {instance.data(), instance.size()};
        draw.instance_guest_base = 0x23000000;
        draw.instance_cpu_owned = draw.instance_mapped = draw.instance_snapshot_stable = true;
        draw.index_bytes = {index.data(), index.size()};
        draw.index_count = index_count;
        draw.index_cpu_owned = draw.index_mapped = draw.index_snapshot_stable = true;
        draw.state = {{state.data(), state.size()}, true, true};
    }
};

std::uint16_t Index(const std::byte* p) {
    return std::uint16_t(std::to_integer<unsigned char>(p[0])) |
           std::uint16_t(std::to_integer<unsigned char>(p[1])) << 8;
}

void Differential() {
    std::mt19937 rng{0x1318DDD4};
    for (int trial = 0; trial < 512; ++trial) {
        Fixture first{rng, 8 + rng() % 120, 1 + rng() % 32, 3 + rng() % 80, 0};
        Fixture second{rng, 8 + rng() % 120, 1 + rng() % 32, 3 + rng() % 80,
                       (trial & 1) ? -1 : 2};
        const std::array<InstancedDraw, 2> draws{first.draw, second.draw};
        const auto packed = BuildInstanced(draws);
        assert(packed && packed.batch.draw_count == 2);
        const std::array<const Fixture*, 2> sources{&first, &second};
        std::vector<std::byte> expected_vertex, expected_instance, expected_index;
        std::size_t vertex_prefix = 0, instance_prefix = 0, index_prefix = 0;
        for (std::size_t d = 0; d < 2; ++d) {
            const auto& source = *sources[d];
            const auto& command = packed.batch.commands[d];
            assert(command.index_count == source.draw.index_count);
            assert(command.instance_count == source.draw.instance_count);
            assert(command.first_index == index_prefix);
            assert(command.vertex_offset ==
                   std::int32_t(vertex_prefix + source.draw.vertex_offset));
            assert(command.first_instance == instance_prefix);
            for (std::uint32_t i = 0; i < command.index_count; ++i) {
                const auto source_index = Index(source.index.data() + i * 2);
                const auto packed_index = Index(packed.batch.indices.data() +
                                                (command.first_index + i) * 2);
                assert(source_index == packed_index);
                const auto source_vertex = source_index + source.draw.vertex_offset;
                const auto packed_vertex = packed_index + command.vertex_offset;
                assert(std::memcmp(source.vertex.data() + source_vertex * 16,
                                   packed.batch.vertices.data() + packed_vertex * 16, 16) == 0);
            }
            for (std::uint32_t instance = 0; instance < command.instance_count; ++instance) {
                assert(std::memcmp(source.instance.data() + instance * 72,
                                   packed.batch.instances.data() +
                                       (command.first_instance + instance) * 72, 72) == 0);
            }
            expected_vertex.insert(expected_vertex.end(), source.vertex.begin(), source.vertex.end());
            expected_instance.insert(expected_instance.end(), source.instance.begin(), source.instance.end());
            expected_index.insert(expected_index.end(), source.index.begin(), source.index.end());
            vertex_prefix += source.vertex.size() / 16;
            instance_prefix += source.draw.instance_count;
            index_prefix += source.draw.index_count;
        }
        assert(packed.batch.vertices == expected_vertex);
        assert(packed.batch.instances == expected_instance);
        assert(packed.batch.indices == expected_index);
    }
}

void Gates() {
    std::mt19937 rng{1};
    Fixture first{rng, 20, 2, 30, 0};
    Fixture second{rng, 20, 3, 30, 0};
    std::array<InstancedDraw, 2> draws{first.draw, second.draw};
    draws[1].graphics_pipeline_key_hash ^= 1;
    assert(BuildInstanced(draws).reject == Reject::UnsupportedShaderOrLayout);
    draws[1] = second.draw;
    draws[1].exact_spirv_verified = false;
    assert(BuildInstanced(draws).reject == Reject::UnsupportedShaderOrLayout);
    draws[1] = second.draw;
    draws[1].instance_cpu_owned = false;
    assert(BuildInstanced(draws).reject == Reject::UnstableSource);
    draws[1] = second.draw;
    draws[1].instance_bytes.size -= 72;
    assert(BuildInstanced(draws).reject == Reject::InvalidRange);
    draws[1] = second.draw;
    draws[1].vertex.stride = 8;
    assert(BuildInstanced(draws).reject == Reject::DifferentState);
    draws[1] = second.draw;
    draws[1].vertex_offset = 100;
    assert(BuildInstanced(draws).reject == Reject::OutOfBoundsIndex);
    draws[1] = second.draw;
    second.state[0] = std::byte{0x33};
    assert(BuildInstanced(draws).reject == Reject::DifferentState);
    assert(BuildInstanced({}).reject == Reject::EmptyOrTooManyDraws);
}

} // namespace

int main() {
    Differential();
    Gates();
}
