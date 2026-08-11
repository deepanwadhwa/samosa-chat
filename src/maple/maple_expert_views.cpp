#include "maple_expert_views.h"

#include "mlx/compile.h"

#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace samosa {
namespace maple {

namespace {

using mlx::core::array;

static const MapleExpertRegion& region_for(const MapleExpertRecord& record,
                                           MapleExpertPart part) {
    const int index = static_cast<int>(part);
    if (index < 0 || index >= 6 || record.regions[index].part != part) {
        throw std::runtime_error("Maple expert region order is invalid");
    }
    return record.regions[index];
}

static array make_region_view(const std::shared_ptr<MapleExpertLease>& lease,
                              const MapleExpertRecord& record,
                              MapleExpertPart part,
                              mlx::core::Shape shape,
                              mlx::core::Dtype dtype) {
    const MapleExpertRegion& region = region_for(record, part);
    size_t elements = 1;
    for (mlx::core::ShapeElem dim : shape) {
        if (dim < 0 ||
            static_cast<size_t>(dim) > std::numeric_limits<size_t>::max() / elements) {
            throw std::runtime_error("Maple MLX view shape overflows");
        }
        elements *= static_cast<size_t>(dim);
    }
    const size_t itemsize = mlx::core::size_of(dtype);
    if (itemsize != 0 && elements > std::numeric_limits<size_t>::max() / itemsize) {
        throw std::runtime_error("Maple MLX view byte size overflows");
    }
    const size_t bytes = elements * itemsize;
    if (bytes != region.bytes || region.slab_offset > lease->bytes() ||
        region.bytes > lease->bytes() - region.slab_offset) {
        throw std::runtime_error("Maple MLX view shape does not match expert region");
    }

    uint8_t* data = lease->data() + region.slab_offset;
    /* The captured shared_ptr is deliberately a no-op deleter: MLX owns the
     * wrapper/buffer, while the capture owns the lease until that wrapper is
     * gone.  If MLX must copy the raw pointer, its constructor invokes this
     * callback immediately after the copy, which is also safe. */
    std::function<void(void*)> keepalive = [lease](void*) {};
    return array(data, std::move(shape), dtype, keepalive);
}

static std::pair<array, array> affine_params(const array& row_alpha,
                                             int input_dim) {
    constexpr int kGroupSize = 128;
    if (input_dim <= 0 || input_dim % kGroupSize != 0) {
        throw std::runtime_error("Maple streamed quantized input is not group aligned");
    }
    const int groups = input_dim / kGroupSize;
    mlx::core::Shape scale_shape(
        row_alpha.shape().begin(), row_alpha.shape().end());
    scale_shape.push_back(groups);
    /* Maple sanitize() materializes expanded row scales before fusing
     * projections.  Preserve that contiguous layout: a zero-stride broadcast
     * is numerically equivalent but selects a different ternary Metal path
     * and can shift BF16 results. */
    auto scales = mlx::core::contiguous(mlx::core::broadcast_to(
        mlx::core::expand_dims(row_alpha, -1), scale_shape));
    return {scales, mlx::core::contiguous(mlx::core::negative(scales))};
}

static array compiled_clamped_swiglu(const array& gate, const array& up,
                                     float mlp_clamp) {
    if (mlp_clamp != 7.0f) {
        throw std::runtime_error(
            "compiled Maple SwiGLU requires the checkpoint clamp of 7");
    }
    static auto compiled = mlx::core::compile(
        [](const std::vector<array>& inputs) {
            const auto& gate_in = inputs[0];
            const auto& up_in = inputs[1];
            auto clipped_gate = mlx::core::minimum(
                gate_in, array(7.0f, gate_in.dtype()));
            auto silu = mlx::core::multiply(
                clipped_gate, mlx::core::sigmoid(clipped_gate));
            auto clipped_up = mlx::core::clip(
                up_in, array(-7.0f, up_in.dtype()),
                array(7.0f, up_in.dtype()));
            return std::vector<array>{mlx::core::multiply(silu, clipped_up)};
        },
        true);
    return compiled({gate, up})[0];
}

} // namespace

MapleExpertMLXViews acquire_mlx_expert_views(MapleExpertStore& store,
                                             int layer,
                                             int expert) {
    auto lease = std::make_shared<MapleExpertLease>(store.acquire_expert(layer, expert));
    const MapleExpertRecord& record = store.record(layer, expert);

    MapleExpertMLXViews views;
    views.lease = lease;
    const int hidden = store.config().hidden_size;
    const int moe = store.config().moe_intermediate_size;
    views.gate_weight = make_region_view(
        lease, record, MapleExpertPart::GateWeight,
        {moe, hidden / 16}, mlx::core::uint32);
    views.gate_row_alpha = make_region_view(
        lease, record, MapleExpertPart::GateRowAlpha,
        {moe}, mlx::core::bfloat16);
    views.up_weight = make_region_view(
        lease, record, MapleExpertPart::UpWeight,
        {moe, hidden / 16}, mlx::core::uint32);
    views.up_row_alpha = make_region_view(
        lease, record, MapleExpertPart::UpRowAlpha,
        {moe}, mlx::core::bfloat16);
    views.down_weight = make_region_view(
        lease, record, MapleExpertPart::DownWeight,
        {hidden, moe / 16}, mlx::core::uint32);
    views.down_row_alpha = make_region_view(
        lease, record, MapleExpertPart::DownRowAlpha,
        {hidden}, mlx::core::bfloat16);
    return views;
}

array streamed_expert_output(const array& x,
                             MapleExpertStore& store,
                             int layer,
                             int expert,
                             float mlp_clamp) {
    MapleExpertMLXViews views = acquire_mlx_expert_views(store, layer, expert);
    const int hidden = store.config().hidden_size;
    if (x.ndim() != 2 || x.shape(1) != hidden) {
        throw std::runtime_error("streamed Maple expert input must be [tokens, hidden]");
    }

    /* Maple's reference fuses up and gate along the output-row axis before
     * running the quantized matmul.  Recreate that exact graph for the one
     * leased expert; the temporary is bounded to one expert and is evaluated
     * while the lease is pinned. */
    auto up_gate_weight = mlx::core::contiguous(mlx::core::concatenate(
        {views.up_weight, views.gate_weight}, 0));
    auto up_gate_row_alpha = mlx::core::contiguous(mlx::core::concatenate(
        {views.up_row_alpha, views.gate_row_alpha}, 0));
    mlx::core::eval(up_gate_weight, up_gate_row_alpha);
    auto up_gate_params = affine_params(
        up_gate_row_alpha, up_gate_weight.shape(-1) * 16);
    auto down_params = affine_params(
        views.down_row_alpha, views.down_weight.shape(-1) * 16);
    auto up_gate = mlx::core::quantized_matmul(
        x, up_gate_weight, up_gate_params.first, up_gate_params.second,
        true, 128, 2);
    const int moe = store.config().moe_intermediate_size;
    auto up = mlx::core::slice(up_gate, {0, 0}, {up_gate.shape(0), moe});
    auto gate = mlx::core::slice(
        up_gate, {0, moe}, {up_gate.shape(0), 2 * moe});
    auto act = compiled_clamped_swiglu(gate, up, mlp_clamp);
    auto down = mlx::core::quantized_matmul(
        act, views.down_weight, down_params.first, down_params.second,
        true, 128, 2);
    /* The lease-backed inputs must remain live through graph execution. */
    mlx::core::eval(down);
    return down;
}

array streamed_expert_outputs(const array& x,
                              MapleExpertStore& store,
                              int layer,
                              const std::vector<int>& experts,
                              float mlp_clamp) {
    const int hidden = store.config().hidden_size;
    const int moe = store.config().moe_intermediate_size;
    const int count = static_cast<int>(experts.size());
    if (x.ndim() != 2 || x.shape(0) != 1 || x.shape(1) != hidden) {
        throw std::runtime_error(
            "streamed Maple expert batch input must be [1, hidden]");
    }
    if (count <= 0) {
        throw std::runtime_error("streamed Maple expert batch is empty");
    }

    std::vector<MapleExpertMLXViews> selected;
    selected.reserve(experts.size());
    for (int expert : experts) {
        selected.push_back(acquire_mlx_expert_views(store, layer, expert));
    }

    std::vector<array> up_weights, gate_weights, up_alphas, gate_alphas;
    std::vector<array> down_weights, down_alphas;
    up_weights.reserve(experts.size());
    gate_weights.reserve(experts.size());
    up_alphas.reserve(experts.size());
    gate_alphas.reserve(experts.size());
    down_weights.reserve(experts.size());
    down_alphas.reserve(experts.size());
    for (const auto& views : selected) {
        up_weights.push_back(views.up_weight);
        gate_weights.push_back(views.gate_weight);
        up_alphas.push_back(views.up_row_alpha);
        gate_alphas.push_back(views.gate_row_alpha);
        down_weights.push_back(views.down_weight);
        down_alphas.push_back(views.down_row_alpha);
    }

    /* sanitize() builds [expert, up|gate rows, packed input] before the
     * SwitchLinear gather.  Stack the selected rows into that exact layout. */
    auto up_gate_weight = mlx::core::contiguous(mlx::core::concatenate(
        {mlx::core::stack(up_weights, 0), mlx::core::stack(gate_weights, 0)},
        1));
    auto up_gate_alpha = mlx::core::contiguous(mlx::core::concatenate(
        {mlx::core::stack(up_alphas, 0), mlx::core::stack(gate_alphas, 0)},
        1));
    auto down_weight = mlx::core::contiguous(
        mlx::core::stack(down_weights, 0));
    auto down_alpha = mlx::core::contiguous(
        mlx::core::stack(down_alphas, 0));
    mlx::core::eval(
        up_gate_weight, up_gate_alpha, down_weight, down_alpha);

    auto up_gate_params = affine_params(up_gate_alpha, hidden);
    auto x_expanded = mlx::core::reshape(x, {1, 1, 1, 1, hidden});
    auto compact_indices = mlx::core::reshape(
        mlx::core::arange(count), {1, 1, count});
    auto up_gate = mlx::core::gather_qmm(
        x_expanded,
        up_gate_weight,
        up_gate_params.first,
        up_gate_params.second,
        std::nullopt,
        compact_indices,
        true, 128, 2, "affine", false);
    up_gate = mlx::core::squeeze(up_gate, -2);
    auto up = mlx::core::slice(
        up_gate, {0, 0, 0, 0}, {1, 1, count, moe});
    auto gate = mlx::core::slice(
        up_gate, {0, 0, 0, moe}, {1, 1, count, 2 * moe});
    auto act = mlx::core::expand_dims(
        compiled_clamped_swiglu(gate, up, mlp_clamp), -2);

    auto down_params = affine_params(down_alpha, moe);
    auto outputs = mlx::core::gather_qmm(
        act,
        down_weight,
        down_params.first,
        down_params.second,
        std::nullopt,
        compact_indices,
        true, 128, 2, "affine", false);
    outputs = mlx::core::reshape(
        mlx::core::squeeze(outputs, -2), {1, count, hidden});

    /* All selected leases stay pinned through both batched matmuls. */
    mlx::core::eval(outputs);
    return outputs;
}

array streamed_expert_batch(const array& x,
                            MapleExpertStore& store,
                            int layer,
                            const std::vector<int>& experts,
                            const array& compact_indices,
                            bool sorted_indices,
                            float mlp_clamp) {
    const int hidden = store.config().hidden_size;
    const int moe = store.config().moe_intermediate_size;
    const int count = static_cast<int>(experts.size());
    const int routes = x.shape(0);
    if (x.ndim() != 3 || routes <= 0 || x.shape(1) != 1 ||
        x.shape(2) != hidden) {
        throw std::runtime_error(
            "streamed Maple expert route batch must be [routes, 1, hidden]");
    }
    if (count <= 0 || compact_indices.size() != static_cast<size_t>(routes)) {
        throw std::runtime_error("invalid compact Maple expert route batch");
    }

    struct CompactBuffers {
        std::vector<uint32_t> up_gate_weight;
        std::vector<mlx::core::bfloat16_t> up_gate_alpha;
        std::vector<uint32_t> down_weight;
        std::vector<mlx::core::bfloat16_t> down_alpha;
    };
    auto buffers = std::make_shared<CompactBuffers>();
    const size_t up_words = static_cast<size_t>(moe) * (hidden / 16);
    const size_t down_words = static_cast<size_t>(hidden) * (moe / 16);
    buffers->up_gate_weight.resize(static_cast<size_t>(count) * 2 * up_words);
    buffers->up_gate_alpha.resize(static_cast<size_t>(count) * 2 * moe);
    buffers->down_weight.resize(static_cast<size_t>(count) * down_words);
    buffers->down_alpha.resize(static_cast<size_t>(count) * hidden);

    /* Assemble the compact tensor in the exact sanitize() row order
     * [expert, up | gate, packed-input].  Only one source lease is held while
     * its six regions are copied, keeping transient SSD payload bounded. */
    for (int compact = 0; compact < count; ++compact) {
        const int expert = experts[static_cast<size_t>(compact)];
        auto lease = store.acquire_expert(layer, expert);
        const MapleExpertRecord& record = store.record(layer, expert);
        const auto copy_region = [&](MapleExpertPart part, void* dst,
                                     size_t expected_bytes) {
            const MapleExpertRegion& region =
                record.regions[static_cast<int>(part)];
            if (region.part != part || region.bytes != expected_bytes ||
                region.slab_offset > lease.bytes() ||
                region.bytes > lease.bytes() - region.slab_offset) {
                throw std::runtime_error(
                    "Maple compact expert region does not match manifest");
            }
            std::memcpy(dst, lease.data() + region.slab_offset, expected_bytes);
        };

        uint32_t* up_gate = buffers->up_gate_weight.data() +
            static_cast<size_t>(compact) * 2 * up_words;
        copy_region(MapleExpertPart::UpWeight, up_gate,
                    up_words * sizeof(uint32_t));
        copy_region(MapleExpertPart::GateWeight, up_gate + up_words,
                    up_words * sizeof(uint32_t));
        auto* alpha = buffers->up_gate_alpha.data() +
            static_cast<size_t>(compact) * 2 * moe;
        copy_region(MapleExpertPart::UpRowAlpha, alpha,
                    static_cast<size_t>(moe) * sizeof(*alpha));
        copy_region(MapleExpertPart::GateRowAlpha, alpha + moe,
                    static_cast<size_t>(moe) * sizeof(*alpha));
        uint32_t* down = buffers->down_weight.data() +
            static_cast<size_t>(compact) * down_words;
        copy_region(MapleExpertPart::DownWeight, down,
                    down_words * sizeof(uint32_t));
        auto* down_alpha = buffers->down_alpha.data() +
            static_cast<size_t>(compact) * hidden;
        copy_region(MapleExpertPart::DownRowAlpha, down_alpha,
                    static_cast<size_t>(hidden) * sizeof(*down_alpha));
    }

    std::function<void(void*)> keepalive = [buffers](void*) {};
    auto up_gate_weight = array(
        buffers->up_gate_weight.data(), {count, 2 * moe, hidden / 16},
        mlx::core::uint32, keepalive);
    auto up_gate_alpha = array(
        buffers->up_gate_alpha.data(), {count, 2 * moe},
        mlx::core::bfloat16, keepalive);
    auto down_weight = array(
        buffers->down_weight.data(), {count, hidden, moe / 16},
        mlx::core::uint32, keepalive);
    auto down_alpha = array(
        buffers->down_alpha.data(), {count, hidden},
        mlx::core::bfloat16, keepalive);

    auto up_gate_params = affine_params(up_gate_alpha, hidden);
    auto up_gate = mlx::core::gather_qmm(
        x, up_gate_weight, up_gate_params.first, up_gate_params.second,
        std::nullopt, compact_indices, true, 128, 2, "affine",
        sorted_indices);
    auto up = mlx::core::slice(
        up_gate, {0, 0, 0}, {routes, 1, moe});
    auto gate = mlx::core::slice(
        up_gate, {0, 0, moe}, {routes, 1, 2 * moe});
    auto act = compiled_clamped_swiglu(gate, up, mlp_clamp);

    auto down_params = affine_params(down_alpha, moe);
    auto outputs = mlx::core::gather_qmm(
        act, down_weight, down_params.first, down_params.second,
        std::nullopt, compact_indices, true, 128, 2, "affine",
        sorted_indices);
    outputs = mlx::core::reshape(outputs, {routes, hidden});
    /* The compact host buffers and every lease-backed copy are now fully
     * consumed; the returned hidden rows no longer reference SSD payload. */
    mlx::core::eval(outputs);
    return outputs;
}

array streamed_expert_contribution(const array& x,
                                   MapleExpertStore& store,
                                   int layer,
                                   int expert,
                                   float router_weight,
                                   float mlp_clamp) {
    auto down = streamed_expert_output(
        x, store, layer, expert, mlp_clamp);
    /* Match qwen36b's streamed MoE contract: expert outputs are accumulated
     * in float32 and rounded only once after the final routed expert.  Keeping
     * this contribution in float32 costs one hidden-size vector, not another
     * expert, and prevents expert-order-dependent BF16 rounding. */
    auto contribution = mlx::core::multiply(
        mlx::core::astype(down, mlx::core::float32),
        mlx::core::array(router_weight, mlx::core::float32));

    mlx::core::eval(contribution);
    return contribution;
}

} // namespace maple
} // namespace samosa
