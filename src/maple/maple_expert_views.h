#pragma once

#include <memory>
#include <vector>

#include "maple_expert_store.h"
#include "mlx/mlx.h"

namespace samosa {
namespace maple {

/*
 * MLX views over one streamed expert.  The six arrays point at the exact
 * packed U32/BF16 regions owned by the MapleExpertLease.  The shared lease is
 * retained both by this object and by each raw-buffer deleter, so a lazy MLX
 * graph can outlive the local view object without exposing an evictable cache
 * entry to the graph.
 *
 * The store must outlive the views.  Callers must evaluate the expert
 * contribution before releasing the views when using a backend that retains
 * an external buffer for lazy execution.
 */
struct MapleExpertMLXViews {
    std::shared_ptr<MapleExpertLease> lease;
    mlx::core::array gate_weight{0.0f};
    mlx::core::array gate_row_alpha{0.0f};
    mlx::core::array up_weight{0.0f};
    mlx::core::array up_row_alpha{0.0f};
    mlx::core::array down_weight{0.0f};
    mlx::core::array down_row_alpha{0.0f};

    explicit operator bool() const {
        return lease && gate_weight.ndim() == 2 && gate_row_alpha.ndim() == 1 &&
               up_weight.ndim() == 2 && up_row_alpha.ndim() == 1 &&
               down_weight.ndim() == 2 && down_row_alpha.ndim() == 1;
    }
};

/* Acquire one expert and expose its six exact packed regions as MLX arrays. */
MapleExpertMLXViews acquire_mlx_expert_views(MapleExpertStore& store,
                                             int layer,
                                             int expert);

/* Evaluate one routed expert to a BF16 hidden vector before releasing its
 * lease.  Callers can retain these tiny vectors and reproduce Maple's single
 * FP32 top-k reduction without retaining any expert weights. */
mlx::core::array streamed_expert_output(const mlx::core::array& x,
                                        MapleExpertStore& store,
                                        int layer,
                                        int expert,
                                        float mlp_clamp = 7.0f);

/* Evaluate one token against its routed expert set in the same batched
 * quantized kernels used by Maple's reference implementation.  Only these
 * selected leases are pinned (eight, or ~6.3 MiB for the real model). */
mlx::core::array streamed_expert_outputs(
    const mlx::core::array& x,
    MapleExpertStore& store,
    int layer,
    const std::vector<int>& experts,
    float mlp_clamp = 7.0f);

/* Evaluate a sorted prefill route batch with a compact RHS containing only
 * the experts selected by this bounded token chunk.  Expert records are
 * copied one at a time into the compact buffer, so leases never accumulate
 * and the full 256-expert layer is never resident. */
mlx::core::array streamed_expert_batch(
    const mlx::core::array& x,
    MapleExpertStore& store,
    int layer,
    const std::vector<int>& experts,
    const mlx::core::array& compact_indices,
    bool sorted_indices,
    float mlp_clamp = 7.0f);

/* Evaluate one routed expert's weighted float32 contribution before releasing
 * its lease.  The dispatcher keeps only a hidden-size float32 accumulator and
 * rounds once after the final expert, matching the bounded Qwen path. */
mlx::core::array streamed_expert_contribution(const mlx::core::array& x,
                                              MapleExpertStore& store,
                                              int layer,
                                              int expert,
                                              float router_weight,
                                              float mlp_clamp = 7.0f);

} // namespace maple
} // namespace samosa
