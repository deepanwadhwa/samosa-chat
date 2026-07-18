#ifndef SAMOSA_METAL_EXPERT_H
#define SAMOSA_METAL_EXPERT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct samosa_metal_expert samosa_metal_expert;

/*
 * Grouped-q4 expert backend for decode or small verification/prefill batches.
 * The caller retains ownership of every expert slab; wrap/release only manage
 * the Metal view over those bytes. Exactly one command may be in flight per
 * context.
 */
samosa_metal_expert *samosa_metal_expert_create(
    int hidden, int intermediate, int group, int experts);
void samosa_metal_expert_destroy(samosa_metal_expert *context);

void *samosa_metal_expert_wrap(
    samosa_metal_expert *context, void *bytes, size_t length);
void samosa_metal_expert_unwrap(void *buffer);

int samosa_metal_expert_submit(
    samosa_metal_expert *context, void *const expert_buffers[8],
    int expert_count, const int8_t *input_q, float input_scale,
    const float route[8]);
int samosa_metal_expert_submit_batch(
    samosa_metal_expert *context, void *const expert_buffers[],
    int expert_count, const int8_t *input_q, const float input_scales[],
    const float route[], const uint32_t expert_rows[], int rows);
int samosa_metal_expert_wait(
    samosa_metal_expert *context, float *output,
    double *gpu_seconds);

const char *samosa_metal_expert_device_name(
    const samosa_metal_expert *context);

int samosa_metal_expert_open_io(
    samosa_metal_expert *context, const char *path);
int samosa_metal_expert_load_io(
    samosa_metal_expert *context, void *const buffers[],
    const uint64_t file_offsets[], const size_t sizes[], int count);

#ifdef __cplusplus
}
#endif

#endif
