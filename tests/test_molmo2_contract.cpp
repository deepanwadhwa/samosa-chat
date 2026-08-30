#include "molmo2/molmo2_contract.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace samosa::molmo2;

int main() {
    Config config;
    assert(config.hidden_size == 2560 && config.text_layers == 36);
    assert(config.vision_required_layers == 25);
    assert(config.selected_vision_layers[0] == 24 && config.selected_vision_layers[1] == 18);

    assert(expected_tensor_count() == 706);
    assert(canonical_source_tensor_name("lm_head.weight") ==
           "language_model.lm_head.weight");
    assert(canonical_source_tensor_name("model.transformer.blocks.35.mlp.ff_out.weight") ==
           "language_model.model.blocks.35.mlp.ff_out.weight");
    assert(canonical_source_tensor_name(
               "model.vision_backbone.image_vit.transformer.resblocks.24.attention.wq.weight") ==
           "vision_tower.image_vit.transformer.24.attention.wq.weight");
    assert(canonical_source_tensor_name("unrelated.weight").empty());
    std::vector<int> shape;
    assert(expected_tensor_shape("language_model.model.blocks.0.self_attn.att_proj.weight",
                                 &shape));
    assert((shape == std::vector<int>{6144, 2560}));
    assert(expected_tensor_shape("vision_tower.image_pooling_2d.wq.weight", &shape));
    assert((shape == std::vector<int>{1152, 2304}));
    assert(package_tensor_is_q4("language_model.model.wte.embedding"));
    assert(package_tensor_is_q4("language_model.model.blocks.0.mlp.ff_proj.weight"));
    assert(!package_tensor_is_q4("vision_tower.image_projector.w1.weight"));
    assert(estimated_tensor_payload_bytes() == 3360597920ULL);

    std::vector<std::uint8_t> types = {0, 1, 1, 0};
    auto mask = build_prefill_attention_mask(types);
    assert(mask.size() == 16);
    assert(mask[0 * 4 + 1] < -1e8f);   // text cannot see the future
    assert(mask[1 * 4 + 2] == 0.0f);  // visual block is bidirectional
    assert(mask[2 * 4 + 1] == 0.0f);
    assert(mask[3 * 4 + 3] == 0.0f);

    auto short_times = uniform_last_frame_times(0.0, 1.0, 8, 2.0);
    assert(short_times.size() == 3);
    assert(short_times.front() == 0.0 && short_times.back() == 1.0);
    auto long_times = uniform_last_frame_times(10.0, 100.0, 8, 2.0);
    assert(long_times.size() == 8);
    assert(long_times.front() == 10.0 && long_times.back() == 100.0);

    auto windows = plan_frame_windows(0.0, 40.0, 16, 2.0, 1.0);
    assert(windows.size() > 1);
    assert(windows.front().start_seconds == 0.0);
    assert(windows.back().end_seconds == 40.0);
    for (std::size_t i = 1; i < windows.size(); ++i) {
        assert(windows[i].start_seconds < windows[i - 1].end_seconds);
        assert(std::abs((windows[i - 1].end_seconds - windows[i].start_seconds) - 1.0) < 1e-9);
    }
    std::cout << "molmo2 contract: PASS\n";
    return 0;
}
