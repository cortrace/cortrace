/* A xorshift32 sequence on ITM stimulus port 0, in bursts: random but
 * reproducible trace data for judging the sampling point of a trace capture
 * (see python/cortrace/prbs.py). The core clock should already be set up; the
 * burst length and the gap keep the TPIU from saturating on a slow port: with
 * no idle time it emits no synchronisation words and the stream cannot be framed.
 *
 * Build: make RAM_BASE=0x20400000 (any RAM the core can run from).
 * Cortex-M3/M4/M7. */

#define R32(a) (*(volatile unsigned *)(a))

#define ITM_STIM0 0xE0000000u
#define ITM_TER 0xE0000E00u
#define ITM_TCR 0xE0000E80u
#define ITM_LAR 0xE0000FB0u

#define BURST_WORDS 64
#define GAP_LOOPS 6000 /* about 120 us at 240 MHz */

__attribute__((noreturn)) void reset(void)
{
  unsigned x = 0x12345678u;

  __asm volatile("cpsid i");
  R32(ITM_LAR) = 0xC5ACCE55u;
  R32(ITM_TCR) |= 1u; /* ITMENA; the host sets the rest */
  R32(ITM_TER) = 1u;
  for (;;)
    {
      for (int i = 0; i < BURST_WORDS; i++)
        {
          x ^= x << 13;
          x ^= x >> 17;
          x ^= x << 5;
          while (!(R32(ITM_STIM0) & 1u));
          R32(ITM_STIM0) = x;
        }
      for (volatile int d = 0; d < GAP_LOOPS; d++);
    }
}

extern char __stack_top[];

__attribute__((section(".vectors"), used)) void *const vectors[2] =
{
  __stack_top, reset
};
