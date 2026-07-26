#include <stdio.h>
#include <stdlib.h>
#include "src/vision.h"

// I'll test vision load base64 directly
int main() {
    // 1 pixel red image
    const char *b64 = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
    int grid_t, grid_h, grid_w;
    float *pixels = vision_load_base64(b64, &grid_t, &grid_h, &grid_w);
    if (!pixels) {
        printf("Failed to load base64\n");
        return 1;
    }
    printf("Loaded 1x1 image! grids: t=%d h=%d w=%d\n", grid_t, grid_h, grid_w);
    printf("Number of tokens: %d\n", grid_t * (grid_h / MERGE_SIZE) * (grid_w / MERGE_SIZE));
    free(pixels);
    return 0;
}
