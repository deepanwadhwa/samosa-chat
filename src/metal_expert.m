#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "metal_expert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SAMOSA_GPU_THREADS = 256, SAMOSA_GPU_SIMDGROUPS = 8,
    SAMOSA_GPU_MAX_EXPERTS = 64, SAMOSA_GPU_MAX_ROWS = 16
};

typedef struct {
    uint32_t D, I, group, experts, rows;
    uint32_t gate_q, gate_s, up_q, up_s, down_q, down_s;
} SamosaMetalParams;

/*
 * The eight expert slabs stay separate. That is important: joining them into
 * a temporary 15 MB buffer would erase the zero-copy/cache advantage we are
 * trying to measure. The expert index is uniform for a whole threadgroup, so
 * the switch does not create per-lane divergence.
 */
static const char *kSamosaMetalSource =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Params { uint D, I, group, experts, rows; uint gate_q, gate_s, up_q, up_s, down_q, down_s; };\n"
"struct ExpertArgs { array<device uchar *, 64> experts [[id(0)]]; };\n"
"kernel void gate_up(constant ExpertArgs &a [[buffer(0)]],\n"
" const device char *xq [[buffer(1)]], const device float *sx [[buffer(2)]],\n"
" const device float *route [[buffer(3)]], const device uint *expert_rows [[buffer(4)]],\n"
" device float *hidden [[buffer(5)]], constant Params &p [[buffer(6)]],\n"
" uint2 tg [[threadgroup_position_in_grid]], uint lane [[thread_index_in_simdgroup]],\n"
" uint simd [[simdgroup_index_in_threadgroup]]) {\n"
" uint o=tg.x*8u+simd, e=tg.y; if(o>=p.I||e>=p.experts)return;\n"
" uint r=expert_rows[e]; if(r>=p.rows||route[e]==0.0f)return;\n"
" const device uchar *ep=a.experts[e];\n"
" uint groups=p.D/p.group, rb=p.D/2u; float ga=0.0f, up=0.0f;\n"
" for(uint g=0;g<groups;++g){ uint i=g*p.group+lane;\n"
"  uchar gb=ep[p.gate_q+o*rb+(i>>1)];\n"
"  uchar ub=ep[p.up_q+o*rb+(i>>1)];\n"
"  int gw=int((i&1u)?(gb>>4):(gb&15u))-8, uw=int((i&1u)?(ub>>4):(ub&15u))-8;\n"
"  int xv=int(xq[r*p.D+i]);\n"
"  ga+=float(gw*xv)*((const device float*)(ep+p.gate_s))[o*groups+g];\n"
"  up+=float(uw*xv)*((const device float*)(ep+p.up_s))[o*groups+g];\n"
" }\n"
" float gs=simd_sum(ga)*sx[r], us=simd_sum(up)*sx[r];\n"
" if(lane==0u) hidden[e*p.I+o]=(gs/(1.0f+exp(-gs)))*us;\n"
"}\n"
"kernel void quant_hidden(const device float *hidden [[buffer(0)]], device char *hidden_q [[buffer(1)]],\n"
" device float *hidden_s [[buffer(2)]], const device float *route [[buffer(3)]],\n"
" constant Params &p [[buffer(4)]], uint e [[threadgroup_position_in_grid]],\n"
" uint tid [[thread_index_in_threadgroup]],\n"
" threadgroup float *scratch [[threadgroup(0)]]) {\n"
" if(e>=p.experts||route[e]==0.0f)return;\n"
" float mx=0.0f; for(uint i=tid;i<p.I;i+=256u)mx=max(mx,abs(hidden[e*p.I+i]));\n"
" scratch[tid]=mx; threadgroup_barrier(mem_flags::mem_threadgroup);\n"
" for(uint w=128u;w>0u;w>>=1u){if(tid<w)scratch[tid]=max(scratch[tid],scratch[tid+w]);\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);}\n"
" float s=max(scratch[0]/127.0f,1.0e-12f); if(tid==0u)hidden_s[e]=s;\n"
" for(uint i=tid;i<p.I;i+=256u){int v=int(rint(hidden[e*p.I+i]/s));\n"
"  hidden_q[e*p.I+i]=char(clamp(v,-127,127));}\n"
"}\n"
"kernel void down_reduce(constant ExpertArgs &a [[buffer(0)]],\n"
" const device char *hidden_q [[buffer(1)]], const device float *hidden_s [[buffer(2)]],\n"
" const device float *route [[buffer(3)]], const device uint *expert_rows [[buffer(4)]],\n"
" device float *output [[buffer(5)]], constant Params &p [[buffer(6)]],\n"
" uint2 tg [[threadgroup_position_in_grid]],\n"
" uint lane [[thread_index_in_simdgroup]], uint simd [[simdgroup_index_in_threadgroup]]) {\n"
" uint o=tg.x*8u+simd, r=tg.y; if(o>=p.D||r>=p.rows)return;\n"
" uint groups=p.I/p.group, rb=p.I/2u;\n"
" float sum=0.0f; for(uint e=0;e<p.experts;++e){\n"
"  if(expert_rows[e]!=r)continue; float rw=route[e]; if(rw==0.0f)continue;\n"
"  const device uchar *ep=a.experts[e];\n"
"  for(uint g=0;g<groups;++g){\n"
"  uint i=g*p.group+lane; uchar b=ep[p.down_q+o*rb+(i>>1)];\n"
"  int w=int((i&1u)?(b>>4):(b&15u))-8;\n"
"  sum+=float(w*int(hidden_q[e*p.I+i]))*((const device float*)(ep+p.down_s))[o*groups+g]*hidden_s[e]*rw;\n"
" }}\n"
" float v=simd_sum(sum); if(lane==0u)output[r*p.D+o]=v;\n"
"}\n";

@interface SamosaMetalContext : NSObject
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) id<MTLComputePipelineState> gatePipeline;
@property(nonatomic, strong) id<MTLComputePipelineState> quantPipeline;
@property(nonatomic, strong) id<MTLComputePipelineState> downPipeline;
@property(nonatomic, strong) id<MTLArgumentEncoder> argumentEncoder;
@property(nonatomic, strong) id<MTLBuffer> expertArguments;
@property(nonatomic, strong) id<MTLBuffer> inputQ;
@property(nonatomic, strong) id<MTLBuffer> inputScale;
@property(nonatomic, strong) id<MTLBuffer> route;
@property(nonatomic, strong) id<MTLBuffer> expertRows;
@property(nonatomic, strong) id<MTLBuffer> hidden;
@property(nonatomic, strong) id<MTLBuffer> hiddenQ;
@property(nonatomic, strong) id<MTLBuffer> hiddenScale;
@property(nonatomic, strong) id<MTLBuffer> output;
@property(nonatomic, strong) id<MTLCommandBuffer> pending;
@property(nonatomic, strong) id<MTLIOCommandQueue> ioQueue;
@property(nonatomic, strong) id<MTLIOFileHandle> ioFile;
@property(nonatomic) SamosaMetalParams params;
@end
@implementation SamosaMetalContext
@end

struct samosa_metal_expert {
    void *object;
};

static id<MTLBuffer> buffer_from_handle(void *handle) {
    return (__bridge id<MTLBuffer>)handle;
}

static id<MTLComputePipelineState> make_pipeline(
    id<MTLDevice> device, id<MTLLibrary> library, NSString *name,
    NSError **error) {
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (!function) return nil;
    return [device newComputePipelineStateWithFunction:function error:error];
}

samosa_metal_expert *samosa_metal_expert_create(
    int hidden, int intermediate, int group, int experts) {
    if (hidden <= 0 || intermediate <= 0 || group != 32 || experts != 8 ||
        hidden % group || intermediate % group)
        return NULL;
    @autoreleasepool {
        SamosaMetalContext *c = [SamosaMetalContext new];
        c.device = MTLCreateSystemDefaultDevice();
        c.queue = [c.device newCommandQueue];
        if (!c.device || !c.queue) return NULL;
        NSError *error = nil;
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.mathMode = MTLMathModeSafe;
        id<MTLLibrary> library =
            [c.device newLibraryWithSource:
                [NSString stringWithUTF8String:kSamosaMetalSource]
                                     options:options error:&error];
        if (!library) {
            fprintf(stderr, "[metal] shader compile failed: %s\n",
                    error.localizedDescription.UTF8String);
            return NULL;
        }
        id<MTLFunction> gateFunction =
            [library newFunctionWithName:@"gate_up"];
        c.gatePipeline =
            [c.device newComputePipelineStateWithFunction:gateFunction error:&error];
        c.quantPipeline = make_pipeline(c.device, library, @"quant_hidden", &error);
        c.downPipeline = make_pipeline(c.device, library, @"down_reduce", &error);
        if (!c.gatePipeline || !c.quantPipeline || !c.downPipeline) {
            fprintf(stderr, "[metal] pipeline creation failed: %s\n",
                    error.localizedDescription.UTF8String);
            return NULL;
        }
        c.argumentEncoder =
            [gateFunction newArgumentEncoderWithBufferIndex:0];
        c.expertArguments =
            [c.device newBufferWithLength:c.argumentEncoder.encodedLength
                                  options:MTLResourceStorageModeShared];
        if (!c.argumentEncoder || !c.expertArguments) return NULL;
        uint32_t gateQ = (uint32_t)((uint64_t)intermediate * hidden / 2u);
        uint32_t gateS = (uint32_t)((uint64_t)intermediate *
                                    (hidden / group) * sizeof(float));
        uint32_t downQ = (uint32_t)((uint64_t)hidden * intermediate / 2u);
        c.params = (SamosaMetalParams){
            (uint32_t)hidden, (uint32_t)intermediate, (uint32_t)group,
            (uint32_t)experts, 1, 0, gateQ, gateQ + gateS,
            2u * gateQ + gateS, 2u * (gateQ + gateS),
            2u * (gateQ + gateS) + downQ
        };
        MTLResourceOptions shared = MTLResourceStorageModeShared;
        c.inputQ = [c.device newBufferWithLength:
                    (NSUInteger)SAMOSA_GPU_MAX_ROWS * hidden options:shared];
        c.inputScale = [c.device newBufferWithLength:
                        SAMOSA_GPU_MAX_ROWS * sizeof(float) options:shared];
        c.route = [c.device newBufferWithLength:
                   SAMOSA_GPU_MAX_EXPERTS * sizeof(float) options:shared];
        c.expertRows = [c.device newBufferWithLength:
                        SAMOSA_GPU_MAX_EXPERTS * sizeof(uint32_t)
                        options:shared];
        c.hidden = [c.device newBufferWithLength:
                    (NSUInteger)SAMOSA_GPU_MAX_EXPERTS * intermediate *
                    sizeof(float) options:shared];
        c.hiddenQ = [c.device newBufferWithLength:
                     (NSUInteger)SAMOSA_GPU_MAX_EXPERTS * intermediate
                     options:shared];
        c.hiddenScale = [c.device newBufferWithLength:
                         SAMOSA_GPU_MAX_EXPERTS * sizeof(float) options:shared];
        c.output = [c.device newBufferWithLength:
                    (NSUInteger)SAMOSA_GPU_MAX_ROWS * hidden *
                    sizeof(float) options:shared];
        if (!c.inputQ || !c.inputScale || !c.route || !c.hidden ||
            !c.expertRows || !c.hiddenQ || !c.hiddenScale || !c.output)
            return NULL;
        samosa_metal_expert *result = calloc(1, sizeof(*result));
        if (!result) return NULL;
        result->object = (__bridge_retained void *)c;
        return result;
    }
}

void samosa_metal_expert_destroy(samosa_metal_expert *context) {
    if (!context) return;
    if (context->object) {
        SamosaMetalContext *c =
            (__bridge_transfer SamosaMetalContext *)context->object;
        if (c.pending) [c.pending waitUntilCompleted];
        context->object = NULL;
        (void)c;
    }
    free(context);
}

void *samosa_metal_expert_wrap(
    samosa_metal_expert *context, void *bytes, size_t length) {
    if (!context || !bytes || !length) return NULL;
    SamosaMetalContext *c =
        (__bridge SamosaMetalContext *)context->object;
    id<MTLBuffer> buffer =
        [c.device newBufferWithBytesNoCopy:bytes length:length
                                   options:MTLResourceStorageModeShared
                               deallocator:nil];
    return buffer ? (__bridge_retained void *)buffer : NULL;
}

void samosa_metal_expert_unwrap(void *buffer) {
    if (buffer) CFRelease(buffer);
}

int samosa_metal_expert_submit(
    samosa_metal_expert *context, void *const expert_buffers[8],
    int expert_count, const int8_t *input_q, float input_scale,
    const float route[8]) {
    uint32_t expert_rows[8] = {0};
    return samosa_metal_expert_submit_batch(
        context, expert_buffers, expert_count, input_q, &input_scale, route,
        expert_rows, 1);
}

int samosa_metal_expert_submit_batch(
    samosa_metal_expert *context, void *const expert_buffers[],
    int expert_count, const int8_t *input_q, const float input_scales[],
    const float route[], const uint32_t expert_rows[], int rows) {
    if (!context || !input_q || !input_scales || !route || !expert_rows ||
        expert_count < 1 || expert_count > SAMOSA_GPU_MAX_EXPERTS ||
        rows < 1 || rows > SAMOSA_GPU_MAX_ROWS) return 0;
    SamosaMetalContext *c =
        (__bridge SamosaMetalContext *)context->object;
    if (c.pending) return 0;
    for (int i = 0; i < expert_count; ++i)
        if (!expert_buffers[i]) return 0;
    memcpy(c.inputQ.contents, input_q, (size_t)rows * c.params.D);
    memcpy(c.inputScale.contents, input_scales,
           (size_t)rows * sizeof(float));
    memcpy(c.route.contents, route, (size_t)expert_count * sizeof(float));
    memcpy(c.expertRows.contents, expert_rows,
           (size_t)expert_count * sizeof(uint32_t));
    SamosaMetalParams params = c.params;
    params.experts = (uint32_t)expert_count;
    params.rows = (uint32_t)rows;
    [c.argumentEncoder setArgumentBuffer:c.expertArguments offset:0];
    for (int i = 0; i < expert_count; ++i)
        [c.argumentEncoder setBuffer:buffer_from_handle(expert_buffers[i])
                             offset:0 atIndex:i];

    id<MTLCommandBuffer> command = [c.queue commandBuffer];
    id<MTLComputeCommandEncoder> gate = [command computeCommandEncoder];
    [gate setComputePipelineState:c.gatePipeline];
    [gate setBuffer:c.expertArguments offset:0 atIndex:0];
    for (int i = 0; i < expert_count; ++i)
        [gate useResource:buffer_from_handle(expert_buffers[i])
                   usage:MTLResourceUsageRead];
    [gate setBuffer:c.inputQ offset:0 atIndex:1];
    [gate setBuffer:c.inputScale offset:0 atIndex:2];
    [gate setBuffer:c.route offset:0 atIndex:3];
    [gate setBuffer:c.expertRows offset:0 atIndex:4];
    [gate setBuffer:c.hidden offset:0 atIndex:5];
    [gate setBytes:&params length:sizeof(params) atIndex:6];
    [gate dispatchThreadgroups:
        MTLSizeMake((c.params.I + SAMOSA_GPU_SIMDGROUPS - 1) /
                    SAMOSA_GPU_SIMDGROUPS, params.experts, 1)
        threadsPerThreadgroup:MTLSizeMake(SAMOSA_GPU_THREADS, 1, 1)];
    [gate endEncoding];

    id<MTLComputeCommandEncoder> quant = [command computeCommandEncoder];
    [quant setComputePipelineState:c.quantPipeline];
    [quant setBuffer:c.hidden offset:0 atIndex:0];
    [quant setBuffer:c.hiddenQ offset:0 atIndex:1];
    [quant setBuffer:c.hiddenScale offset:0 atIndex:2];
    [quant setBuffer:c.route offset:0 atIndex:3];
    [quant setBytes:&params length:sizeof(params) atIndex:4];
    [quant setThreadgroupMemoryLength:
        SAMOSA_GPU_THREADS * sizeof(float) atIndex:0];
    [quant dispatchThreadgroups:
        MTLSizeMake(params.experts, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(SAMOSA_GPU_THREADS, 1, 1)];
    [quant endEncoding];

    id<MTLComputeCommandEncoder> down = [command computeCommandEncoder];
    [down setComputePipelineState:c.downPipeline];
    [down setBuffer:c.expertArguments offset:0 atIndex:0];
    for (int i = 0; i < expert_count; ++i)
        [down useResource:buffer_from_handle(expert_buffers[i])
                   usage:MTLResourceUsageRead];
    [down setBuffer:c.hiddenQ offset:0 atIndex:1];
    [down setBuffer:c.hiddenScale offset:0 atIndex:2];
    [down setBuffer:c.route offset:0 atIndex:3];
    [down setBuffer:c.expertRows offset:0 atIndex:4];
    [down setBuffer:c.output offset:0 atIndex:5];
    [down setBytes:&params length:sizeof(params) atIndex:6];
    [down dispatchThreadgroups:
        MTLSizeMake((c.params.D + SAMOSA_GPU_SIMDGROUPS - 1) /
                    SAMOSA_GPU_SIMDGROUPS, params.rows, 1)
         threadsPerThreadgroup:MTLSizeMake(SAMOSA_GPU_THREADS, 1, 1)];
    [down endEncoding];
    c.params = (SamosaMetalParams){
        c.params.D, c.params.I, c.params.group, c.params.experts,
        params.rows, c.params.gate_q, c.params.gate_s, c.params.up_q,
        c.params.up_s, c.params.down_q, c.params.down_s
    };
    c.pending = command;
    [command commit];
    return 1;
}

int samosa_metal_expert_wait(
    samosa_metal_expert *context, float *output, double *gpu_seconds) {
    if (!context || !output) return 0;
    SamosaMetalContext *c =
        (__bridge SamosaMetalContext *)context->object;
    id<MTLCommandBuffer> command = c.pending;
    if (!command) return 0;
    [command waitUntilCompleted];
    int ok = command.status == MTLCommandBufferStatusCompleted;
    if (gpu_seconds)
        *gpu_seconds = command.GPUEndTime > command.GPUStartTime
            ? command.GPUEndTime - command.GPUStartTime : 0.0;
    if (ok) {
        SamosaMetalParams params = c.params;
        /* The pending command may contain 1..16 rows. Store the row count in
         * the otherwise immutable context params until wait consumes it. */
        memcpy(output, c.output.contents,
               (size_t)params.rows * params.D * sizeof(float));
    }
    else fprintf(stderr, "[metal] command failed: %s\n",
                 command.error.localizedDescription.UTF8String);
    c.pending = nil;
    return ok;
}

const char *samosa_metal_expert_device_name(
    const samosa_metal_expert *context) {
    if (!context) return NULL;
    SamosaMetalContext *c =
        (__bridge SamosaMetalContext *)context->object;
    return c.device.name.UTF8String;
}

int samosa_metal_expert_open_io(
    samosa_metal_expert *context, const char *path) {
    if (!context || !path) return 0;
    SamosaMetalContext *c =
        (__bridge SamosaMetalContext *)context->object;
    NSError *error = nil;
    MTLIOCommandQueueDescriptor *descriptor =
        [MTLIOCommandQueueDescriptor new];
    descriptor.type = MTLIOCommandQueueTypeConcurrent;
    descriptor.priority = MTLIOPriorityHigh;
    descriptor.maxCommandBufferCount = 2;
    descriptor.maxCommandsInFlight = 16;
    c.ioQueue =
        [c.device newIOCommandQueueWithDescriptor:descriptor error:&error];
    c.ioFile =
        [c.device newIOFileHandleWithURL:
            [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]
                                     error:&error];
    if (!c.ioQueue || !c.ioFile) {
        fprintf(stderr, "[metal-io] initialization failed: %s\n",
                error.localizedDescription.UTF8String);
        c.ioQueue = nil;
        c.ioFile = nil;
        return 0;
    }
    return 1;
}

int samosa_metal_expert_load_io(
    samosa_metal_expert *context, void *const buffers[],
    const uint64_t file_offsets[], const size_t sizes[], int count) {
    if (!context || !buffers || !file_offsets || !sizes ||
        count < 1 || count > 16) return 0;
    SamosaMetalContext *c =
        (__bridge SamosaMetalContext *)context->object;
    if (!c.ioQueue || !c.ioFile) return 0;
    @autoreleasepool {
        id<MTLIOCommandBuffer> command = [c.ioQueue commandBuffer];
        for (int i = 0; i < count; ++i) {
            if (!buffers[i] || !sizes[i]) return 0;
            [command loadBuffer:buffer_from_handle(buffers[i])
                         offset:0 size:sizes[i]
                   sourceHandle:c.ioFile
             sourceHandleOffset:(NSUInteger)file_offsets[i]];
        }
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLIOStatusComplete) {
            fprintf(stderr, "[metal-io] load failed: %s\n",
                    command.error.localizedDescription.UTF8String);
            return 0;
        }
    }
    return 1;
}
