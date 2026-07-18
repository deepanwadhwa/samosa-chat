#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "kernels.h"

/*
 * E-X10 M0: standalone Metal feasibility spike.
 *
 * This executable never initializes the model engine and never changes
 * qwen36b. It measures the production grouped-q4/i8-dot arithmetic, a bounded
 * real-file mmap, page-aligned no-copy storage, and the CPU/GPU event tax.
 */

enum {
    REAL_D = 2048,
    REAL_I = 512,
    REAL_E = 256,
    ROUTED_K = 8,
    MODEL_LAYERS = 40,
    QGROUP = 32,
    GPU_SIMDGROUPS = 8,
    GPU_THREADS = 256,
};

typedef struct {
    uint32_t S;
    uint32_t I;
    uint32_t O;
    uint32_t group;
    uint32_t row_bytes;
    uint32_t groups;
    uint32_t q_offset;
    uint32_t scale_offset;
} GpuParams;

typedef struct {
    uint32_t D;
    uint32_t I;
    uint32_t group;
    uint32_t experts;
    uint32_t expert_stride;
    uint32_t gate_q;
    uint32_t gate_s;
    uint32_t up_q;
    uint32_t up_s;
    uint32_t down_q;
    uint32_t down_s;
} FullParams;

typedef struct {
    uint8_t *blob;
    size_t blob_bytes;
    uint8_t *q4;
    float *scales;
    int I;
    int O;
    int group;
    size_t q_bytes;
    size_t scale_bytes;
} Q4Matrix;

typedef struct {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> q4_pipeline;
    id<MTLComputePipelineState> gate_up_pipeline;
    id<MTLComputePipelineState> quantize_pipeline;
    id<MTLComputePipelineState> down_pipeline;
    id<MTLComputePipelineState> read_pipeline;
    id<MTLComputePipelineState> ping_pipeline;
} MetalContext;

static const char *kMetalSource =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Params {\n"
"  uint S, I, O, group, row_bytes, groups, q_offset, scale_offset;\n"
"};\n"
"struct FullParams {\n"
"  uint D, I, group, experts, expert_stride;\n"
"  uint gate_q, gate_s, up_q, up_s, down_q, down_s;\n"
"};\n"
"kernel void grouped_q4_i8(\n"
"    const device uchar *blob [[buffer(0)]],\n"
"    const device char *xq [[buffer(1)]],\n"
"    const device float *sx [[buffer(2)]],\n"
"    device float *y [[buffer(3)]],\n"
"    constant Params &p [[buffer(4)]],\n"
"    uint2 tg [[threadgroup_position_in_grid]],\n"
"    uint lane [[thread_index_in_simdgroup]],\n"
"    uint simd [[simdgroup_index_in_threadgroup]]) {\n"
"  const uint o = tg.x * 8u + simd;\n"
"  const uint s = tg.y;\n"
"  if (o >= p.O || s >= p.S) return;\n"
"  float total = 0.0f;\n"
"  for (uint g = 0; g < p.groups; ++g) {\n"
"    const uint i = g * p.group + lane;\n"
"    int product = 0;\n"
"    if (i < p.I) {\n"
"      const uchar packed = blob[p.q_offset + o * p.row_bytes + (i >> 1)];\n"
"      const int w = int((i & 1u) ? (packed >> 4) : (packed & 15u)) - 8;\n"
"      product = w * int(xq[s * p.I + i]);\n"
"    }\n"
"    const int dot = simd_sum(product);\n"
"    if (lane == 0u)\n"
"      total += float(dot) *\n"
"          *((const device float *)(blob + p.scale_offset) +\n"
"            o * p.groups + g);\n"
"  }\n"
"  if (lane == 0u) y[s * p.O + o] = total * sx[s];\n"
"}\n"
"kernel void fused_gate_up(\n"
"    const device uchar *experts [[buffer(0)]],\n"
"    const device char *xq [[buffer(1)]],\n"
"    constant float &sx [[buffer(2)]],\n"
"    device float *hidden [[buffer(3)]],\n"
"    constant FullParams &p [[buffer(4)]],\n"
"    uint2 tg [[threadgroup_position_in_grid]],\n"
"    uint lane [[thread_index_in_simdgroup]],\n"
"    uint simd [[simdgroup_index_in_threadgroup]]) {\n"
"  const uint o = tg.x * 8u + simd;\n"
"  const uint e = tg.y;\n"
"  if (o >= p.I || e >= p.experts) return;\n"
"  const uint base = e * p.expert_stride;\n"
"  const uint groups = p.D / p.group;\n"
"  const uint row_bytes = p.D / 2u;\n"
"  float gate_lane = 0.0f, up_lane = 0.0f;\n"
"  for (uint g = 0; g < groups; ++g) {\n"
"    const uint i = g * p.group + lane;\n"
"    const uchar gb = experts[base + p.gate_q + o * row_bytes + (i >> 1)];\n"
"    const uchar ub = experts[base + p.up_q + o * row_bytes + (i >> 1)];\n"
"    const int gw = int((i & 1u) ? (gb >> 4) : (gb & 15u)) - 8;\n"
"    const int uw = int((i & 1u) ? (ub >> 4) : (ub & 15u)) - 8;\n"
"    const int xv = int(xq[i]);\n"
"    const device float *gs =\n"
"        (const device float *)(experts + base + p.gate_s);\n"
"    const device float *us =\n"
"        (const device float *)(experts + base + p.up_s);\n"
"    gate_lane += float(gw * xv) * gs[o * groups + g];\n"
"    up_lane += float(uw * xv) * us[o * groups + g];\n"
"  }\n"
"  const float gate_sum = simd_sum(gate_lane);\n"
"  const float up_sum = simd_sum(up_lane);\n"
"  if (lane == 0u) {\n"
"    const float gate = gate_sum * sx;\n"
"    const float up = up_sum * sx;\n"
"    hidden[e * p.I + o] = (gate / (1.0f + exp(-gate))) * up;\n"
"  }\n"
"}\n"
"kernel void quantize_hidden(\n"
"    const device float *hidden [[buffer(0)]],\n"
"    device char *hidden_q [[buffer(1)]],\n"
"    device float *hidden_s [[buffer(2)]],\n"
"    constant FullParams &p [[buffer(3)]],\n"
"    uint e [[threadgroup_position_in_grid]],\n"
"    uint tid [[thread_index_in_threadgroup]],\n"
"    threadgroup float *scratch [[threadgroup(0)]]) {\n"
"  float maximum = 0.0f;\n"
"  for (uint i = tid; i < p.I; i += 256u)\n"
"    maximum = max(maximum, abs(hidden[e * p.I + i]));\n"
"  scratch[tid] = maximum;\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  for (uint width = 128u; width > 0u; width >>= 1u) {\n"
"    if (tid < width) scratch[tid] = max(scratch[tid], scratch[tid + width]);\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  }\n"
"  float scale = max(scratch[0] / 127.0f, 1.0e-12f);\n"
"  if (tid == 0u) hidden_s[e] = scale;\n"
"  const float inverse = 1.0f / scale;\n"
"  for (uint i = tid; i < p.I; i += 256u) {\n"
"    int value = int(rint(hidden[e * p.I + i] * inverse));\n"
"    value = clamp(value, -127, 127);\n"
"    hidden_q[e * p.I + i] = char(value);\n"
"  }\n"
"}\n"
"kernel void fused_down_reduce(\n"
"    const device uchar *experts [[buffer(0)]],\n"
"    const device char *hidden_q [[buffer(1)]],\n"
"    const device float *hidden_s [[buffer(2)]],\n"
"    const device float *route [[buffer(3)]],\n"
"    device float *output [[buffer(4)]],\n"
"    constant FullParams &p [[buffer(5)]],\n"
"    uint tg [[threadgroup_position_in_grid]],\n"
"    uint lane [[thread_index_in_simdgroup]],\n"
"    uint simd [[simdgroup_index_in_threadgroup]]) {\n"
"  const uint o = tg * 8u + simd;\n"
"  if (o >= p.D) return;\n"
"  const uint groups = p.I / p.group;\n"
"  const uint row_bytes = p.I / 2u;\n"
"  float combined_lane = 0.0f;\n"
"  for (uint e = 0; e < p.experts; ++e) {\n"
"    const uint base = e * p.expert_stride;\n"
"    for (uint g = 0; g < groups; ++g) {\n"
"      const uint i = g * p.group + lane;\n"
"      const uchar packed =\n"
"          experts[base + p.down_q + o * row_bytes + (i >> 1)];\n"
"      const int w = int((i & 1u) ? (packed >> 4) : (packed & 15u)) - 8;\n"
"      const device float *scales =\n"
"          (const device float *)(experts + base + p.down_s);\n"
"      combined_lane += float(w * int(hidden_q[e * p.I + i])) *\n"
"          scales[o * groups + g] * hidden_s[e] * route[e];\n"
"    }\n"
"  }\n"
"  const float combined = simd_sum(combined_lane);\n"
"  if (lane == 0u) output[o] = combined;\n"
"}\n"
"kernel void read_byte(const device uchar *input [[buffer(0)]],\n"
"                      device uint *output [[buffer(1)]],\n"
"                      constant uint &offset [[buffer(2)]],\n"
"                      uint gid [[thread_position_in_grid]]) {\n"
"  if (gid == 0u) output[0] = uint(input[offset]);\n"
"}\n"
"kernel void ping(device atomic_uint *counter [[buffer(0)]],\n"
"                 uint gid [[thread_position_in_grid]]) {\n"
"  if (gid == 0u) atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);\n"
"}\n";

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline float spike_silu(float value) {
    return value / (1.0f + expf(-value));
}

static void die(const char *message) {
    fprintf(stderr, "metal-spike: %s\n", message);
    exit(1);
}

static void die_nserror(const char *what, NSError *error) {
    fprintf(stderr, "metal-spike: %s: %s\n", what,
            error ? error.localizedDescription.UTF8String : "unknown error");
    exit(1);
}

static uint64_t process_footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count)
        != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
}

static uint64_t system_wired_bytes(void) {
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) != KERN_SUCCESS)
        return 0;
    return (uint64_t)vm.wire_count * (uint64_t)getpagesize();
}

static void pread_exact(int fd, void *buffer, size_t bytes, off_t offset) {
    uint8_t *out = buffer;
    size_t done = 0;
    while (done < bytes) {
        ssize_t got = pread(fd, out + done, bytes - done, offset + (off_t)done);
        if (got < 0) {
            perror("pread");
            exit(1);
        }
        if (got == 0) die("unexpected EOF");
        done += (size_t)got;
    }
}

static Q4Matrix make_matrix(int I, int O, int group, uint32_t seed) {
    Q4Matrix m = {0};
    m.I = I;
    m.O = O;
    m.group = group;
    m.q_bytes = (size_t)O * (size_t)((I + 1) / 2);
    m.scale_bytes = (size_t)O * (size_t)((I + group - 1) / group) * sizeof(float);
    m.blob_bytes = m.q_bytes + m.scale_bytes;
    m.blob = malloc(m.blob_bytes);
    if (!m.blob) die("OOM synthetic q4 matrix");
    m.q4 = m.blob;
    m.scales = (float *)(m.blob + m.q_bytes);
    uint32_t state = seed;
    for (size_t i = 0; i < m.q_bytes; ++i) {
        state = state * 1664525u + 1013904223u;
        m.q4[i] = (uint8_t)(state >> 24);
    }
    size_t scale_count = m.scale_bytes / sizeof(float);
    for (size_t i = 0; i < scale_count; ++i) {
        state = state * 1664525u + 1013904223u;
        m.scales[i] = 0.0005f + (float)(state & 0xffffu) / 65535.0f * 0.02f;
    }
    return m;
}

static void free_matrix(Q4Matrix *m) {
    free(m->blob);
    memset(m, 0, sizeof(*m));
}

static void make_inputs(float *x, int8_t *xq, float *sx, int S, int I,
                        uint32_t seed) {
    uint32_t state = seed;
    for (int s = 0; s < S; ++s) {
        float *row = x + (int64_t)s * I;
        for (int i = 0; i < I; ++i) {
            state = state * 1664525u + 1013904223u;
            row[i] = ((float)(int32_t)(state >> 8) / 8388608.0f) * 1.75f;
        }
        sx[s] = qrow_i8(row, xq + (int64_t)s * I, I);
    }
}

static MetalContext make_metal(void) {
    MetalContext context = {0};
    context.device = MTLCreateSystemDefaultDevice();
    if (!context.device) die("no Metal device");
    context.queue = [context.device newCommandQueue];
    if (!context.queue) die("cannot create Metal command queue");
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.mathMode = MTLMathModeSafe;
    NSError *error = nil;
    id<MTLLibrary> library =
        [context.device newLibraryWithSource:[NSString stringWithUTF8String:kMetalSource]
                                    options:options error:&error];
    if (!library) die_nserror("compile Metal library", error);
    id<MTLFunction> q4 = [library newFunctionWithName:@"grouped_q4_i8"];
    id<MTLFunction> gate_up = [library newFunctionWithName:@"fused_gate_up"];
    id<MTLFunction> quantize =
        [library newFunctionWithName:@"quantize_hidden"];
    id<MTLFunction> down =
        [library newFunctionWithName:@"fused_down_reduce"];
    id<MTLFunction> read = [library newFunctionWithName:@"read_byte"];
    id<MTLFunction> ping = [library newFunctionWithName:@"ping"];
    context.q4_pipeline =
        [context.device newComputePipelineStateWithFunction:q4 error:&error];
    if (!context.q4_pipeline) die_nserror("create grouped-q4 pipeline", error);
    context.gate_up_pipeline =
        [context.device newComputePipelineStateWithFunction:gate_up error:&error];
    if (!context.gate_up_pipeline)
        die_nserror("create fused gate/up pipeline", error);
    context.quantize_pipeline =
        [context.device newComputePipelineStateWithFunction:quantize error:&error];
    if (!context.quantize_pipeline)
        die_nserror("create hidden quantize pipeline", error);
    context.down_pipeline =
        [context.device newComputePipelineStateWithFunction:down error:&error];
    if (!context.down_pipeline)
        die_nserror("create fused down pipeline", error);
    context.read_pipeline =
        [context.device newComputePipelineStateWithFunction:read error:&error];
    if (!context.read_pipeline) die_nserror("create read pipeline", error);
    context.ping_pipeline =
        [context.device newComputePipelineStateWithFunction:ping error:&error];
    if (!context.ping_pipeline) die_nserror("create ping pipeline", error);
    return context;
}

static void encode_q4(id<MTLComputeCommandEncoder> encoder,
                      MetalContext *metal, id<MTLBuffer> blob,
                      id<MTLBuffer> xq, id<MTLBuffer> sx, id<MTLBuffer> y,
                      GpuParams params) {
    [encoder setComputePipelineState:metal->q4_pipeline];
    [encoder setBuffer:blob offset:0 atIndex:0];
    [encoder setBuffer:xq offset:0 atIndex:1];
    [encoder setBuffer:sx offset:0 atIndex:2];
    [encoder setBuffer:y offset:0 atIndex:3];
    [encoder setBytes:&params length:sizeof(params) atIndex:4];
    MTLSize threads = MTLSizeMake(GPU_THREADS, 1, 1);
    MTLSize groups =
        MTLSizeMake((params.O + GPU_SIMDGROUPS - 1) / GPU_SIMDGROUPS,
                    params.S, 1);
    [encoder dispatchThreadgroups:groups threadsPerThreadgroup:threads];
}

static double run_gpu(MetalContext *metal, id<MTLBuffer> blob,
                      id<MTLBuffer> xq, id<MTLBuffer> sx, id<MTLBuffer> y,
                      GpuParams params, int iterations) {
    double start = monotonic_seconds();
    for (int i = 0; i < iterations; ++i) {
        @autoreleasepool {
            id<MTLCommandBuffer> command = [metal->queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder =
                [command computeCommandEncoder];
            encode_q4(encoder, metal, blob, xq, sx, y, params);
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status == MTLCommandBufferStatusError)
                die_nserror("GPU grouped-q4 dispatch", command.error);
        }
    }
    return monotonic_seconds() - start;
}

static void compare_outputs(const float *cpu, const float *gpu, size_t count,
                            double *max_abs, double *max_rel) {
    *max_abs = 0.0;
    *max_rel = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double delta = fabs((double)cpu[i] - (double)gpu[i]);
        double denom = fmax(fabs((double)cpu[i]), 1e-7);
        if (delta > *max_abs) *max_abs = delta;
        if (delta / denom > *max_rel) *max_rel = delta / denom;
    }
}

static GpuParams params_for(int S, int I, int O, int group,
                            uint32_t q_offset, uint32_t scale_offset) {
    GpuParams p = {
        .S = (uint32_t)S,
        .I = (uint32_t)I,
        .O = (uint32_t)O,
        .group = (uint32_t)group,
        .row_bytes = (uint32_t)((I + 1) / 2),
        .groups = (uint32_t)((I + group - 1) / group),
        .q_offset = q_offset,
        .scale_offset = scale_offset,
    };
    return p;
}

static void synthetic_correctness(MetalContext *metal) {
    const int S = 3, I = REAL_D, O = REAL_I;
    Q4Matrix matrix = make_matrix(I, O, QGROUP, 0x13579bdu);
    float *x = malloc((size_t)S * I * sizeof(float));
    int8_t *xq = malloc((size_t)S * I);
    float *sx = malloc((size_t)S * sizeof(float));
    float *cpu = calloc((size_t)S * O, sizeof(float));
    if (!x || !xq || !sx || !cpu) die("OOM correctness buffers");
    make_inputs(x, xq, sx, S, I, 0x2468aceu);
    matmul_i4_grouped_idot(cpu, xq, sx, matrix.q4, matrix.scales,
                           QGROUP, S, I, O);

    id<MTLBuffer> blob =
        [metal->device newBufferWithBytes:matrix.blob
                                  length:matrix.blob_bytes
                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> xb =
        [metal->device newBufferWithBytes:xq length:(size_t)S * I
                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> sb =
        [metal->device newBufferWithBytes:sx length:(size_t)S * sizeof(float)
                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> yb =
        [metal->device newBufferWithLength:(size_t)S * O * sizeof(float)
                                   options:MTLResourceStorageModeShared];
    GpuParams p = params_for(S, I, O, QGROUP, 0, (uint32_t)matrix.q_bytes);
    run_gpu(metal, blob, xb, sb, yb, p, 1);
    double max_abs, max_rel;
    compare_outputs(cpu, yb.contents, (size_t)S * O, &max_abs, &max_rel);
    printf("[correctness] source=synthetic S=%d I=%d O=%d max_abs=%.9g "
           "max_rel=%.9g verdict=%s\n", S, I, O, max_abs, max_rel,
           max_abs <= 2e-4 ? "PASS" : "FAIL");
    if (max_abs > 2e-4) die("synthetic GPU output exceeds tolerance");
    free(x);
    free(xq);
    free(sx);
    free(cpu);
    free_matrix(&matrix);
}

static NSDictionary *read_json(NSString *path) {
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) die("cannot read JSON file");
    NSError *error = nil;
    id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if (!object || ![object isKindOfClass:[NSDictionary class]])
        die_nserror("parse JSON", error);
    return object;
}

@interface ExpertRecordBox : NSObject
@property(nonatomic, copy) NSString *key;
@property(nonatomic) off_t offset;
@property(nonatomic) size_t size;
@end
@implementation ExpertRecordBox
@end

static NSArray<ExpertRecordBox *> *load_record_boxes(NSString *model_dir) {
    NSDictionary *manifest =
        read_json([model_dir stringByAppendingPathComponent:@"manifest.json"]);
    NSDictionary *experts = manifest[@"experts"];
    if (![experts isKindOfClass:[NSDictionary class]])
        die("manifest experts is not an object");
    NSArray<NSString *> *keys =
        [[experts allKeys] sortedArrayUsingSelector:@selector(compare:)];
    NSMutableArray<ExpertRecordBox *> *records =
        [NSMutableArray arrayWithCapacity:keys.count];
    for (NSString *key in keys) {
        NSDictionary *entry = experts[key];
        ExpertRecordBox *record = [ExpertRecordBox new];
        record.key = key;
        record.offset = (off_t)[entry[@"offset"] longLongValue];
        record.size = (size_t)[entry[@"size"] unsignedLongLongValue];
        [records addObject:record];
    }
    return records;
}

static void real_correctness(MetalContext *metal, NSString *model_dir,
                             ExpertRecordBox *record) {
    NSString *path = [model_dir stringByAppendingPathComponent:@"experts.bin"];
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) {
        perror("open experts.bin");
        exit(1);
    }
    void *slab = NULL;
    int page = getpagesize();
    if (posix_memalign(&slab, (size_t)page, record.size) != 0)
        die("OOM real expert slab");
    pread_exact(fd, slab, record.size, record.offset);
    close(fd);

    const int S = 2, I = REAL_D, O = REAL_I;
    const size_t q_bytes = (size_t)O * I / 2;
    const size_t scale_bytes = (size_t)O * (I / QGROUP) * sizeof(float);
    if (record.size < q_bytes + scale_bytes)
        die("real expert slab is smaller than gate projection");
    float *x = malloc((size_t)S * I * sizeof(float));
    int8_t *xq = malloc((size_t)S * I);
    float *sx = malloc((size_t)S * sizeof(float));
    float *cpu = calloc((size_t)S * O, sizeof(float));
    if (!x || !xq || !sx || !cpu) die("OOM real correctness buffers");
    make_inputs(x, xq, sx, S, I, 0xabcdef01u);
    matmul_i4_grouped_idot(cpu, xq, sx, slab,
                           (float *)((uint8_t *)slab + q_bytes),
                           QGROUP, S, I, O);

    __block void *owned = slab;
    id<MTLBuffer> blob =
        [metal->device newBufferWithBytesNoCopy:slab length:record.size
                                        options:MTLResourceStorageModeShared
                                    deallocator:^(void *pointer, NSUInteger length) {
        (void)length;
        free(pointer);
        owned = NULL;
    }];
    if (!blob) die("cannot wrap real expert slab without copying");
    id<MTLBuffer> xb =
        [metal->device newBufferWithBytes:xq length:(size_t)S * I
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> sb =
        [metal->device newBufferWithBytes:sx length:(size_t)S * sizeof(float)
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> yb =
        [metal->device newBufferWithLength:(size_t)S * O * sizeof(float)
                                   options:MTLResourceStorageModeShared];
    GpuParams p = params_for(S, I, O, QGROUP, 0, (uint32_t)q_bytes);
    run_gpu(metal, blob, xb, sb, yb, p, 1);
    double max_abs, max_rel;
    compare_outputs(cpu, yb.contents, (size_t)S * O, &max_abs, &max_rel);
    printf("[correctness] source=real key=%s S=%d I=%d O=%d max_abs=%.9g "
           "max_rel=%.9g verdict=%s\n", record.key.UTF8String, S, I, O,
           max_abs, max_rel, max_abs <= 2e-4 ? "PASS" : "FAIL");
    if (max_abs > 2e-4) die("real GPU output exceeds tolerance");
    blob = nil;
    if (owned) {
        free(owned);
        owned = NULL;
    }
    free(x);
    free(xq);
    free(sx);
    free(cpu);
}

static double bench_cpu(const Q4Matrix *matrix, const int8_t *xq,
                        const float *sx, float *y, int S, int threads,
                        int iterations) {
#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(threads);
#else
    (void)threads;
#endif
    matmul_i4_grouped_idot(y, xq, sx, matrix->q4, matrix->scales,
                           matrix->group, S, matrix->I, matrix->O);
    double start = monotonic_seconds();
    for (int i = 0; i < iterations; ++i)
        matmul_i4_grouped_idot(y, xq, sx, matrix->q4, matrix->scales,
                               matrix->group, S, matrix->I, matrix->O);
    return monotonic_seconds() - start;
}

static void throughput_shape(MetalContext *metal, const char *name,
                             int I, int O, uint32_t seed) {
    Q4Matrix matrix = make_matrix(I, O, QGROUP, seed);
    id<MTLBuffer> blob =
        [metal->device newBufferWithBytes:matrix.blob length:matrix.blob_bytes
                                  options:MTLResourceStorageModeShared];
    const int batches[] = {1, 8, 32, 128};
    for (size_t b = 0; b < sizeof(batches) / sizeof(batches[0]); ++b) {
        int S = batches[b];
        int iterations = 256 / S;
        if (iterations < 2) iterations = 2;
        float *x = malloc((size_t)S * I * sizeof(float));
        int8_t *xq = malloc((size_t)S * I);
        float *sx = malloc((size_t)S * sizeof(float));
        float *y = malloc((size_t)S * O * sizeof(float));
        if (!x || !xq || !sx || !y) die("OOM throughput buffers");
        make_inputs(x, xq, sx, S, I, seed ^ (uint32_t)S);
        id<MTLBuffer> xb =
            [metal->device newBufferWithBytes:xq length:(size_t)S * I
                                      options:MTLResourceStorageModeShared];
        id<MTLBuffer> sb =
            [metal->device newBufferWithBytes:sx length:(size_t)S * sizeof(float)
                                      options:MTLResourceStorageModeShared];
        id<MTLBuffer> yb =
            [metal->device newBufferWithLength:(size_t)S * O * sizeof(float)
                                       options:MTLResourceStorageModeShared];
        GpuParams p =
            params_for(S, I, O, QGROUP, 0, (uint32_t)matrix.q_bytes);
        run_gpu(metal, blob, xb, sb, yb, p, 2);
        double gpu_s = run_gpu(metal, blob, xb, sb, yb, p, iterations);
        double cpu1_s = bench_cpu(&matrix, xq, sx, y, S, 1, iterations);
        double cpu4_s = bench_cpu(&matrix, xq, sx, y, S, 4, iterations);
        double flops = 2.0 * (double)iterations * S * I * O;
        printf("[throughput] shape=%s S=%d I=%d O=%d iterations=%d "
               "gpu_wall_ms=%.4f gpu_gflops=%.2f "
               "cpu1_ms=%.4f cpu1_gflops=%.2f "
               "cpu4_ms=%.4f cpu4_gflops=%.2f\n",
               name, S, I, O, iterations,
               gpu_s * 1000.0 / iterations, flops / gpu_s / 1e9,
               cpu1_s * 1000.0 / iterations, flops / cpu1_s / 1e9,
               cpu4_s * 1000.0 / iterations, flops / cpu4_s / 1e9);
        free(x);
        free(xq);
        free(sx);
        free(y);
    }
    free_matrix(&matrix);
}

static FullParams full_params(void) {
    const uint32_t gate_q_bytes = (uint32_t)((uint64_t)REAL_I * REAL_D / 2u);
    const uint32_t gate_s_bytes =
        (uint32_t)((uint64_t)REAL_I * (REAL_D / QGROUP) * sizeof(float));
    const uint32_t down_q_bytes = (uint32_t)((uint64_t)REAL_D * REAL_I / 2u);
    const uint32_t down_s_bytes =
        (uint32_t)((uint64_t)REAL_D * (REAL_I / QGROUP) * sizeof(float));
    FullParams p = {
        .D = REAL_D,
        .I = REAL_I,
        .group = QGROUP,
        .experts = ROUTED_K,
        .expert_stride =
            2u * (gate_q_bytes + gate_s_bytes) + down_q_bytes + down_s_bytes,
        .gate_q = 0,
        .gate_s = gate_q_bytes,
        .up_q = gate_q_bytes + gate_s_bytes,
        .up_s = 2u * gate_q_bytes + gate_s_bytes,
        .down_q = 2u * (gate_q_bytes + gate_s_bytes),
        .down_s = 2u * (gate_q_bytes + gate_s_bytes) + down_q_bytes,
    };
    return p;
}

static uint8_t *make_full_experts(FullParams p) {
    size_t bytes = (size_t)p.experts * p.expert_stride;
    uint8_t *blob = malloc(bytes);
    if (!blob) die("OOM top-8 expert blob");
    for (uint32_t e = 0; e < p.experts; ++e) {
        uint8_t *base = blob + (size_t)e * p.expert_stride;
        Q4Matrix gate =
            make_matrix(REAL_D, REAL_I, QGROUP, 0x11110000u + e);
        Q4Matrix up =
            make_matrix(REAL_D, REAL_I, QGROUP, 0x22220000u + e);
        Q4Matrix down =
            make_matrix(REAL_I, REAL_D, QGROUP, 0x33330000u + e);
        memcpy(base + p.gate_q, gate.q4, gate.q_bytes);
        memcpy(base + p.gate_s, gate.scales, gate.scale_bytes);
        memcpy(base + p.up_q, up.q4, up.q_bytes);
        memcpy(base + p.up_s, up.scales, up.scale_bytes);
        memcpy(base + p.down_q, down.q4, down.q_bytes);
        memcpy(base + p.down_s, down.scales, down.scale_bytes);
        free_matrix(&gate);
        free_matrix(&up);
        free_matrix(&down);
    }
    return blob;
}

static void cpu_full_layer_once(const uint8_t *experts, FullParams p,
                                const int8_t *xq, float sx,
                                const float *route, float *output,
                                float *gate, float *up, int8_t *hidden_q,
                                float *hidden_s, float *down) {
    memset(output, 0, (size_t)p.D * sizeof(float));
    for (uint32_t e = 0; e < p.experts; ++e) {
        const uint8_t *base = experts + (size_t)e * p.expert_stride;
        matmul_i4_grouped_idot(gate, xq, &sx, base + p.gate_q,
                               (const float *)(base + p.gate_s),
                               p.group, 1, p.D, p.I);
        matmul_i4_grouped_idot(up, xq, &sx, base + p.up_q,
                               (const float *)(base + p.up_s),
                               p.group, 1, p.D, p.I);
        for (uint32_t i = 0; i < p.I; ++i)
            gate[i] = spike_silu(gate[i]) * up[i];
        hidden_s[e] = qrow_i8(gate, hidden_q + (size_t)e * p.I, p.I);
        matmul_i4_grouped_idot(down, hidden_q + (size_t)e * p.I,
                               hidden_s + e, base + p.down_q,
                               (const float *)(base + p.down_s),
                               p.group, 1, p.I, p.D);
        for (uint32_t d = 0; d < p.D; ++d)
            output[d] += route[e] * down[d];
    }
}

typedef struct {
    double wall_seconds;
    double gpu_seconds;
} GpuTiming;

static int compare_double(const void *left, const void *right);

static void encode_full_layer(id<MTLCommandBuffer> command,
                              MetalContext *metal, id<MTLBuffer> experts,
                              id<MTLBuffer> xq, id<MTLBuffer> sx,
                              id<MTLBuffer> route, id<MTLBuffer> hidden,
                              id<MTLBuffer> hidden_q,
                              id<MTLBuffer> hidden_s,
                              id<MTLBuffer> output, FullParams p) {
    id<MTLComputeCommandEncoder> gate_encoder =
        [command computeCommandEncoder];
    [gate_encoder setComputePipelineState:metal->gate_up_pipeline];
    [gate_encoder setBuffer:experts offset:0 atIndex:0];
    [gate_encoder setBuffer:xq offset:0 atIndex:1];
    [gate_encoder setBuffer:sx offset:0 atIndex:2];
    [gate_encoder setBuffer:hidden offset:0 atIndex:3];
    [gate_encoder setBytes:&p length:sizeof(p) atIndex:4];
    [gate_encoder
        dispatchThreadgroups:MTLSizeMake(
            (p.I + GPU_SIMDGROUPS - 1) / GPU_SIMDGROUPS,
            p.experts, 1)
        threadsPerThreadgroup:MTLSizeMake(GPU_THREADS, 1, 1)];
    [gate_encoder endEncoding];

    id<MTLComputeCommandEncoder> quant_encoder =
        [command computeCommandEncoder];
    [quant_encoder setComputePipelineState:metal->quantize_pipeline];
    [quant_encoder setBuffer:hidden offset:0 atIndex:0];
    [quant_encoder setBuffer:hidden_q offset:0 atIndex:1];
    [quant_encoder setBuffer:hidden_s offset:0 atIndex:2];
    [quant_encoder setBytes:&p length:sizeof(p) atIndex:3];
    [quant_encoder setThreadgroupMemoryLength:
        GPU_THREADS * sizeof(float) atIndex:0];
    [quant_encoder
        dispatchThreadgroups:MTLSizeMake(p.experts, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(GPU_THREADS, 1, 1)];
    [quant_encoder endEncoding];

    id<MTLComputeCommandEncoder> down_encoder =
        [command computeCommandEncoder];
    [down_encoder setComputePipelineState:metal->down_pipeline];
    [down_encoder setBuffer:experts offset:0 atIndex:0];
    [down_encoder setBuffer:hidden_q offset:0 atIndex:1];
    [down_encoder setBuffer:hidden_s offset:0 atIndex:2];
    [down_encoder setBuffer:route offset:0 atIndex:3];
    [down_encoder setBuffer:output offset:0 atIndex:4];
    [down_encoder setBytes:&p length:sizeof(p) atIndex:5];
    [down_encoder
        dispatchThreadgroups:MTLSizeMake(
            (p.D + GPU_SIMDGROUPS - 1) / GPU_SIMDGROUPS, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(GPU_THREADS, 1, 1)];
    [down_encoder endEncoding];
}

static GpuTiming run_full_gpu(MetalContext *metal, id<MTLBuffer> experts,
                              id<MTLBuffer> xq, id<MTLBuffer> sx,
                              id<MTLBuffer> route, id<MTLBuffer> hidden,
                              id<MTLBuffer> hidden_q,
                              id<MTLBuffer> hidden_s, id<MTLBuffer> output,
                              FullParams p, int iterations) {
    GpuTiming timing = {0};
    double wall_start = monotonic_seconds();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        @autoreleasepool {
            id<MTLCommandBuffer> command = [metal->queue commandBuffer];
            encode_full_layer(command, metal, experts, xq, sx, route,
                              hidden, hidden_q, hidden_s, output, p);
            [command commit];
            [command waitUntilCompleted];
            if (command.status == MTLCommandBufferStatusError)
                die_nserror("full top-8 dispatch", command.error);
            if (command.GPUEndTime > command.GPUStartTime)
                timing.gpu_seconds +=
                    command.GPUEndTime - command.GPUStartTime;
        }
    }
    timing.wall_seconds = monotonic_seconds() - wall_start;
    return timing;
}

static double run_full_cpu(const uint8_t *experts, FullParams p,
                           const int8_t *xq, float sx, const float *route,
                           float *output, float *gate, float *up,
                           int8_t *hidden_q, float *hidden_s, float *down,
                           int threads, int iterations) {
#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(threads);
#else
    (void)threads;
#endif
    cpu_full_layer_once(experts, p, xq, sx, route, output, gate, up,
                        hidden_q, hidden_s, down);
    double start = monotonic_seconds();
    for (int i = 0; i < iterations; ++i)
        cpu_full_layer_once(experts, p, xq, sx, route, output, gate, up,
                            hidden_q, hidden_s, down);
    return monotonic_seconds() - start;
}

static double full_event_batch(
    MetalContext *metal, id<MTLSharedEvent> event,
    MTLSharedEventListener *listener, NSArray *semaphores,
    uint64_t *next_value, id<MTLBuffer> experts, id<MTLBuffer> xq,
    id<MTLBuffer> sx, id<MTLBuffer> route, id<MTLBuffer> hidden,
    id<MTLBuffer> hidden_q, id<MTLBuffer> hidden_s,
    id<MTLBuffer> output, FullParams p) {
    uint64_t base = *next_value;
    id<MTLCommandBuffer> command = [metal->queue commandBuffer];
    for (int layer = 0; layer < MODEL_LAYERS; ++layer) {
        uint64_t cpu_value = base + (uint64_t)(2 * layer + 1);
        [command encodeWaitForEvent:event value:cpu_value];
        encode_full_layer(command, metal, experts, xq, sx, route, hidden,
                          hidden_q, hidden_s, output, p);
        [command encodeSignalEvent:event value:cpu_value + 1];
    }
    for (int layer = 0; layer < MODEL_LAYERS; ++layer) {
        uint64_t gpu_value = base + (uint64_t)(2 * layer + 2);
        dispatch_semaphore_t semaphore = semaphores[layer];
        [event notifyListener:listener atValue:gpu_value
                        block:^(id<MTLSharedEvent> ignored, uint64_t value) {
            (void)ignored;
            (void)value;
            dispatch_semaphore_signal(semaphore);
        }];
    }
    double start = monotonic_seconds();
    [command commit];
    for (int layer = 0; layer < MODEL_LAYERS; ++layer) {
        event.signaledValue = base + (uint64_t)(2 * layer + 1);
        dispatch_semaphore_wait(semaphores[layer], DISPATCH_TIME_FOREVER);
    }
    [command waitUntilCompleted];
    if (command.status == MTLCommandBufferStatusError)
        die_nserror("full event pipeline", command.error);
    *next_value = base + (uint64_t)(2 * MODEL_LAYERS);
    return (monotonic_seconds() - start) * 1000.0;
}

static double full_busy_event_batch(
    MetalContext *metal, id<MTLSharedEvent> event, uint64_t *next_value,
    id<MTLBuffer> experts, id<MTLBuffer> xq, id<MTLBuffer> sx,
    id<MTLBuffer> route, id<MTLBuffer> hidden, id<MTLBuffer> hidden_q,
    id<MTLBuffer> hidden_s, id<MTLBuffer> output, FullParams p) {
    uint64_t base = *next_value;
    id<MTLCommandBuffer> command = [metal->queue commandBuffer];
    for (int layer = 0; layer < MODEL_LAYERS; ++layer) {
        uint64_t cpu_value = base + (uint64_t)(2 * layer + 1);
        [command encodeWaitForEvent:event value:cpu_value];
        encode_full_layer(command, metal, experts, xq, sx, route, hidden,
                          hidden_q, hidden_s, output, p);
        [command encodeSignalEvent:event value:cpu_value + 1];
    }
    double start = monotonic_seconds();
    [command commit];
    for (int layer = 0; layer < MODEL_LAYERS; ++layer) {
        uint64_t cpu_value = base + (uint64_t)(2 * layer + 1);
        event.signaledValue = cpu_value;
        while (event.signaledValue < cpu_value + 1)
            __asm__ volatile("" ::: "memory");
    }
    [command waitUntilCompleted];
    if (command.status == MTLCommandBufferStatusError)
        die_nserror("full busy-poll event pipeline", command.error);
    *next_value = base + (uint64_t)(2 * MODEL_LAYERS);
    return (monotonic_seconds() - start) * 1000.0;
}

static double full_no_event_batch(
    MetalContext *metal, id<MTLBuffer> experts, id<MTLBuffer> xq,
    id<MTLBuffer> sx, id<MTLBuffer> route, id<MTLBuffer> hidden,
    id<MTLBuffer> hidden_q, id<MTLBuffer> hidden_s,
    id<MTLBuffer> output, FullParams p) {
    id<MTLCommandBuffer> command = [metal->queue commandBuffer];
    for (int layer = 0; layer < MODEL_LAYERS; ++layer)
        encode_full_layer(command, metal, experts, xq, sx, route, hidden,
                          hidden_q, hidden_s, output, p);
    double start = monotonic_seconds();
    [command commit];
    [command waitUntilCompleted];
    if (command.status == MTLCommandBufferStatusError)
        die_nserror("full no-event batch", command.error);
    return (monotonic_seconds() - start) * 1000.0;
}

static void full_event_pipeline_probe(
    MetalContext *metal, id<MTLBuffer> experts, id<MTLBuffer> xq,
    id<MTLBuffer> sx, id<MTLBuffer> route, id<MTLBuffer> hidden,
    id<MTLBuffer> hidden_q, id<MTLBuffer> hidden_s,
    id<MTLBuffer> output, FullParams p, double cpu4_ms_token) {
    id<MTLSharedEvent> event = [metal->device newSharedEvent];
    dispatch_queue_attr_t callback_attributes =
        dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    dispatch_queue_t callback_queue =
        dispatch_queue_create("samosa.metal-spike.full-events",
                              callback_attributes);
    MTLSharedEventListener *listener =
        [[MTLSharedEventListener alloc] initWithDispatchQueue:callback_queue];
    NSMutableArray *semaphores =
        [NSMutableArray arrayWithCapacity:MODEL_LAYERS];
    for (int i = 0; i < MODEL_LAYERS; ++i)
        [semaphores addObject:dispatch_semaphore_create(0)];
    for (int i = 0; i < 3; ++i)
        (void)full_no_event_batch(
            metal, experts, xq, sx, route, hidden, hidden_q, hidden_s,
            output, p);
    double no_event_samples[20];
    for (int i = 0; i < 20; ++i)
        no_event_samples[i] = full_no_event_batch(
            metal, experts, xq, sx, route, hidden, hidden_q, hidden_s,
            output, p);
    qsort(no_event_samples, 20, sizeof(no_event_samples[0]), compare_double);
    printf("[full-batch-no-events] layers=%d median_ms_token=%.3f "
           "p10_ms=%.3f p90_ms=%.3f cpu4_ms_token=%.3f "
           "speedup_vs_cpu4=%.3f dependency_model=unrealizable_lower_bound\n",
           MODEL_LAYERS, no_event_samples[10], no_event_samples[2],
           no_event_samples[18], cpu4_ms_token,
           cpu4_ms_token / no_event_samples[10]);
    uint64_t next = 0;
    for (int i = 0; i < 3; ++i)
        (void)full_event_batch(
            metal, event, listener, semaphores, &next, experts, xq, sx,
            route, hidden, hidden_q, hidden_s, output, p);
    double samples[20];
    for (int i = 0; i < 20; ++i)
        samples[i] = full_event_batch(
            metal, event, listener, semaphores, &next, experts, xq, sx,
            route, hidden, hidden_q, hidden_s, output, p);
    qsort(samples, 20, sizeof(samples[0]), compare_double);
    printf("[full-pipeline] layers=%d one_command_buffer=true "
           "median_ms_token=%.3f p10_ms=%.3f p90_ms=%.3f "
           "cpu4_ms_token=%.3f speedup_vs_cpu4=%.3f\n",
           MODEL_LAYERS, samples[10], samples[2], samples[18],
           cpu4_ms_token, cpu4_ms_token / samples[10]);

    id<MTLSharedEvent> busy_event = [metal->device newSharedEvent];
    uint64_t busy_next = 0;
    for (int i = 0; i < 3; ++i)
        (void)full_busy_event_batch(
            metal, busy_event, &busy_next, experts, xq, sx, route, hidden,
            hidden_q, hidden_s, output, p);
    double busy_samples[20];
    for (int i = 0; i < 20; ++i)
        busy_samples[i] = full_busy_event_batch(
            metal, busy_event, &busy_next, experts, xq, sx, route, hidden,
            hidden_q, hidden_s, output, p);
    qsort(busy_samples, 20, sizeof(busy_samples[0]), compare_double);
    printf("[full-pipeline-busy] layers=%d median_ms_token=%.3f "
           "p10_ms=%.3f p90_ms=%.3f cpu4_ms_token=%.3f "
           "speedup_vs_cpu4=%.3f caveat=burns_one_cpu_core\n",
           MODEL_LAYERS, busy_samples[10], busy_samples[2],
           busy_samples[18], cpu4_ms_token,
           cpu4_ms_token / busy_samples[10]);
}

static void full_layer_probe(MetalContext *metal) {
    FullParams p = full_params();
    uint8_t *experts = make_full_experts(p);
    float *x = malloc((size_t)p.D * sizeof(float));
    int8_t *xq = malloc((size_t)p.D);
    float sx = 0.0f;
    float route[ROUTED_K];
    float *cpu_output = malloc((size_t)p.D * sizeof(float));
    float *gate = malloc((size_t)p.I * sizeof(float));
    float *up = malloc((size_t)p.I * sizeof(float));
    int8_t *cpu_hidden_q = malloc((size_t)p.experts * p.I);
    float *cpu_hidden_s = malloc((size_t)p.experts * sizeof(float));
    float *down = malloc((size_t)p.D * sizeof(float));
    if (!experts || !x || !xq || !cpu_output || !gate || !up ||
        !cpu_hidden_q || !cpu_hidden_s || !down)
        die("OOM full-layer buffers");
    make_inputs(x, xq, &sx, 1, p.D, 0x98765432u);
    float route_sum = 0.0f;
    for (uint32_t e = 0; e < p.experts; ++e) {
        route[e] = (float)(e + 1);
        route_sum += route[e];
    }
    for (uint32_t e = 0; e < p.experts; ++e)
        route[e] /= route_sum;
    cpu_full_layer_once(experts, p, xq, sx, route, cpu_output, gate, up,
                        cpu_hidden_q, cpu_hidden_s, down);

    id<MTLBuffer> expert_buffer =
        [metal->device newBufferWithBytes:experts
                                  length:(size_t)p.experts * p.expert_stride
                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> xq_buffer =
        [metal->device newBufferWithBytes:xq length:p.D
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> sx_buffer =
        [metal->device newBufferWithBytes:&sx length:sizeof(sx)
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> route_buffer =
        [metal->device newBufferWithBytes:route length:sizeof(route)
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> hidden_buffer =
        [metal->device newBufferWithLength:(size_t)p.experts * p.I *
                                           sizeof(float)
                                   options:MTLResourceStorageModeShared];
    id<MTLBuffer> hidden_q_buffer =
        [metal->device newBufferWithLength:(size_t)p.experts * p.I
                                   options:MTLResourceStorageModeShared];
    id<MTLBuffer> hidden_s_buffer =
        [metal->device newBufferWithLength:(size_t)p.experts * sizeof(float)
                                   options:MTLResourceStorageModeShared];
    id<MTLBuffer> output_buffer =
        [metal->device newBufferWithLength:(size_t)p.D * sizeof(float)
                                   options:MTLResourceStorageModeShared];
    run_full_gpu(metal, expert_buffer, xq_buffer, sx_buffer, route_buffer,
                 hidden_buffer, hidden_q_buffer, hidden_s_buffer,
                 output_buffer, p, 1);
    double max_abs, max_rel;
    compare_outputs(cpu_output, output_buffer.contents, p.D,
                    &max_abs, &max_rel);
    printf("[full-layer-correctness] experts=%u max_abs=%.9g max_rel=%.9g "
           "verdict=%s\n", p.experts, max_abs, max_rel,
           max_abs <= 2e-3 ? "PASS" : "FAIL");
    if (max_abs > 2e-3)
        die("full-layer GPU output exceeds tolerance");

    const int iterations = 100;
    GpuTiming gpu = run_full_gpu(
        metal, expert_buffer, xq_buffer, sx_buffer, route_buffer,
        hidden_buffer, hidden_q_buffer, hidden_s_buffer, output_buffer,
        p, iterations);
    double cpu1 = run_full_cpu(
        experts, p, xq, sx, route, cpu_output, gate, up, cpu_hidden_q,
        cpu_hidden_s, down, 1, iterations);
    double cpu4 = run_full_cpu(
        experts, p, xq, sx, route, cpu_output, gate, up, cpu_hidden_q,
        cpu_hidden_s, down, 4, iterations);
    double flops_per_layer =
        2.0 * p.experts * 3.0 * (double)p.D * p.I;
    printf("[full-layer] experts=%u iterations=%d bytes_per_layer=%u "
           "gpu_wall_ms=%.4f gpu_active_ms=%.4f gpu_gflops=%.2f "
           "cpu1_ms=%.4f cpu1_gflops=%.2f "
           "cpu4_ms=%.4f cpu4_gflops=%.2f "
           "gpu_wall_x40_ms=%.3f cpu4_x40_ms=%.3f\n",
           p.experts, iterations, p.experts * p.expert_stride,
           gpu.wall_seconds * 1000.0 / iterations,
           gpu.gpu_seconds * 1000.0 / iterations,
           flops_per_layer * iterations / gpu.wall_seconds / 1e9,
           cpu1 * 1000.0 / iterations,
           flops_per_layer * iterations / cpu1 / 1e9,
           cpu4 * 1000.0 / iterations,
           flops_per_layer * iterations / cpu4 / 1e9,
           gpu.wall_seconds * 1000.0 / iterations * MODEL_LAYERS,
           cpu4 * 1000.0 / iterations * MODEL_LAYERS);
    full_event_pipeline_probe(
        metal, expert_buffer, xq_buffer, sx_buffer, route_buffer,
        hidden_buffer, hidden_q_buffer, hidden_s_buffer, output_buffer, p,
        cpu4 * 1000.0 / iterations * MODEL_LAYERS);
    free(experts);
    free(x);
    free(xq);
    free(cpu_output);
    free(gate);
    free(up);
    free(cpu_hidden_q);
    free(cpu_hidden_s);
    free(down);
}

static void sustain_probe(MetalContext *metal, const char *mode,
                          double requested_seconds) {
    FullParams p = full_params();
    uint8_t *experts = make_full_experts(p);
    float *x = malloc((size_t)p.D * sizeof(float));
    int8_t *xq = malloc((size_t)p.D);
    float sx = 0.0f;
    float route[ROUTED_K];
    float *output = malloc((size_t)p.D * sizeof(float));
    float *gate = malloc((size_t)p.I * sizeof(float));
    float *up = malloc((size_t)p.I * sizeof(float));
    int8_t *cpu_hidden_q = malloc((size_t)p.experts * p.I);
    float *cpu_hidden_s = malloc((size_t)p.experts * sizeof(float));
    float *down = malloc((size_t)p.D * sizeof(float));
    if (!experts || !x || !xq || !output || !gate || !up ||
        !cpu_hidden_q || !cpu_hidden_s || !down)
        die("OOM sustained-loop buffers");
    make_inputs(x, xq, &sx, 1, p.D, 0x76543210u);
    for (uint32_t e = 0; e < p.experts; ++e)
        route[e] = 1.0f / (float)p.experts;

    id<MTLBuffer> expert_buffer =
        [metal->device newBufferWithBytes:experts
                                  length:(size_t)p.experts * p.expert_stride
                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> xq_buffer =
        [metal->device newBufferWithBytes:xq length:p.D
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> sx_buffer =
        [metal->device newBufferWithBytes:&sx length:sizeof(sx)
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> route_buffer =
        [metal->device newBufferWithBytes:route length:sizeof(route)
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> hidden_buffer =
        [metal->device newBufferWithLength:(size_t)p.experts * p.I *
                                           sizeof(float)
                                   options:MTLResourceStorageModeShared];
    id<MTLBuffer> hidden_q_buffer =
        [metal->device newBufferWithLength:(size_t)p.experts * p.I
                                   options:MTLResourceStorageModeShared];
    id<MTLBuffer> hidden_s_buffer =
        [metal->device newBufferWithLength:(size_t)p.experts * sizeof(float)
                                   options:MTLResourceStorageModeShared];
    id<MTLBuffer> output_buffer =
        [metal->device newBufferWithLength:(size_t)p.D * sizeof(float)
                                   options:MTLResourceStorageModeShared];
    int threads = strcmp(mode, "cpu1") == 0 ? 1 : 4;
    int use_gpu = strcmp(mode, "gpu") == 0;
    if (!use_gpu && strcmp(mode, "cpu1") != 0 && strcmp(mode, "cpu4") != 0)
        die("--sustain mode must be gpu, cpu1, or cpu4");
    time_t epoch_start = time(NULL);
    double start = monotonic_seconds();
    uint64_t layers = 0;
    printf("[sustain-start] mode=%s epoch=%lld requested_seconds=%.1f\n",
           mode, (long long)epoch_start, requested_seconds);
    fflush(stdout);
    while (monotonic_seconds() - start < requested_seconds) {
        if (use_gpu) {
            (void)run_full_gpu(
                metal, expert_buffer, xq_buffer, sx_buffer, route_buffer,
                hidden_buffer, hidden_q_buffer, hidden_s_buffer,
                output_buffer, p, 10);
        } else {
            (void)run_full_cpu(
                experts, p, xq, sx, route, output, gate, up, cpu_hidden_q,
                cpu_hidden_s, down, threads, 10);
        }
        layers += 10;
    }
    double elapsed = monotonic_seconds() - start;
    time_t epoch_end = time(NULL);
    double flops = (double)layers * 2.0 * p.experts * 3.0 * p.D * p.I;
    printf("[sustain-end] mode=%s epoch=%lld elapsed_seconds=%.3f "
           "layers=%llu layers_per_second=%.3f gflops=%.3f\n",
           mode, (long long)epoch_end, elapsed,
           (unsigned long long)layers, layers / elapsed,
           flops / elapsed / 1e9);
    free(experts);
    free(x);
    free(xq);
    free(output);
    free(gate);
    free(up);
    free(cpu_hidden_q);
    free(cpu_hidden_s);
    free(down);
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double sync_batch(MetalContext *metal, id<MTLSharedEvent> event,
                         MTLSharedEventListener *listener,
                         NSArray *semaphores,
                         id<MTLBuffer> counter, uint64_t *next_value) {
    const int rounds = MODEL_LAYERS;
    uint64_t base = *next_value;
    id<MTLCommandBuffer> command = [metal->queue commandBuffer];
    for (int i = 0; i < rounds; ++i) {
        uint64_t cpu_value = base + (uint64_t)(2 * i + 1);
        [command encodeWaitForEvent:event value:cpu_value];
        id<MTLComputeCommandEncoder> encoder =
            [command computeCommandEncoder];
        [encoder setComputePipelineState:metal->ping_pipeline];
        [encoder setBuffer:counter offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        [command encodeSignalEvent:event value:cpu_value + 1];
    }
    for (int i = 0; i < rounds; ++i) {
        uint64_t gpu_value = base + (uint64_t)(2 * i + 2);
        dispatch_semaphore_t semaphore = semaphores[i];
        [event notifyListener:listener atValue:gpu_value
                        block:^(id<MTLSharedEvent> ignored, uint64_t value) {
            (void)ignored;
            (void)value;
            dispatch_semaphore_signal(semaphore);
        }];
    }
    double start = monotonic_seconds();
    [command commit];
    for (int i = 0; i < rounds; ++i) {
        event.signaledValue = base + (uint64_t)(2 * i + 1);
        dispatch_semaphore_wait(semaphores[i], DISPATCH_TIME_FOREVER);
    }
    [command waitUntilCompleted];
    if (command.status == MTLCommandBufferStatusError)
        die_nserror("shared-event command buffer", command.error);
    *next_value = base + (uint64_t)(2 * rounds);
    return (monotonic_seconds() - start) * 1e6 / rounds;
}

static void synchronization_probe(MetalContext *metal) {
    id<MTLSharedEvent> event = [metal->device newSharedEvent];
    dispatch_queue_attr_t callback_attributes =
        dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    dispatch_queue_t callback_queue =
        dispatch_queue_create("samosa.metal-spike.events",
                              callback_attributes);
    MTLSharedEventListener *listener =
        [[MTLSharedEventListener alloc] initWithDispatchQueue:callback_queue];
    id<MTLBuffer> counter =
        [metal->device newBufferWithLength:sizeof(uint32_t)
                                   options:MTLResourceStorageModeShared];
    NSMutableArray *semaphores =
        [NSMutableArray arrayWithCapacity:MODEL_LAYERS];
    for (int i = 0; i < MODEL_LAYERS; ++i)
        [semaphores addObject:dispatch_semaphore_create(0)];
    uint64_t next = 0;
    for (int i = 0; i < 5; ++i)
        (void)sync_batch(metal, event, listener, semaphores, counter, &next);
    double samples[50];
    for (int i = 0; i < 50; ++i)
        samples[i] = sync_batch(metal, event, listener, semaphores,
                                counter, &next);
    qsort(samples, 50, sizeof(samples[0]), compare_double);
    double median = samples[25];
    printf("[sync] layers=%d median_us_roundtrip=%.3f p10_us=%.3f p90_us=%.3f "
           "predicted_ms_token=%.3f\n", MODEL_LAYERS, median, samples[5],
           samples[45], median * MODEL_LAYERS / 1000.0);
}

static void nocopy_probe(MetalContext *metal) {
    const size_t bytes = 16u * 1024u * 1024u;
    void *arena = NULL;
    int page = getpagesize();
    if (posix_memalign(&arena, (size_t)page, bytes) != 0)
        die("OOM no-copy arena");
    memset(arena, 0, bytes);
    ((uint8_t *)arena)[123] = 42;
    __block void *owned = arena;
    id<MTLBuffer> buffer =
        [metal->device newBufferWithBytesNoCopy:arena length:bytes
                                        options:MTLResourceStorageModeShared
                                    deallocator:^(void *pointer, NSUInteger length) {
        (void)length;
        free(pointer);
        owned = NULL;
    }];
    if (!buffer) die("page-aligned no-copy arena creation failed");
    id<MTLBuffer> output =
        [metal->device newBufferWithLength:sizeof(uint32_t)
                                   options:MTLResourceStorageModeShared];
    uint32_t offset = 123;
    id<MTLCommandBuffer> command = [metal->queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:metal->read_pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&offset length:sizeof(offset) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    uint32_t got = *(uint32_t *)output.contents;
    printf("[nocopy] page_bytes=%d arena_bytes=%zu create=PASS gpu_read=%u "
           "verdict=%s\n", page, bytes, got, got == 42 ? "PASS" : "FAIL");
    if (got != 42) die("GPU did not observe CPU-written arena byte");
    buffer = nil;
    if (owned) {
        free(owned);
        owned = NULL;
    }
}

static double mapping_residency(void *mapping, size_t bytes) {
    size_t page = (size_t)getpagesize();
    size_t pages = (bytes + page - 1) / page;
    unsigned char *vector = malloc(pages);
    if (!vector) die("OOM mincore vector");
    if (mincore(mapping, bytes, (char *)vector) != 0) {
        perror("mincore");
        exit(1);
    }
    size_t resident = 0;
    for (size_t i = 0; i < pages; ++i)
        resident += (vector[i] & 1u) != 0;
    free(vector);
    return pages ? 100.0 * (double)resident / (double)pages : 0.0;
}

static void mmap_probe(MetalContext *metal, NSString *model_dir,
                       NSArray<ExpertRecordBox *> *records) {
    NSString *path = [model_dir stringByAppendingPathComponent:@"experts.bin"];
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) {
        perror("open experts.bin");
        exit(1);
    }
    ExpertRecordBox *best = nil;
    double best_residency = 101.0;
    size_t stride = records.count / 32;
    if (!stride) stride = 1;
    for (size_t i = 0; i < records.count; i += stride) {
        ExpertRecordBox *candidate = records[i];
        if ((candidate.offset % getpagesize()) ||
            (candidate.size % (size_t)getpagesize()))
            continue;
        void *mapping = mmap(NULL, candidate.size, PROT_READ, MAP_PRIVATE,
                             fd, candidate.offset);
        if (mapping == MAP_FAILED) continue;
        double residency = mapping_residency(mapping, candidate.size);
        munmap(mapping, candidate.size);
        if (residency < best_residency) {
            best = candidate;
            best_residency = residency;
        }
        if (residency == 0.0) break;
    }
    if (!best) die("could not find page-aligned expert for mmap probe");
    void *mapping = mmap(NULL, best.size, PROT_READ, MAP_PRIVATE,
                         fd, best.offset);
    if (mapping == MAP_FAILED) {
        perror("mmap expert");
        exit(1);
    }
    double residency_before = mapping_residency(mapping, best.size);
    uint64_t footprint_before = process_footprint();
    uint64_t wired_before = system_wired_bytes();
    id<MTLBuffer> blob =
        [metal->device newBufferWithBytesNoCopy:mapping length:best.size
                                        options:MTLResourceStorageModeShared
                                    deallocator:nil];
    if (!blob) die("bounded experts.bin mmap no-copy creation failed");

    const int S = 1, I = REAL_D, O = REAL_I;
    float *x = malloc((size_t)I * sizeof(float));
    int8_t *xq = malloc((size_t)I);
    float sx = 0.0f;
    if (!x || !xq) die("OOM mmap input");
    make_inputs(x, xq, &sx, S, I, 0x44556677u);
    id<MTLBuffer> xb =
        [metal->device newBufferWithBytes:xq length:(size_t)I
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> sb =
        [metal->device newBufferWithBytes:&sx length:sizeof(float)
                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> yb =
        [metal->device newBufferWithLength:(size_t)O * sizeof(float)
                                   options:MTLResourceStorageModeShared];
    size_t q_bytes = (size_t)O * I / 2;
    GpuParams p = params_for(S, I, O, QGROUP, 0, (uint32_t)q_bytes);
    double first_start = monotonic_seconds();
    run_gpu(metal, blob, xb, sb, yb, p, 1);
    double first_ms = (monotonic_seconds() - first_start) * 1000.0;
    double residency_after = mapping_residency(mapping, best.size);
    uint64_t footprint_after = process_footprint();
    uint64_t wired_after = system_wired_bytes();
    printf("[mmap] key=%s bytes=%zu resident_before_pct=%.2f "
           "resident_after_pct=%.2f first_gpu_ms=%.3f "
           "footprint_delta_mb=%.3f system_wired_delta_mb=%.3f "
           "buffer_limit_bytes=%llu whole_file_one_buffer=%s\n",
           best.key.UTF8String, best.size, residency_before, residency_after,
           first_ms, (double)((int64_t)footprint_after -
                              (int64_t)footprint_before) / 1e6,
           (double)((int64_t)wired_after - (int64_t)wired_before) / 1e6,
           (unsigned long long)metal->device.maxBufferLength,
           metal->device.maxBufferLength < 20942159872ull ? "IMPOSSIBLE" :
                                                            "POSSIBLE");
    blob = nil;
    munmap(mapping, best.size);
    close(fd);
    free(x);
    free(xq);
}

static void io_queue_probe(MetalContext *metal) {
    if (@available(macOS 13.0, *)) {
        MTLIOCommandQueueDescriptor *descriptor =
            [MTLIOCommandQueueDescriptor new];
        descriptor.type = MTLIOCommandQueueTypeConcurrent;
        descriptor.priority = MTLIOPriorityNormal;
        descriptor.maxCommandBufferCount = 8;
        NSError *error = nil;
        id<MTLIOCommandQueue> queue =
            [metal->device newIOCommandQueueWithDescriptor:descriptor
                                                     error:&error];
        printf("[metal-io] queue_create=%s%s%s\n", queue ? "PASS" : "FAIL",
               error ? " error=" : "",
               error ? error.localizedDescription.UTF8String : "");
    } else {
        printf("[metal-io] queue_create=UNAVAILABLE macos_lt_13\n");
    }
}

static NSString *default_model_dir(void) {
    return [@"~/.samosa/current/model" stringByExpandingTildeInPath];
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSString *model_dir = default_model_dir();
        const char *sustain_mode = NULL;
        double sustain_seconds = 0.0;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--model-dir") == 0 && i + 1 < argc) {
                model_dir = [[NSString stringWithUTF8String:argv[++i]]
                    stringByExpandingTildeInPath];
            } else if (strcmp(argv[i], "--sustain") == 0 && i + 1 < argc) {
                sustain_mode = argv[++i];
            } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
                sustain_seconds = atof(argv[++i]);
            } else {
                fprintf(stderr, "usage: %s [--model-dir PATH] "
                        "[--sustain gpu|cpu1|cpu4 --seconds N]\n", argv[0]);
                return 2;
            }
        }
        if ((sustain_mode && sustain_seconds <= 0.0) ||
            (!sustain_mode && sustain_seconds > 0.0))
            die("--sustain and a positive --seconds must be supplied together");
        MetalContext metal = make_metal();
        printf("[device] name=%s unified=%s max_buffer_bytes=%llu "
               "recommended_working_set_bytes=%llu current_allocated_bytes=%llu\n",
               metal.device.name.UTF8String,
               metal.device.hasUnifiedMemory ? "true" : "false",
               (unsigned long long)metal.device.maxBufferLength,
               (unsigned long long)metal.device.recommendedMaxWorkingSetSize,
               (unsigned long long)metal.device.currentAllocatedSize);
#ifdef _OPENMP
        printf("[cpu] openmp=true max_threads=%d kernel=%s\n",
               omp_get_max_threads(), IDOT_KERNEL);
#else
        printf("[cpu] openmp=false max_threads=1 kernel=%s\n", IDOT_KERNEL);
#endif
        if (sustain_mode) {
            sustain_probe(&metal, sustain_mode, sustain_seconds);
            return 0;
        }
        NSArray<ExpertRecordBox *> *records = load_record_boxes(model_dir);
        if (!records.count) die("manifest contains no expert records");
        nocopy_probe(&metal);
        io_queue_probe(&metal);
        mmap_probe(&metal, model_dir, records);
        synthetic_correctness(&metal);
        real_correctness(&metal, model_dir, records[0]);
        throughput_shape(&metal, "gate_up", REAL_D, REAL_I, 0x10203040u);
        throughput_shape(&metal, "down", REAL_I, REAL_D, 0x50607080u);
        full_layer_probe(&metal);
        synchronization_probe(&metal);
        printf("[summary] automated_energy=NOT_RUN reason=powermetrics_requires_privilege\n");
    }
    return 0;
}
