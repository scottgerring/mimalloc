/* Exercise pprof stack unwinding and allocation-stack attribution. */

#include <mimalloc.h>
#include <mimalloc-profile.h>
#include <mimalloc/internal.h>
#include <mimalloc/prim-tls.h>

#include <stdio.h>
#include <string.h>

#if MI_HAS_EXECINFOH
#include <execinfo.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TEST_NOINLINE __attribute__((noinline))
#else
#define TEST_NOINLINE
#endif

#define UNWIND_SAMPLE_INTERVAL (64u * 1024u)
#define UNWIND_ALLOCATION_SIZE 64u
#define UNWIND_ALLOCATION_COUNT 100000u

#define ATTRIBUTION_SAMPLE_INTERVAL (4u * 1024u)
#define ATTRIBUTION_BULK_SIZE 64u
#define ATTRIBUTION_BULK_COUNT 256u
#define ATTRIBUTION_TRIGGER_SIZE 80u
#define ATTRIBUTION_CALIBRATION_SIZE 96u
#define ATTRIBUTION_CALIBRATION_COUNT 2u
#define ATTRIBUTION_RESERVE_COUNT (ATTRIBUTION_BULK_COUNT + 1u)

static void* volatile stack_sink;
static volatile size_t stack_marker;

static TEST_NOINLINE void* pprof_stack_leaf(void) {
  stack_marker = 1;
  void* ptr = mi_malloc(UNWIND_ALLOCATION_SIZE);
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_stack_middle(void) {
  void* ptr = pprof_stack_leaf();
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_stack_root(void) {
  void* ptr = pprof_stack_middle();
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_bulk_leaf(mi_heap_t* heap) {
  stack_marker = 2;
  void* ptr = mi_heap_malloc(heap, ATTRIBUTION_BULK_SIZE);
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_bulk_middle(mi_heap_t* heap) {
  void* ptr = pprof_bulk_leaf(heap);
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_bulk_root(mi_heap_t* heap) {
  void* ptr = pprof_bulk_middle(heap);
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_trigger_leaf(mi_heap_t* heap) {
  stack_marker = 3;
  void* ptr = mi_heap_malloc(heap, ATTRIBUTION_TRIGGER_SIZE);
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_trigger_middle(mi_heap_t* heap) {
  void* ptr = pprof_trigger_leaf(heap);
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_trigger_root(mi_heap_t* heap) {
  void* ptr = pprof_trigger_middle(heap);
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_calibration_leaf(mi_heap_t* heap) {
  stack_marker = 4;
  void* ptr = mi_heap_malloc(heap, ATTRIBUTION_CALIBRATION_SIZE);
  stack_sink = ptr;
  return ptr;
}

static TEST_NOINLINE void* pprof_calibration_root(mi_heap_t* heap) {
  void* ptr = pprof_calibration_leaf(heap);
  stack_sink = ptr;
  return ptr;
}

static bool prime_backtrace(void) {
#if MI_HAS_EXECINFOH
  void* frame[1];
  return backtrace(frame, 1) > 0;
#else
  return false;
#endif
}

static mi_profiler_t* start_profiler(size_t interval, const char* profile_base) {
  mi_profiler_t* profiler = mi_pprof_profiler_new(interval, profile_base, 0, 0, 0);
  if (profiler == NULL || !mi_profile(profiler)) {
    mi_pprof_profiler_delete(profiler);
    return NULL;
  }
  mi_profiler_start(profiler);
  return profiler;
}

static mi_profiler_t* start_heap_profiler(mi_heap_t* heap, size_t interval, const char* profile_base) {
  mi_profiler_t* profiler = mi_pprof_profiler_new(interval, profile_base, 0, 0, 0);
  if (profiler == NULL || !mi_heap_profile(heap, profiler)) {
    mi_pprof_profiler_delete(profiler);
    return NULL;
  }
  mi_profiler_start(profiler);
  return profiler;
}

static void finish_profiler(mi_profiler_t* profiler) {
  mi_profiler_stop(profiler);
  mi_profiler_snapshot(profiler);
  mi_profile(NULL);
  mi_pprof_profiler_delete(profiler);
}

static void finish_heap_profiler(mi_heap_t* heap, mi_profiler_t* profiler) {
  mi_profiler_stop(profiler);
  mi_profiler_snapshot(profiler);
  mi_heap_profile(heap, NULL);
  mi_pprof_profiler_delete(profiler);
}

static int run_unwind(bool prime, const char* profile_base) {
  if (prime && !prime_backtrace()) {
    fprintf(stderr, "could not prime backtrace\n");
    return 1;
  }

  mi_profiler_t* profiler = start_profiler(UNWIND_SAMPLE_INTERVAL, profile_base);
  if (profiler == NULL) {
    fprintf(stderr, "failed to create or attach pprof profiler\n");
    return 1;
  }

  for (size_t i = 0; i < UNWIND_ALLOCATION_COUNT; i++) {
    void* ptr = pprof_stack_root();
    if (ptr == NULL) {
      fprintf(stderr, "allocation failed\n");
      return 1;
    }
    mi_free(ptr);
  }

  finish_profiler(profiler);
  return 0;
}

static int run_attribution(const char* profile_base) {
  void* reserve[ATTRIBUTION_RESERVE_COUNT];
  void* bulk[ATTRIBUTION_BULK_COUNT];

  if (!prime_backtrace()) {
    fprintf(stderr, "could not prime backtrace\n");
    return 1;
  }

  mi_heap_t* heap = mi_heap_new();
  if (heap == NULL) return 1;

  /* Leave the last object live so collect keeps the current 64-byte page. */
  for (size_t i = 0; i < ATTRIBUTION_RESERVE_COUNT; i++) {
    reserve[i] = mi_heap_malloc(heap, ATTRIBUTION_BULK_SIZE);
    if (reserve[i] == NULL) return 1;
  }
  for (size_t i = 0; i + 1 < ATTRIBUTION_RESERVE_COUNT; i++) {
    mi_free(reserve[i]);
  }
  mi_heap_collect(heap, false);

  mi_profiler_t* profiler = start_heap_profiler(heap, ATTRIBUTION_SAMPLE_INTERVAL, profile_base);
  if (profiler == NULL) {
    fprintf(stderr, "failed to create or attach pprof profiler\n");
    return 1;
  }

  /* mi_profiler_start() only primes the main heap. Mirror that one-time setup
     for this isolated heap, then keep start-up callbacks out of the measured
     stacks. The second callback consumes the residual one-byte countdown. */
  mi_theap_t* theap = _mi_heap_theap_peek(heap);
  if (theap == NULL) return 1;
  _mi_theap_set_profile_sample_rate(theap, 1);
  for (size_t i = 0; i < ATTRIBUTION_CALIBRATION_COUNT; i++) {
    void* calibration = pprof_calibration_root(heap);
    if (calibration == NULL) return 1;
    mi_free(calibration);
  }

  /* These allocations stay on the prepared fast path. In MI_PROFILE=ON their
     bytes are only charged when mi_heap_collect() updates the page statistics. */
  for (size_t i = 0; i < ATTRIBUTION_BULK_COUNT; i++) {
    bulk[i] = pprof_bulk_root(heap);
    if (bulk[i] == NULL) return 1;
  }
  mi_heap_collect(heap, false);

  /* The unused 80-byte size class forces a slow path. MI_PROFILE=ON observes
     the expired countdown here, after the bulk allocation stack has returned. */
  void* trigger = pprof_trigger_root(heap);
  if (trigger == NULL) return 1;

  finish_heap_profiler(heap, profiler);

  mi_free(trigger);
  for (size_t i = 0; i < ATTRIBUTION_BULK_COUNT; i++) {
    mi_free(bulk[i]);
  }
  mi_free(reserve[ATTRIBUTION_RESERVE_COUNT - 1]);
  mi_heap_delete(heap);
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s {cold|unwind|attribution} PROFILE_BASE\n", argv[0]);
    return 2;
  }
  if (strcmp(argv[1], "cold") == 0) {
    return run_unwind(false, argv[2]);
  }
  if (strcmp(argv[1], "unwind") == 0) {
    return run_unwind(true, argv[2]);
  }
  if (strcmp(argv[1], "attribution") == 0) {
    return run_attribution(argv[2]);
  }
  fprintf(stderr, "usage: %s {cold|unwind|attribution} PROFILE_BASE\n", argv[0]);
  return 2;
}
