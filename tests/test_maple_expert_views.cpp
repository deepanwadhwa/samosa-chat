#include "maple/maple_expert_views.h"
#include "maple/maple_model.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

using mlx::core::array;
using mlx::core::bfloat16;
using mlx::core::float32;
using mlx::core::uint32;
using samosa::maple::MapleExpertCacheConfig;
using samosa::maple::MapleExpertMLXViews;
using samosa::maple::MapleExpertStore;

namespace {

constexpr int kHidden = 128;
constexpr int kMoe = 128;
constexpr int kExperts = 32;
constexpr size_t kWeightBytes = static_cast<size_t>(kMoe) * (kHidden / 16) * 4;
constexpr size_t kAlphaBytes = static_cast<size_t>(kMoe) * 2;
constexpr size_t kLogicalBytes = 3 * (kWeightBytes + kAlphaBytes);
constexpr size_t kRecordBytes = 16384;

static void die_errno(const char* what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

static void write_all(int fd, const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = ::write(fd, p + done, bytes - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            die_errno("write");
        }
        if (n == 0) throw std::runtime_error("short write");
        done += static_cast<size_t>(n);
    }
}

static std::string make_temp_dir() {
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base ? base : "/tmp") + "/maple-expert-views.XXXXXX";
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back('\0');
    char* made = ::mkdtemp(path.data());
    if (!made) die_errno("mkdtemp");
    return made;
}

static void write_text(const std::string& path, const std::string& text) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) die_errno("open manifest");
    write_all(fd, text.data(), text.size());
    if (::close(fd) != 0) die_errno("close manifest");
}

static std::string make_fixture() {
    const std::string dir = make_temp_dir();
    int fd = ::open((dir + "/maple-experts.bin").c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) die_errno("open expert fixture");

    std::vector<uint32_t> packed(kWeightBytes / sizeof(uint32_t));
    for (size_t i = 0; i < packed.size(); ++i) {
        /* Four distinct 2-bit values in each packed word. */
        packed[i] = 0xE4E4E4E4u ^ static_cast<uint32_t>(i * 0x01010101u);
    }
    std::vector<uint16_t> alpha(kMoe, 0x3f80u); /* BF16 1.0 */
    std::vector<uint8_t> padding(kRecordBytes - kLogicalBytes, 0);
    for (int expert = 0; expert < kExperts; ++expert) {
        write_all(fd, packed.data(), kWeightBytes);
        write_all(fd, alpha.data(), kAlphaBytes);
        write_all(fd, packed.data(), kWeightBytes);
        write_all(fd, alpha.data(), kAlphaBytes);
        write_all(fd, packed.data(), kWeightBytes);
        write_all(fd, alpha.data(), kAlphaBytes);
        write_all(fd, padding.data(), padding.size());
    }
    if (::close(fd) != 0) die_errno("close expert fixture");

    write_text(dir + "/maple-manifest.json",
               "{\"schema\":\"maple-ssd-streaming-v1\","
               "\"experts_file\":\"maple-experts.bin\","
               "\"num_layers\":1,\"first_moe_layer\":0,\"num_experts\":32,"
               "\"hidden_size\":128,\"moe_intermediate_size\":128,"
               "\"expert_logical_bytes\":" + std::to_string(kLogicalBytes) + ","
               "\"expert_record_bytes\":" + std::to_string(kRecordBytes) + "}");
    return dir;
}

static std::pair<array, array> affine_params(const array& row_alpha) {
    auto scales = mlx::core::broadcast_to(
        mlx::core::expand_dims(row_alpha, -1), {row_alpha.shape(0), 1});
    return {scales, mlx::core::negative(scales)};
}

static void assert_close(const array& actual, const array& expected,
                         const char* label, float tolerance = 1e-5f) {
    auto error = mlx::core::max(mlx::core::abs(actual - expected));
    mlx::core::eval(error);
    const float max_error = error.item<float>();
    if (max_error > tolerance) {
        throw std::runtime_error(std::string(label) + " mismatch (max error " +
                                 std::to_string(max_error) + ")");
    }
}

static bool run_test() {
    try {
        if (!mlx::core::gpu::is_available()) {
            std::puts("SKIP: Maple MLX expert-view test requires a Metal device");
            return false;
        }
        (void)mlx::core::device_info(mlx::core::Device::gpu);
    } catch (const std::exception& e) {
        if (std::string(e.what()).find("No Metal device") != std::string::npos) {
            std::puts("SKIP: Maple MLX expert-view test requires a Metal device");
            return false;
        }
        throw;
    }
    /* The streamed block uses the Metal fused router.  The raw expert
     * buffers remain host-backed, but MLX stages them as needed for GPU ops. */
    mlx::core::set_default_device(mlx::core::Device::gpu);
    const std::string dir = make_fixture();
    try {
        MapleExpertStore store = MapleExpertStore::open_packed(dir);
        MapleExpertCacheConfig cache_config;
        cache_config.budget_bytes = kRecordBytes;
        cache_config.payload_alignment = 16384;
        cache_config.max_entries = 1;
        store.enable_cache(cache_config);

        MapleExpertMLXViews views =
            samosa::maple::acquire_mlx_expert_views(store, 0, 0);
        if (!views || views.gate_weight.shape() != mlx::core::Shape{128, 8} ||
            views.gate_row_alpha.shape() != mlx::core::Shape{128} ||
            views.down_weight.shape() != mlx::core::Shape{128, 8} ||
            views.gate_weight.dtype() != uint32 ||
            views.gate_row_alpha.dtype() != bfloat16) {
            throw std::runtime_error("Maple MLX expert view shape or dtype mismatch");
        }

        uint64_t reclaimed = 0;
        auto pressure = store.apply_cache_pressure(
            ECACHE_PRESSURE_CRITICAL, 0, &reclaimed);
        if (pressure != ECACHE_PARTIAL || reclaimed != 0) {
            throw std::runtime_error("critical pressure evicted a live Maple view");
        }

        std::vector<float> input_data(128);
        for (size_t i = 0; i < input_data.size(); ++i) {
            input_data[i] = static_cast<float>(i % 17) * 0.03125f - 0.25f;
        }
        array input(input_data.begin(), {1, 128}, float32);

        auto gate_params = affine_params(views.gate_row_alpha);
        auto up_params = affine_params(views.up_row_alpha);
        auto down_params = affine_params(views.down_row_alpha);
        auto gate_stream = mlx::core::quantized_matmul(
            input, views.gate_weight, gate_params.first, gate_params.second,
            true, 128, 2);
        auto up_stream = mlx::core::quantized_matmul(
            input, views.up_weight, up_params.first, up_params.second,
            true, 128, 2);
        auto act_stream = samosa::maple::clamped_swiglu(
            gate_stream, up_stream, 7.0f);
        auto down_stream = mlx::core::quantized_matmul(
            act_stream, views.down_weight, down_params.first, down_params.second,
            true, 128, 2);
        auto weighted_stream = down_stream * array(0.25f);

        /* The streamed and resident arrays are built from the same record
         * bytes.  This is the fixture-scale one-expert oracle used before
         * replacing the production take() dispatch. */
        std::vector<uint32_t> packed(kWeightBytes / sizeof(uint32_t));
        for (size_t i = 0; i < packed.size(); ++i) {
            packed[i] = 0xE4E4E4E4u ^ static_cast<uint32_t>(i * 0x01010101u);
        }
        array resident_weight(packed.begin(), {128, 8}, uint32);
        std::vector<mlx::core::bfloat16_t> resident_alpha_data(
            kMoe, mlx::core::bfloat16_t(1.0f));
        array resident_alpha(resident_alpha_data.begin(), {128}, bfloat16);
        auto resident_params = affine_params(resident_alpha);
        auto gate_resident = mlx::core::quantized_matmul(
            input, resident_weight, resident_params.first, resident_params.second,
            true, 128, 2);
        auto up_resident = mlx::core::quantized_matmul(
            input, resident_weight, resident_params.first, resident_params.second,
            true, 128, 2);
        auto act_resident = samosa::maple::clamped_swiglu(
            gate_resident, up_resident, 7.0f);
        auto down_resident = mlx::core::quantized_matmul(
            act_resident, resident_weight, resident_params.first, resident_params.second,
            true, 128, 2);
        auto weighted_resident = down_resident * array(0.25f);
        mlx::core::eval(gate_stream, up_stream, act_stream, down_stream,
                        weighted_stream, gate_resident, up_resident,
                        act_resident, down_resident, weighted_resident);
        assert_close(gate_stream, gate_resident, "gate projection");
        assert_close(up_stream, up_resident, "up projection");
        assert_close(act_stream, act_resident, "SwiGLU");
        assert_close(down_stream, down_resident, "down projection");
        assert_close(weighted_stream, weighted_resident, "weighted contribution");

        auto helper_contribution = samosa::maple::streamed_expert_contribution(
            input, store, 0, 0, 0.25f);
        mlx::core::eval(helper_contribution);
        assert_close(helper_contribution, weighted_resident,
                     "evaluated streamed expert helper");

        /* Exercise the production-shaped one-layer MoE dispatch as well as
         * the isolated expert oracle above.  The resident block and streamed
         * block receive identical router and packed projection tensors. */
        std::vector<uint32_t> stacked_packed(kExperts * packed.size());
        std::vector<mlx::core::bfloat16_t> stacked_alpha_data(
            kExperts * kMoe, mlx::core::bfloat16_t(1.0f));
        for (int expert = 0; expert < kExperts; ++expert) {
            std::copy(packed.begin(), packed.end(),
                      stacked_packed.begin() + expert * packed.size());
        }
        std::vector<float> router_data(kExperts * kHidden, 0.0f);
        std::unordered_map<std::string, array> block_weights;
        block_weights.emplace(
            "mlp.gate.weight",
            array(router_data.begin(), {kExperts, kHidden}, bfloat16));
        auto stacked_weight = array(
            stacked_packed.begin(), {kExperts, kMoe, kHidden / 16}, uint32);
        auto stacked_row_alpha = array(
            stacked_alpha_data.begin(), {kExperts, kMoe}, bfloat16);
        block_weights.emplace("mlp.switch_mlp.gate_proj.weight", stacked_weight);
        block_weights.emplace("mlp.switch_mlp.gate_proj.row_alpha", stacked_row_alpha);
        block_weights.emplace("mlp.switch_mlp.up_proj.weight", stacked_weight);
        block_weights.emplace("mlp.switch_mlp.up_proj.row_alpha", stacked_row_alpha);
        block_weights.emplace("mlp.switch_mlp.down_proj.weight", stacked_weight);
        block_weights.emplace("mlp.switch_mlp.down_proj.row_alpha", stacked_row_alpha);

        samosa::maple::ModelArgs block_args;
        block_args.hidden_size = kHidden;
        block_args.moe_intermediate_size = kMoe;
        block_args.num_hidden_layers = 1;
        block_args.num_experts = kExperts;
        block_args.num_experts_per_tok = 8;
        block_args.first_k_dense_replace = 0;
        samosa::maple::MapleSparseMoeBlock resident_block(block_args, 0);
        resident_block.load_weights(block_weights, "");
        samosa::maple::MapleSparseMoeBlock streamed_block(block_args, 0);
        streamed_block.load_streaming_weights(block_weights, "", &store);

        auto block_input = mlx::core::reshape(input, {1, 1, kHidden});
        auto resident_block_out = resident_block(block_input);
        auto streamed_block_out = streamed_block(block_input);
        mlx::core::eval(resident_block_out, streamed_block_out);
        /* The production streamed block uses gather_qmm while this synthetic
         * resident oracle takes eight RHS tensors before quantized_matmul.
         * Their FP32 aggregate can differ below one BF16 ULP because Metal
         * tiles those two graphs differently.  The real streamed checkpoint
         * is covered separately by the bit-exact Python-logit oracle. */
        assert_close(streamed_block_out, resident_block_out,
                     "one-layer streamed MoE aggregate", 2e-5f);

        /* All lazy consumers are evaluated before the view/lease is released. */
        streamed_block_out = array(0.0f);
        resident_block_out = array(0.0f);
        block_input = array(0.0f);
        stacked_weight = array(0.0f);
        stacked_row_alpha = array(0.0f);
        helper_contribution = array(0.0f);
        weighted_stream = array(0.0f);
        down_stream = array(0.0f);
        act_stream = array(0.0f);
        up_stream = array(0.0f);
        gate_stream = array(0.0f);
        gate_params = {array(0.0f), array(0.0f)};
        up_params = {array(0.0f), array(0.0f)};
        down_params = {array(0.0f), array(0.0f)};
        views = MapleExpertMLXViews{};
        pressure = store.apply_cache_pressure(
            ECACHE_PRESSURE_CRITICAL, 0, &reclaimed);
        if (pressure != ECACHE_OK || reclaimed != 16384) {
            throw std::runtime_error("released Maple view did not become reclaimable");
        }
        store.disable_cache();
    } catch (...) {
        std::filesystem::remove_all(dir);
        throw;
    }
    std::filesystem::remove_all(dir);
    return true;
}

} // namespace

int main() {
    try {
        if (!run_test()) return 0;
        std::puts("PASS: Maple MLX views preserve lease lifetime and projection parity");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
