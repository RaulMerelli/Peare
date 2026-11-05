#include "peare_mtx_bridge.h"

#include <libeot/EOTError.h>
#include "writeFontFile.h"

#include <stdlib.h>

int peare_mtx_decompress(const uint8_t* input, uint32_t input_size, int encrypted,
                         uint8_t** output, uint32_t* output_size)
{
  if (!input || input_size == 0 || !output || !output_size)
    return EOT_INSUFFICIENT_BYTES;
  *output = NULL;
  *output_size = 0;
  unsigned expanded_size = 0;
  const enum EOTError result = writeFontBuffer(input, (unsigned)input_size, true,
                                               encrypted != 0, output,
                                               &expanded_size);
  if (result != EOT_SUCCESS)
    return result;
  *output_size = expanded_size;
  return EOT_SUCCESS;
}

void peare_mtx_free(uint8_t* output)
{
  free(output);
}
