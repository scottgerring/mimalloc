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
