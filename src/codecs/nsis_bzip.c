#include "nsis_bzip.h"
#include "bzlib.h"
#include <stdlib.h>

void* extract_nsis_bzip_create(void) {
    DState* state = (DState*)calloc(1, sizeof(DState));
    if (state) { BZ2_bzDecompressInit(state); }
    return state;
}
void extract_nsis_bzip_destroy(void* context) { free(context); }
int extract_nsis_bzip_read(void* context, const unsigned char* input, unsigned* input_size,
                          unsigned char* output, unsigned* output_size) {
    DState* state = (DState*)context;
    state->next_in = (unsigned char*)input;
    state->avail_in = *input_size;
    state->next_out = output;
    state->avail_out = *output_size;
    const int result = BZ2_bzDecompress(state);
    *input_size -= state->avail_in;
    *output_size -= state->avail_out;
    return result == BZ_STREAM_END ? 1 : (result == BZ_OK ? 0 : -1);
}
