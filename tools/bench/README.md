# uplcbench

A benchmark harness for llvm-uplc compatible with the output format used by
[saib-inc/cardano-plutus-vm-benchmark](https://github.com/saib-inc/cardano-plutus-vm-benchmark).

Measures end-to-end evaluation time on a directory of `.flat` files, with
per-script warmup, configurable minimum time and iteration count, and
JSON/CSV output that parses cleanly through the reference repo's
`parsers/parse_plutuz_json.py`.

## Modes

| mode | what it measures | framework equivalent |
|---|---|---|
| `compiled` (default) | one LLJIT setup + codegen for all scripts, then timed `program_entry()` calls per iteration with a fresh arena+budget | Chrysalis JIT |
| `cek` | decode flat + `uplc_cek_run` per iteration | uplc-turbo, Plutuz, plutigo |

`compiled` is what the rest of llvm-uplc's architecture optimizes for. It
amortizes LLJIT initialization (~1 s) over every script in the run — the
"compile once, run many" model.

## Usage

```bash
# Full suite, JIT mode, pretty terminal table (default)
./build/tools/bench/uplcbench benchmarks

# Quick single-script check
./build/tools/bench/uplcbench --filter auction_1-1 --iterations 20 \
    --min-time 0.5 --max-time 2.0 benchmarks

# JSON output for feeding into the reference repo's parsers
./build/tools/bench/uplcbench --format json -o results.json benchmarks

# CEK mode for apples-to-apples comparison with other VMs
./build/tools/bench/uplcbench --mode cek --format csv -o cek.csv benchmarks
```

### Options

```
--iterations N    minimum measured iterations per script   (default 50)
--warmup     N    warmup iterations per script             (default 5)
--min-time   S    minimum seconds per script               (default 5.0)
--max-time   S    maximum seconds per script               (default 30.0)
--format     FMT  terminal | json | csv                    (default terminal)
--mode       M    compiled | cek                           (default compiled)
--filter     SUB  only scripts whose stem matches SUB
-o           FILE write output to FILE (default stdout)
```

The measurement loop runs until **both** `--iterations` and `--min-time` are
satisfied, then stops. `--max-time` is a hard ceiling to keep the suite
runtime bounded when scripts are slow.

## Output schemas

### Terminal (default)

```
  uplcbench (llvm-uplc-jit, mode=compiled)
  LLJIT + codegen warmup: 12083 ms (one-time, amortized across all scripts)

+--------------+--------------+--------------+--------------+--------------+----------+
| script       |       median |          min |          max |       stddev |    iters |
+--------------+--------------+--------------+--------------+--------------+----------+
| auction_1-1  |    163.00 µs |    143.54 µs |    293.71 µs |     13.84 µs |     2987 |
| auction_1-2  |    442.58 µs |    400.33 µs |    607.17 µs |     28.00 µs |     1103 |
| auction_1-3  |    451.88 µs |    405.92 µs |    606.21 µs |     28.65 µs |     1078 |
| auction_1-4  |    212.33 µs |    187.54 µs |       1.45 ms |     30.02 µs |     2300 |
+--------------+--------------+--------------+--------------+--------------+----------+

  scripts run     : 4
  succeeded       : 4
  failed          : 0
  sum of medians  : 1.27 ms
  geomean median  : 288.44 µs
```

Geometric mean of per-script medians is the summary statistic to compare
against other VMs — it matches the reference repo's methodology and handles
the wide range of per-script runtimes without being dominated by outliers.

### JSON

```json
{
  "benchmarks": [
    {"name": "auction_1-1", "mean_ns": 167159, "median_ns": 165625,
     "min_ns": 141958, "max_ns": 295958, "stddev_ns": 15556,
     "iterations": 2988}
  ],
  "failures": [
    {"name": "broken_script", "error": "compile failure"}
  ]
}
```

This format is compatible with `parse_plutuz_json.py` from the reference repo:

```bash
python3 parsers/parse_plutuz_json.py results.json > llvm-uplc.csv
```

### CSV

```
vm,script,mean_ns,median_ns,min_ns,max_ns,stddev_ns,iterations
llvm-uplc-jit,auction_1-1,167159,165625,141958,295958,15556,2988
...
```

`vm` is either `llvm-uplc-jit` or `llvm-uplc-cek` depending on `--mode`.
Scripts that failed to benchmark appear with `-1` timings and `iterations=0`,
matching the reference repo's `fill_failures.py` convention.

## Adding the results to the reference repo

1. Run `uplcbench --format json -o llvm-uplc-raw.json benchmarks`
2. Feed through `parse_plutuz_json.py` to get the unified CSV
3. Drop in as another row in the reference repo's summary table
