#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void* extract_nsis_bzip_create(void);
void extract_nsis_bzip_destroy(void* context);
/* Returns 0 (progress), 1 (end), or -1 (invalid data). */
int extract_nsis_bzip_read(void* context, const unsigned char* input, unsigned* input_size,
                          unsigned char* output, unsigned* output_size);
#ifdef __cplusplus
}
#endif
