#include "mlx/mlx.h"
#include "../src/maple/maple_model.h"
#include <iostream>
#include <mach/mach.h>

using namespace mlx::core;
using namespace samosa::maple;

size_t get_footprint_mb() {
    task_vm_info_data_t vm_info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm_info, &count) == KERN_SUCCESS) {
        return vm_info.phys_footprint / (1024 * 1024);
    }
    return 0;
}

int main() {
    std::cout << "[Gate A] Real Checkpoint Load Test\n";
    std::cout << "Initial Footprint: " << get_footprint_mb() << " MB\n";
    
    std::string model_dir = "models/maple";
    
    try {
        std::cout << "Loading MapleModel from: " << model_dir << "\n";
        MapleModel model = load_maple_model(model_dir);
        
        std::cout << "Model successfully instantiated.\n";
        std::cout << "Final Footprint: " << get_footprint_mb() << " MB\n";
        
        if (get_footprint_mb() > 200) {
            std::cerr << "FAIL: Memory footprint exceeded 200 MB, load is not mmap safe!\n";
            return 1;
        }
        std::cout << "PASS: Model loaded safely with minimal resident memory footprint.\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: Exception thrown during load: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
