#include "visionpsy/visionpsy_model.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/resource.h>
#include <vector>

using samosa::visionpsy::ImageTiles;
using samosa::visionpsy::InferenceResult;
using samosa::visionpsy::VisionPsyModel;

static int fail(const std::string& message) {
    std::cerr << "VisionPsy real-checkpoint gate: FAIL: " << message << "\n";
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) return fail("usage: test-visionpsy-real MODEL_DIR FIXTURE.ppm");

    VisionPsyModel model;
    if (!model.load(argv[1])) return fail("checkpoint load rejected: " + model.last_error());

    std::ifstream input(argv[2], std::ios::binary);
    std::vector<unsigned char> image((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    if (image.empty()) return fail("rendered fixture is empty");

    ImageTiles tiles;
    if (!model.preprocess_image_tiles(image.data(), image.size(), 512, &tiles))
        return fail("real fixture preprocessing failed");
    if (tiles.n_w != 1 || tiles.n_h != 1 || tiles.has_global || tiles.tiles.size() != 1)
        return fail("single-tile processor contract changed");

    std::vector<int> prompt = model.build_prompt_tokens(
        "Read the large heading on this page. Answer briefly.", tiles);
    if (std::find(prompt.begin(), prompt.end(), model.global_image_token_id()) != prompt.end())
        return fail("single-tile prompt incorrectly contains the global image token");
    if (std::count(prompt.begin(), prompt.end(), model.image_token_id()) != 64)
        return fail("single-tile prompt does not contain exactly 64 image placeholders");

    /* 1536 is the admission tier selected on a healthy 16 GB Apple-Silicon
       machine. Exercise it with the real graph, not only the emergency 512
       tier, while an RSS ceiling leaves room for Samosa and its active LLM. */
    ImageTiles qualified_tiles;
    if (!model.preprocess_image_tiles(image.data(), image.size(), 1536, &qualified_tiles))
        return fail("16 GB qualification-tier preprocessing failed");
    InferenceResult result = model.generate_from_tiles(
        qualified_tiles, "Read the large heading on this page. Answer briefly.", 32);
    if (!result.error.empty()) return fail("inference error: " + result.error);
    std::string answer = result.text;
    std::transform(answer.begin(), answer.end(), answer.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (answer.find("hello") == std::string::npos)
        return fail("fixture answer did not contain the expected heading; got: " + result.text);

    struct rusage usage = {};
    getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    double peak_mb = (double)usage.ru_maxrss / (1024.0 * 1024.0);
#else
    double peak_mb = (double)usage.ru_maxrss / 1024.0;
#endif
    if (peak_mb > 3072.0)
        return fail("16 GB qualification tier exceeded the 3072 MB helper RSS ceiling");
    std::cout << "VisionPsy real-checkpoint gate: PASS"
              << " (answer=\"" << result.text << "\", peak_rss_mb=" << peak_mb
              << ", prefill_ms=" << result.prefill_ms
              << ", decode_ms=" << result.decode_ms << ")\n";
    return 0;
}
