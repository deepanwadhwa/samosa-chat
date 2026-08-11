#include "mlx/mlx.h"
#include <iostream>

using namespace mlx::core;

int main() {
    auto dict = load_safetensors("models/maple/model-00001-of-00003.safetensors").first;
    auto w = dict.at("model.word_embeddings.weight");
    auto s = dict.at("model.word_embeddings.scales");
    auto b = dict.at("model.word_embeddings.biases");

    array inputs({10}, int32);
    auto w_x = take(w, inputs, 0);
    auto s_x = take(s, inputs, 0);
    auto b_x = take(b, inputs, 0);

    auto dq2 = dequantize(w_x, s_x, b_x, 64, 4, "affine", std::nullopt, bfloat16);
    eval(dq2);

    // cast to float32 before printing!
    auto dq2_f32 = astype(dq2, float32);
    eval(dq2_f32);
    std::cout << "DQ Mean casted bf16->f32: " << mean(abs(dq2_f32)).item<float>() << std::endl;
    return 0;
}
