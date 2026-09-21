/*
 * End-to-end workload for trying the experimental mimalloc profiling API with
 * the OpenTelemetry memory-profiling USDT contract.
 *
 * This deliberately consumes the API as it exists. In particular:
 *
 * - bytes_since_last_sample is forwarded unchanged as weighted_bytes;
 * - the callback supplies the next exponentially distributed sample interval;
 * - non-zero per-allocation sample data is requested so on_free is invoked;
 * - pointers and sizes received by the callbacks are emitted unchanged.
 *
 * The program prints exact application-side totals for comparison with the
 * alloc_space/alloc_objects and inuse_space/inuse_objects profiles emitted by
 * an external eBPF profiler.
 */

#include <mimalloc.h>
#include <mimalloc-profile.h>

#include <sys/sdt.h>

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#define TEST_NOINLINE __attribute__((noinline))
#else
#define TEST_NOINLINE
#endif

#define DEFAULT_MEAN_INTERVAL (512u * 1024u)
#define DEFAULT_ROUNDS 20u
#define DEFAULT_STARTUP_DELAY_SECONDS 10u
#define DEFAULT_HOLD_SECONDS 60u

#define SMALL_ALLOCATIONS_PER_ROUND 50000u
#define SMALL_ALLOCATION_SIZE 64u

#define LARGE_ALLOCATIONS_PER_ROUND 512u
#define LARGE_ALLOCATION_SIZE (64u * 1024u)
#define LARGE_RETAIN_EVERY 128u
#define LARGE_RETAIN_LIMIT 1024u

#define ALIGNED_ALLOCATIONS_PER_ROUND 1024u
#define ALIGNED_ALLOCATION_SIZE 4096u
#define ALIGNED_ALLOCATION_ALIGNMENT 4096u
#define ALIGNED_RETAIN_EVERY 64u
#define ALIGNED_RETAIN_LIMIT 1024u

typedef struct {
  uint64_t alloc_objects;
  uint64_t alloc_space;
  uint64_t free_objects;
  uint64_t free_space;
} ground_truth_t;

typedef struct {
  mi_profiler_t profiler;
  size_t mean_interval;
  _Atomic(uint64_t) alloc_callbacks;
  _Atomic(uint64_t) free_callbacks;
  _Atomic(uint64_t) callback_alloc_space;
  _Atomic(uint64_t) callback_weighted_bytes;
  _Atomic(uint64_t) pointer_mismatches;
} otel_memory_profiler_t;

static void* large_retained[LARGE_RETAIN_LIMIT];
static size_t large_retained_count;
static void* aligned_retained[ALIGNED_RETAIN_LIMIT];
static size_t aligned_retained_count;

static ground_truth_t small_truth;
static ground_truth_t large_truth;
static ground_truth_t aligned_truth;

/* One PRNG stream per allocating thread. Interval generation belongs to the
 * callback under the current API, so this adapter supplies it without changing
 * mimalloc's sampling machinery. */
static _Thread_local uint64_t interval_rng_state;

static uint64_t interval_random_next(void) {
  uint64_t state = interval_rng_state;
  if (state == 0) {
    state = ((uint64_t)(uintptr_t)&interval_rng_state << 1) ^
            ((uint64_t)(uintptr_t)&state >> 3) ^ UINT64_C(0x9e3779b97f4a7c15);
    if (state == 0) state = 1;
  }
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  interval_rng_state = state;
  return state * UINT64_C(2685821657736338717);
}

static size_t next_exponential_interval(size_t mean) {
  if (mean == 0) return 0;
  const uint64_t random = interval_random_next();
  const double unit = (double)((random >> 11) + 1) /
                      (double)(UINT64_C(1) << 53);
  const double interval = -log(unit) * (double)mean;
  if (interval < 1.0) return 1;
  if (interval >= (double)SIZE_MAX) return SIZE_MAX;
  return (size_t)interval;
}

/* Keep each probe in one non-inlined function so the executable contains one
 * .note.stapsdt entry per probe kind. */
static TEST_NOINLINE void otel_memory_probe_alloc(void* ptr, uint64_t size,
                                                  uint64_t weighted_bytes) {
  DTRACE_PROBE3(otel_memory, alloc, ptr, size, weighted_bytes);
}

static TEST_NOINLINE void otel_memory_probe_free(void* ptr) {
  DTRACE_PROBE1(otel_memory, free, ptr);
}

static inline otel_memory_profiler_t* otel_profiler_from_base(
    mi_profiler_t* profiler) {
  return (otel_memory_profiler_t*)profiler;
}

static size_t mi_cdecl otel_on_alloc(mi_profiler_t* profiler,
                                     mi_profiler_sample_data_t* data,
                                     void* ptr,
                                     size_t requested_size,
                                     size_t bytes_sample_rate,
                                     uint64_t bytes_since_last_sample,
                                     const mi_heap_t* heap) {
  (void)bytes_sample_rate;
  (void)heap;
  otel_memory_profiler_t* otel = otel_profiler_from_base(profiler);

  if (data != NULL && data->user_data_size >= sizeof(void*)) {
    data->user_data[0] = ptr;
  }

  atomic_fetch_add_explicit(&otel->alloc_callbacks, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&otel->callback_alloc_space,
                            (uint64_t)requested_size, memory_order_relaxed);
  atomic_fetch_add_explicit(&otel->callback_weighted_bytes,
                            bytes_since_last_sample, memory_order_relaxed);

  /* Use the current API's bytes_since_last_sample value as weighted_bytes.
   * This is intentionally not corrected in this baseline integration. */
  otel_memory_probe_alloc(ptr, (uint64_t)requested_size,
                          bytes_since_last_sample);
  return next_exponential_interval(otel->mean_interval);
}

static void mi_cdecl otel_on_free(mi_profiler_t* profiler,
                                  mi_profiler_sample_data_t* data,
                                  void* ptr,
                                  const mi_heap_t* heap) {
  (void)heap;
  otel_memory_profiler_t* otel = otel_profiler_from_base(profiler);

  atomic_fetch_add_explicit(&otel->free_callbacks, 1, memory_order_relaxed);
  if (data != NULL && data->user_data_size >= sizeof(void*) &&
      data->user_data[0] != ptr) {
    atomic_fetch_add_explicit(&otel->pointer_mismatches, 1,
                              memory_order_relaxed);
  }
  otel_memory_probe_free(ptr);
}

static otel_memory_profiler_t otel_profiler = {
  .profiler = {
    .reserved = NULL,
    /* The current implementation only invokes on_free when sample_data_size
     * is non-zero, so request one pointer even though the USDT needs no data. */
    .sample_data_size = sizeof(void*),
    .initial_sample_rate = DEFAULT_MEAN_INTERVAL,
    .on_alloc = &otel_on_alloc,
    .on_free = &otel_on_free,
    .on_realloc_inplace = NULL,
    .on_snapshot = NULL,
  },
  .mean_interval = DEFAULT_MEAN_INTERVAL,
};

static TEST_NOINLINE void* allocation_site_small(void) {
  void* ptr = mi_malloc(SMALL_ALLOCATION_SIZE);
  if (ptr != NULL) ((volatile unsigned char*)ptr)[0] = 0x11;
  return ptr;
}

static TEST_NOINLINE void* allocation_site_large(void) {
  void* ptr = mi_malloc(LARGE_ALLOCATION_SIZE);
  if (ptr != NULL) ((volatile unsigned char*)ptr)[0] = 0x22;
  return ptr;
}

static TEST_NOINLINE void* allocation_site_aligned(void) {
  void* ptr = mi_malloc_aligned(ALIGNED_ALLOCATION_SIZE,
                                ALIGNED_ALLOCATION_ALIGNMENT);
  if (ptr != NULL) ((volatile unsigned char*)ptr)[0] = 0x33;
  return ptr;
}

static void truth_alloc(ground_truth_t* truth, size_t size) {
  truth->alloc_objects++;
  truth->alloc_space += (uint64_t)size;
}

static void truth_free(ground_truth_t* truth, size_t size) {
  truth->free_objects++;
  truth->free_space += (uint64_t)size;
}

static TEST_NOINLINE void run_small_phase(void) {
  for (size_t i = 0; i < SMALL_ALLOCATIONS_PER_ROUND; i++) {
    void* ptr = allocation_site_small();
    if (ptr == NULL) continue;
    truth_alloc(&small_truth, SMALL_ALLOCATION_SIZE);
    mi_free(ptr);
    truth_free(&small_truth, SMALL_ALLOCATION_SIZE);
  }
}

static TEST_NOINLINE void run_large_phase(void) {
  for (size_t i = 0; i < LARGE_ALLOCATIONS_PER_ROUND; i++) {
    void* ptr = allocation_site_large();
    if (ptr == NULL) continue;
    truth_alloc(&large_truth, LARGE_ALLOCATION_SIZE);
    if ((i % LARGE_RETAIN_EVERY) == 0 &&
        large_retained_count < LARGE_RETAIN_LIMIT) {
      large_retained[large_retained_count++] = ptr;
    }
    else {
      mi_free(ptr);
      truth_free(&large_truth, LARGE_ALLOCATION_SIZE);
    }
  }
}

static TEST_NOINLINE void run_aligned_phase(void) {
  for (size_t i = 0; i < ALIGNED_ALLOCATIONS_PER_ROUND; i++) {
    void* ptr = allocation_site_aligned();
    if (ptr == NULL) continue;
    truth_alloc(&aligned_truth, ALIGNED_ALLOCATION_SIZE);
    if ((i % ALIGNED_RETAIN_EVERY) == 0 &&
        aligned_retained_count < ALIGNED_RETAIN_LIMIT) {
      aligned_retained[aligned_retained_count++] = ptr;
    }
    else {
      mi_free(ptr);
      truth_free(&aligned_truth, ALIGNED_ALLOCATION_SIZE);
    }
  }
}

static void print_truth(const char* site, const ground_truth_t* truth) {
  printf("GROUND_TRUTH site=%s alloc_objects=%" PRIu64
         " alloc_space=%" PRIu64 " inuse_objects=%" PRIu64
         " inuse_space=%" PRIu64 "\n",
         site,
         truth->alloc_objects,
         truth->alloc_space,
         truth->alloc_objects - truth->free_objects,
         truth->alloc_space - truth->free_space);
}

static uint64_t parse_u64(const char* value, const char* option) {
  char* end = NULL;
  errno = 0;
  const unsigned long long parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    fprintf(stderr, "invalid value for %s: %s\n", option, value);
    exit(2);
  }
  return (uint64_t)parsed;
}

static void busy_wait_seconds(uint64_t seconds) {
  struct timespec start;
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return;
  do {
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
  } while ((uint64_t)(now.tv_sec - start.tv_sec) < seconds);
}

static void usage(const char* program) {
  fprintf(stderr,
          "usage: %s [--rounds N] [--startup-delay N] [--hold-seconds N] "
          "[--mean-interval N]\n",
          program);
}

int main(int argc, char** argv) {
  uint64_t rounds = DEFAULT_ROUNDS;
  uint64_t startup_delay = DEFAULT_STARTUP_DELAY_SECONDS;
  uint64_t hold_seconds = DEFAULT_HOLD_SECONDS;
  uint64_t mean_interval = DEFAULT_MEAN_INTERVAL;

  for (int i = 1; i < argc; i++) {
    if (i + 1 >= argc) {
      usage(argv[0]);
      return 2;
    }
    const char* option = argv[i];
    const char* value = argv[++i];
    if (strcmp(option, "--rounds") == 0) {
      rounds = parse_u64(value, option);
    }
    else if (strcmp(option, "--startup-delay") == 0) {
      startup_delay = parse_u64(value, option);
    }
    else if (strcmp(option, "--hold-seconds") == 0) {
      hold_seconds = parse_u64(value, option);
    }
    else if (strcmp(option, "--mean-interval") == 0) {
      mean_interval = parse_u64(value, option);
    }
    else {
      usage(argv[0]);
      return 2;
    }
  }

  if (mean_interval == 0 || mean_interval > SIZE_MAX) {
    fprintf(stderr, "mean interval must be in the range 1..SIZE_MAX\n");
    return 2;
  }

  otel_profiler.mean_interval = (size_t)mean_interval;
  otel_profiler.profiler.initial_sample_rate = (size_t)mean_interval;

  printf("OTEL_MEMORY_WORKLOAD pid=%ld rounds=%" PRIu64
         " startup_delay=%" PRIu64 " hold_seconds=%" PRIu64
         " mean_interval=%" PRIu64 "\n",
         (long)getpid(), rounds, startup_delay, hold_seconds, mean_interval);
  fflush(stdout);

  /* Burn CPU before enabling sampling so a system-wide profiler has time to
   * discover this process and attach to the executable's USDT notes. */
  busy_wait_seconds(startup_delay);

  if (!mi_profile(&otel_profiler.profiler)) {
    fprintf(stderr, "failed to attach OTel memory profiler to mimalloc\n");
    return 1;
  }
  mi_profiler_start(&otel_profiler.profiler);

  for (uint64_t round = 0; round < rounds; round++) {
    run_small_phase();
    run_large_phase();
    run_aligned_phase();
  }

  print_truth("small", &small_truth);
  print_truth("large", &large_truth);
  print_truth("aligned", &aligned_truth);
  printf("ADAPTER alloc_callbacks=%" PRIu64 " free_callbacks=%" PRIu64
         " callback_alloc_space=%" PRIu64
         " callback_weighted_bytes=%" PRIu64
         " pointer_mismatches=%" PRIu64 "\n",
         atomic_load_explicit(&otel_profiler.alloc_callbacks,
                              memory_order_relaxed),
         atomic_load_explicit(&otel_profiler.free_callbacks,
                              memory_order_relaxed),
         atomic_load_explicit(&otel_profiler.callback_alloc_space,
                              memory_order_relaxed),
         atomic_load_explicit(&otel_profiler.callback_weighted_bytes,
                              memory_order_relaxed),
         atomic_load_explicit(&otel_profiler.pointer_mismatches,
                              memory_order_relaxed));
  fflush(stdout);

  if (hold_seconds > 0) sleep((unsigned int)hold_seconds);
  mi_profiler_stop(&otel_profiler.profiler);
  return 0;
}
