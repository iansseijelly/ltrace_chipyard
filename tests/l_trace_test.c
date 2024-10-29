#include <stdio.h>
#include <riscv-pk/encoding.h>
#include "marchid.h"
#include <stdint.h>
#include "mmio.h"

#define TRACER_N_CTRL_BASE 0x10000000

#define ITER 100

void tracer_n_init() {
  reg_write8(TRACER_N_CTRL_BASE, 0x1 << 1);
}

void tracer_n_stop() {
  reg_write8(TRACER_N_CTRL_BASE, 0x0);
}

int main(void) {
  uint64_t marchid = read_csr(marchid);
  const char* march = get_march(marchid);
  printf("Hello world from core 0, a %s\n", march);
  tracer_n_init();
  printf("Trace encoder ctrl initialized\n");
  // do a simple array access
  uint8_t arr[ITER] = {0};
  for (int i = 0; i < ITER; i++) {
    arr[i] = i;
  }
  tracer_n_stop();
  // read and print the array
  // for (int i = 0; i < ITER; i++) {
    // printf("arr[%d] = %d\n", i, arr[i]);
  // }
  return 0;
}
