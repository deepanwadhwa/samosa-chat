import re

with open("tests/test_maple_components.cpp", "r") as f:
    code = f.read()

# I want to find the router block and inject prints.
new_code = code.replace("""        bool idx_match = array_equal(sorted_actual, sorted_expected).item<bool>();""", """        bool idx_match = array_equal(sorted_actual, sorted_expected).item<bool>();

        // Print reference top-8 and native top-8
        std::cout << "  - Reference Top-8 IDs (Row 0): ";
        for (int i=0; i<8; i++) std::cout << sorted_expected.data<uint32_t>()[i] << " ";
        std::cout << "\n  - Native Top-8 IDs (Row 0): ";
        for (int i=0; i<8; i++) std::cout << sorted_actual.data<uint32_t>()[i] << " ";
        std::cout << "\n  - Exact ID Match: " << (idx_match ? "PASS" : "FAIL") << "\n";
        """)

with open("tests/test_maple_components.cpp", "w") as f:
    f.write(new_code)
