#include "mlx/mlx.h"
#include "../src/maple/maple_model.h"
#include <iostream>
#include <cassert>

using namespace mlx::core;
using namespace samosa::maple;

void assert_allclose(const array& a, const array& b, const std::string& label, float atol=1e-3, float rtol=1e-3) {
    auto diff = abs(subtract(a, b));
    eval(diff);
    auto max_diff = max(diff).item<float>();
    std::cout << label << " - Max Abs Error: " << max_diff << "\n";
}

int main() {
    std::string model_dir = "models/maple";
    MapleModel model = load_maple_model(model_dir);

    auto fixture = load_safetensors("tests/fixtures/maple/gate_c_debug.safetensors").first;
    auto x = fixture.at("x");

    std::vector<KVCache*> caches;
    for (const auto& lt : model.args().layer_types) {
        if (lt == "sliding_attention") caches.push_back(new RotatingKVCache(model.args().sliding_window));
        else caches.push_back(new KVCache());
    }

    auto h = model.word_embeddings_(x);
    eval(h);
    assert_allclose(h, fixture.at("emb"), "Word Embeddings");

    auto mask = create_attention_mask(x, caches);

    h = model.layers_[0](h, mask, caches[0]);
    eval(h);
    assert_allclose(h, fixture.at("h_layer0"), "Layer 0 Out");

    h = model.layers_[1](h, mask, caches[1]);
    eval(h);
    assert_allclose(h, fixture.at("h_layer1"), "Layer 1 Out");

    return 0;
}
