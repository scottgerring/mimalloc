# Initial OTel USDT baseline

This records the first results from `test-otel-memory.c`. The experiment uses
mimalloc's profiling API from `dev3-profile` at `33d9c019` without changing its
sampling, allocation, free, or lifecycle implementation.

## Environment

- Ubuntu Linux, arm64
- Mean sampling interval: 512 KiB
- `RelWithDebInfo` builds
- SystemTap SDT probes from `systemtap-sdt-dev`
- OTel eBPF profiler heap probe configured for `otel_memory`, allocation
  profiling, and live-heap profiling

## Reproducing the results

On Linux with `sys/sdt.h` installed, run:

```sh
test/reproduce-otel-memory-baseline.py
```

The script builds both profiling modes, runs ten fresh workload processes per
mode using a 512 KiB mean interval, and prints the results table. All workload
arguments are built into the script so the experiment is reproduced consistently.

## Results

Ten fresh processes were measured for each build mode. Each process made
515,360 allocations totalling exactly 409,487,360 application-requested bytes.

| Build mode | Runs | Mean `callback_weighted_bytes / ground_truth_alloc_space` | Standard deviation | Mean allocation callbacks | Mean alloc/free pointer mismatches |
|---|---:|---:|---:|---:|---:|
| `MI_PROFILE=FULL` | 10 | 1.9052 | 0.0476 | 1412.1 | 151.7 |
| `MI_PROFILE=ON` | 10 | 1.9011 | 0.0237 | 1402.2 | 159.8 |

### Column definitions

**Mean `callback_weighted_bytes / ground_truth_alloc_space`** divides the sum
of every `bytes_since_last_sample` value supplied to `on_alloc` by the exact
application-requested allocation bytes. The adapter forwards that callback value
unchanged as the USDT `weighted_bytes` argument. An unbiased estimator should
converge towards 1.0 over repeated runs.

**Standard deviation** describes variation in that ratio across the ten fresh
processes.

**Mean allocation callbacks** is the average number of `on_alloc` calls per
process. It counts sampled events, not application allocations. As a rough
reference, 409,487,360 bytes divided by the configured 524,288-byte mean interval
is about 781 sampling boundaries, compared with roughly 1,400 observed
callbacks.

**Mean alloc/free pointer mismatches** compares the pointer supplied to
`on_alloc`, retained in mimalloc's per-sample data, with the pointer later
supplied to `on_free`. A mismatch means that directly translating the callbacks
would emit different pointer keys in `otel_memory:alloc` and
`otel_memory:free`.

## Built-in pprof stacks

Run the isolated unwind and attribution tests with:

```sh
test/reproduce-pprof-stacks.py
```

### Unwinding

The script starts each profiling mode once with a cold unwinder, then primes
`backtrace()` before profiling and checks for a fixed three-frame allocation
stack.

| Build mode | First unwind | After priming `backtrace()` |
|---|---|---|
| `MI_PROFILE=FULL` | `SIGSEGV` | `pprof_stack_leaf` → `pprof_stack_middle` → `pprof_stack_root` |
| `MI_PROFILE=ON` | `SIGSEGV` | `pprof_stack_leaf` → `pprof_stack_middle` → `pprof_stack_root` |

The first Linux `backtrace()` lazily loads its unwinding support, which allocates
through mimalloc and recursively enters the profiler until the process crashes.
Once `backtrace()` is primed, mimalloc's unwinder captures the expected
callback-time stack in both modes.

### Attribution

Each fresh process configures a nominal 4 KiB interval and consumes the forced
start-up callbacks on a separate calibration stack. It then makes 256
allocations of 64 bytes through the `pprof_bulk_*` stack, updates the page
statistics, and makes one 80-byte slow-path allocation through the
`pprof_trigger_*` stack. The measured phase therefore allocates 16,384 bulk
bytes and 80 trigger bytes, making the ground-truth trigger share 0.486%. The
distinct call ladders are non-inlined and non-tail-called.

The table aggregates `alloc_objects` from 100 fresh pprof profiles per build.
The 95% confidence intervals are process-cluster bootstrap intervals with 20,000
resamples, so callbacks from the same process are kept together.

| Build mode | Fresh processes | Bulk-stack callbacks | Trigger-stack callbacks | Trigger share (95% CI) |
|---|---:|---:|---:|---:|
| `MI_PROFILE=FULL` | 100 | 803 | 5 | 0.62% (0.13%–1.19%) |
| `MI_PROFILE=ON` | 100 | 6 | 100 | 94.34% (87.72%–100.00%) |

`MI_PROFILE=FULL` is consistent with the 0.486% byte share. `MI_PROFILE=ON` is
not: after bulk bytes expire the batched countdown, the next slow-path trigger
allocation receives the callback and its stack. The reported stack is a valid
synchronous unwind, but it often belongs to the allocation that noticed the
expired countdown rather than to an allocation whose bytes crossed it.

## End-to-end eBPF run

Run the workload alongside the profiler from
[memory profiling 3/4: eBPF alloc probes and OTLP export](https://github.com/open-telemetry/opentelemetry-ebpf-profiler/pull/1746):

```sh
out/otel-memory-baseline/full/mimalloc-test-otel-memory \
  --startup-delay 5 \
  --rounds 2 \
  --hold-seconds 12 \
  --mean-interval 524288
```

The end-to-end path works: the profiler discovers and attaches to the
`otel_memory` probes, unwinds the allocation stacks, and emits OTLP heap
profiles. Live-heap values drift because some sampled aligned allocations use
different pointers in the alloc and free callbacks. The profiler correlates
those events by `(PID, pointer)`, so it cannot retire an allocation when the
free event carries a different pointer.

# Problems

* In `MI_PROFILE=ON`, unwinding captures the allocation that noticed the expired countdown, not the allocation that crossed it.
* Using the pprof exporter, the first Linux `backtrace()` allocates while loading unwind support, recursively re-enters profiling, and segfaults unless primed.
* (needed for eBPF) `on_alloc` returns the next interval so the consumer can control Poisson or adaptive sampling, but mimalloc leaves the effective countdown at `min(previous, new)`, causing roughly 2x callbacks and weight.
* (needed for eBPF) Sampled aligned allocations can pass the internal base pointer and over-allocation size to `on_alloc`, while `on_free` receives the final aligned pointer, breaking live-heap correlation and size accounting.
