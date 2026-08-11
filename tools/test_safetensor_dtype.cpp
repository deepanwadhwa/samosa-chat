#include "mlx/mlx.h"
#include <iostream>

using namespace mlx::core;

int main() {
    auto dict = load_safetensors("models/maple/model-00001-of-00003.safetensors").first;
    auto w = dict.at("model.word_embeddings.weight");
    std::cout << "w dtype: " << size_of(w.dtype()) << " bytes, kind: " << (int)w.dtype().kind << std::endl;
    return 0;
}
