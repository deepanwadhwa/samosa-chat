#include "mlx/mlx.h"
#include <iostream>
#include <mach/mach.h>

using namespace mlx::core;

void print_memory_usage(const std::string& label) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &infoCount) == KERN_SUCCESS) {
        std::cout << label << " Memory: " << info.resident_size / (1024 * 1024) << " MB\n";
    }
}

int main() {
    print_memory_usage("Initial");

    std::unordered_map<std::string, mlx::core::array> weights;
    auto data1 = load_safetensors("models/maple/model-00001-of-00003.safetensors").first;
    weights.insert(data1.begin(), data1.end());
    print_memory_usage("After loading shard 1 (2GB)");

    auto data2 = load_safetensors("models/maple/model-00002-of-00003.safetensors").first;
    weights.insert(data2.begin(), data2.end());
    print_memory_usage("After loading shard 2 (2GB)");

    auto data3 = load_safetensors("models/maple/model-00003-of-00003.safetensors").first;
    weights.insert(data3.begin(), data3.end());
    print_memory_usage("After loading shard 3 (914MB)");

    std::cout << "Total keys: " << weights.size() << "\n";
    return 0;
}
