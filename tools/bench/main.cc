// uplcbench — benchmark harness for llvm-uplc, compatible with the output
// format used by https://github.com/saib-inc/cardano-plutus-vm-benchmark.
//
// Two modes:
//   --mode compiled  (default)
//       LLJIT-compile every script once (amortized LLJIT startup), then
//       call the compiled program_entry() in a tight loop with a fresh
//       arena + budget per iteration. This is the llvm-uplc fast path
//       that the static-exe uses under the hood.
//
//   --mode cek
//       Decode flat + run CEK interpreter per iteration. Matches what the
//       reference repo measures for every other VM (flat-decode + eval).
//
// Output format (JSON) matches Plutuz:
//   {"benchmarks":[{"name":"...", "mean_ns":N, "median_ns":N, "min_ns":N,
//                   "max_ns":N, "stddev_ns":N, "iterations":N}, ...]}
// which parses_plutuz_json.py from the reference repo ingests verbatim.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/arena.h"
#include "runtime/cek/cek.h"
#include "runtime/compiled/entry.h"
#include "runtime/errors.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

#include "compiler/analysis/compile_plan.h"
#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/flat.h"
#include "compiler/jit/codegen_pipeline.h"
#include "compiler/jit/jit_runner.h"
#include "compiler/lowering.h"

namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path             data_dir;
    std::size_t          iterations   = 50;
    std::size_t          warmup       = 5;
    double               min_time_s   = 5.0;
    double               max_time_s   = 30.0;
    std::string          format       = "terminal";
    std::string          mode         = "compiled";
    std::string          filter;
    std::optional<fs::path> output;
};

struct Stats {
    std::string  name;
    bool         ok = false;
    std::string  error;
    std::uint64_t compile_ns = 0;  // compiled mode: full pipeline → JIT. cek: 0.
    std::uint64_t mean_ns   = 0;
    std::uint64_t median_ns = 0;
    std::uint64_t min_ns    = 0;
    std::uint64_t max_ns    = 0;
    std::uint64_t stddev_ns = 0;
    std::size_t   iterations = 0;
};

constexpr const char* kUsage =
    "usage: uplcbench [options] <flat-directory>\n"
    "\n"
    "Options:\n"
    "  --iterations N    measured iterations per script (min, default 50)\n"
    "  --warmup     N    warmup iterations per script   (default 5)\n"
    "  --min-time   S    minimum seconds per script     (default 5.0)\n"
    "  --max-time   S    maximum seconds per script     (default 30.0)\n"
    "  --format     FMT  terminal | json | csv          (default terminal)\n"
    "  --mode       M    compiled | cek                 (default compiled)\n"
    "  --filter     SUB  only scripts whose stem matches SUB\n"
    "  -o FILE           write output to FILE (default stdout)\n";

bool read_bytes(const fs::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return true;
}

// ---------------------------------------------------------------------------
// Bench-side compile entry point.
//
// The shared uplc::JitRunner handles every LLJIT detail (init, symbol
// registration, JITDylib creation, codegen, optimization, lookup); the
// bench just wraps it with a per-script "decode flat → run pipeline →
// add to JIT" helper.
// ---------------------------------------------------------------------------

uplc::JitProgram compile_flat_script(uplc::JitRunner& jit,
                                     const fs::path& path,
                                     std::size_t /*script_ix*/) {
    std::vector<std::uint8_t> bytes;
    if (!read_bytes(path, bytes)) {
        throw std::runtime_error("cannot read " + path.string());
    }
    uplc::Pipeline p;
    uplc::build_pipeline_from_flat(bytes.data(), bytes.size(), p);
    return jit.add_pipeline(p, path.stem().string());
}

// One measured iteration of the compiled path: fresh arena + budget, call
// program_entry() through the setjmp trampoline, tear down. Returns ns.
std::int64_t run_one_compiled(uplc_program_entry entry) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);

    uplc_compiled_result r = uplcrt_run_compiled(entry, arena, &budget);
    (void)r;  // failures still count as timing samples

    uplc_arena_destroy(arena);
    auto t1 = clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

// One measured iteration of the CEK path: decode flat + run CEK.
std::int64_t run_one_cek(const std::vector<std::uint8_t>& bytes) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);

    try {
        uplc::Arena ca;
        uplc::Program db = uplc::decode_flat(ca, bytes.data(), bytes.size());
        uplc_rprogram rp = uplc::lower_to_runtime(arena, db);
        uplc_cek_result r = uplc_cek_run(arena, rp.term, &budget);
        (void)r;
    } catch (const std::exception&) {
        uplc_arena_destroy(arena);
        return -1;
    }

    uplc_arena_destroy(arena);
    auto t1 = clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

Stats summarize(std::string name, std::vector<std::int64_t> samples) {
    Stats s;
    s.name       = std::move(name);
    s.ok         = true;
    s.iterations = samples.size();

    std::sort(samples.begin(), samples.end());
    s.min_ns    = static_cast<std::uint64_t>(samples.front());
    s.max_ns    = static_cast<std::uint64_t>(samples.back());
    s.median_ns = static_cast<std::uint64_t>(samples[samples.size() / 2]);

    long double sum = 0.0L;
    for (auto v : samples) sum += static_cast<long double>(v);
    long double mean = sum / static_cast<long double>(samples.size());
    s.mean_ns = static_cast<std::uint64_t>(mean + 0.5L);

    long double var = 0.0L;
    for (auto v : samples) {
        long double d = static_cast<long double>(v) - mean;
        var += d * d;
    }
    var /= static_cast<long double>(samples.size());
    s.stddev_ns = static_cast<std::uint64_t>(std::sqrt(static_cast<double>(var)) + 0.5);

    return s;
}

// Shared measurement loop: warmup + timed iterations until the budget is
// exhausted. `run_one` is a std::function that does one sampled run.
template <typename F>
Stats measure_loop(const Options& opts, std::string name, F&& run_one) {
    Stats s;
    s.name = std::move(name);

    for (std::size_t i = 0; i < opts.warmup; ++i) {
        if (run_one() < 0) { s.error = "warmup failure"; return s; }
    }

    using clock = std::chrono::steady_clock;
    auto deadline_min = clock::now() + std::chrono::duration<double>(opts.min_time_s);
    auto deadline_max = clock::now() + std::chrono::duration<double>(opts.max_time_s);

    std::vector<std::int64_t> samples;
    samples.reserve(opts.iterations * 2);

    while (true) {
        std::int64_t ns = run_one();
        if (ns < 0) { s.error = "measurement failure"; return s; }
        samples.push_back(ns);

        bool min_iters = samples.size() >= opts.iterations;
        bool min_time  = clock::now() >= deadline_min;
        bool max_time  = clock::now() >= deadline_max;

        if ((min_iters && min_time) || max_time) break;
    }
    return summarize(s.name, std::move(samples));
}

// ---- Output --------------------------------------------------------------

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

void write_json(std::ostream& os, const std::vector<Stats>& results) {
    os << "{\n  \"benchmarks\": [";
    bool first = true;
    for (const auto& r : results) {
        if (!r.ok) continue;
        if (!first) os << ",";
        os << "\n    {"
           << "\"name\": \""      << json_escape(r.name) << "\","
           << " \"compile_ns\": " << r.compile_ns       << ","
           << " \"mean_ns\": "    << r.mean_ns          << ","
           << " \"median_ns\": "  << r.median_ns        << ","
           << " \"min_ns\": "     << r.min_ns           << ","
           << " \"max_ns\": "     << r.max_ns           << ","
           << " \"stddev_ns\": "  << r.stddev_ns        << ","
           << " \"iterations\": " << r.iterations
           << "}";
        first = false;
    }
    os << "\n  ],\n  \"failures\": [";
    first = true;
    for (const auto& r : results) {
        if (r.ok) continue;
        if (!first) os << ",";
        os << "\n    {"
           << "\"name\": \""  << json_escape(r.name)  << "\","
           << " \"error\": \"" << json_escape(r.error) << "\""
           << "}";
        first = false;
    }
    os << "\n  ]\n}\n";
}

void write_csv(std::ostream& os, const std::vector<Stats>& results,
               const std::string& vm) {
    os << "vm,script,compile_ns,mean_ns,median_ns,min_ns,max_ns,stddev_ns,iterations\n";
    for (const auto& r : results) {
        if (!r.ok) {
            os << vm << "," << r.name << ",-1,-1,-1,-1,-1,-1,0\n";
            continue;
        }
        os << vm << "," << r.name << ","
           << r.compile_ns << ","
           << r.mean_ns   << ","
           << r.median_ns << ","
           << r.min_ns    << ","
           << r.max_ns    << ","
           << r.stddev_ns << ","
           << r.iterations << "\n";
    }
}

std::string fmt_ns(std::uint64_t ns) {
    char buf[64];
    if (ns < 1'000)             { std::snprintf(buf, sizeof(buf), "%llu ns", (unsigned long long)ns); }
    else if (ns < 1'000'000)    { std::snprintf(buf, sizeof(buf), "%.2f µs", ns / 1e3); }
    else if (ns < 1'000'000'000){ std::snprintf(buf, sizeof(buf), "%.2f ms", ns / 1e6); }
    else                        { std::snprintf(buf, sizeof(buf), "%.2f s",  ns / 1e9); }
    return buf;
}

// Geometric mean of the median_ns across all successful results. This is
// the standard cross-benchmark summary the reference repo uses — it handles
// the wide range of per-script runtimes without letting outliers dominate.
double geomean_median_ns(const std::vector<Stats>& results) {
    long double log_sum = 0.0L;
    std::size_t n = 0;
    for (const auto& r : results) {
        if (!r.ok || r.median_ns == 0) continue;
        log_sum += std::log(static_cast<long double>(r.median_ns));
        ++n;
    }
    if (n == 0) return 0.0;
    return static_cast<double>(std::exp(log_sum / static_cast<long double>(n)));
}

// ---- Streaming terminal format ------------------------------------------
//
// One line per script, aligned by script-name column width computed from
// the longest name in the batch. Written incrementally so a multi-minute
// run shows progress instead of going silent.
//
//   1/102  auction_1-1                compile  23.4 ms   run 450 µs  (min 420 µs, max 520 µs, σ 25 µs, 50 iters)
//   2/102  FAIL auction_1-2           compile failure: cannot read file
//
// `compile` is the time for the full codegen → runtime-bitcode link → O3
// → LLJIT addIRModule path (compiled mode only). `run` is the median of
// the measured per-iteration execution time — summary metric we pay for
// with `min_time` / `max_time` seconds of sampling.

struct TermLayout {
    std::size_t index_w  = 7;       // "  N/M  "
    std::size_t name_w   = 28;      // recomputed per-run from file list
};

std::string pad_right(const std::string& s, std::size_t w) {
    if (s.size() >= w) return s;
    return s + std::string(w - s.size(), ' ');
}

std::string pad_left(const std::string& s, std::size_t w) {
    if (s.size() >= w) return s;
    return std::string(w - s.size(), ' ') + s;
}

void term_header(std::ostream& os,
                 const std::string& vm_name, const std::string& mode,
                 double jit_init_ms, std::size_t n_files) {
    os << "\nuplcbench  vm=" << vm_name
       << "  mode=" << mode
       << "  scripts=" << n_files;
    if (jit_init_ms > 0.0) {
        char cbuf[64];
        std::snprintf(cbuf, sizeof(cbuf), "%.0f ms", jit_init_ms);
        os << "  LLJIT init=" << cbuf;
    }
    os << "\n\n";
    os.flush();
}

void term_row(std::ostream& os, const TermLayout& w,
              std::size_t idx, std::size_t total, const Stats& r) {
    /* "  i/n  " with right-aligned index for steady left margin. */
    {
        char ibuf[32];
        std::snprintf(ibuf, sizeof(ibuf), "%zu/%zu", idx, total);
        os << "  " << pad_left(ibuf, w.index_w - 2) << "  ";
    }

    os << pad_right(r.name, w.name_w);

    if (!r.ok) {
        os << "  FAIL  " << r.error << "\n";
        os.flush();
        return;
    }

    /* compile column is only meaningful in compiled mode; the caller
     * leaves it at 0 for cek mode, where we print a dash. */
    std::string compile_str = r.compile_ns > 0 ? fmt_ns(r.compile_ns) : "—";

    os << "  compile " << pad_left(compile_str, 9)
       << "   run "    << pad_left(fmt_ns(r.median_ns), 9)
       << "  (min "    << fmt_ns(r.min_ns)
       << ", max "     << fmt_ns(r.max_ns)
       << ", σ "       << fmt_ns(r.stddev_ns)
       << ", "         << r.iterations << " iters)\n";
    os.flush();
}

void term_footer(std::ostream& os, const std::vector<Stats>& results) {
    std::size_t ok = 0, failed = 0;
    std::uint64_t sum_median  = 0;
    std::uint64_t sum_compile = 0;
    for (const auto& r : results) {
        if (r.ok) {
            ++ok;
            sum_median  += r.median_ns;
            sum_compile += r.compile_ns;
        } else {
            ++failed;
        }
    }
    double geo = geomean_median_ns(results);

    os << "\n"
       << "scripts=" << results.size()
       << "  ok="    << ok
       << "  failed="<< failed << "\n";
    if (ok > 0) {
        os << "compile total=" << fmt_ns(sum_compile) << "\n"
           << "run sum-of-medians=" << fmt_ns(sum_median)
           << "  run geomean-median="
           << fmt_ns(static_cast<std::uint64_t>(geo + 0.5))
           << "\n";
    }
    os << "\n";
}

// ---- Arg parsing ---------------------------------------------------------

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto need_val = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "uplcbench: %s needs a value\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { std::puts(kUsage); std::exit(0); }
        if (a == "--iterations") { o.iterations = std::stoul(need_val("--iterations")); continue; }
        if (a == "--warmup")     { o.warmup     = std::stoul(need_val("--warmup"));     continue; }
        if (a == "--min-time")   { o.min_time_s = std::stod(need_val("--min-time"));    continue; }
        if (a == "--max-time")   { o.max_time_s = std::stod(need_val("--max-time"));    continue; }
        if (a == "--format")     { o.format     = need_val("--format");                 continue; }
        if (a == "--mode")       { o.mode       = need_val("--mode");                   continue; }
        if (a == "--filter")     { o.filter     = need_val("--filter");                 continue; }
        if (a == "-o")           { o.output     = need_val("-o");                       continue; }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "uplcbench: unknown option %s\n", argv[i]);
            return false;
        }
        if (o.data_dir.empty()) { o.data_dir = argv[i]; continue; }
        std::fprintf(stderr, "uplcbench: extra positional arg %s\n", argv[i]);
        return false;
    }
    if (o.data_dir.empty()) {
        std::fprintf(stderr, "uplcbench: missing <flat-directory>\n");
        return false;
    }
    if (o.format != "json" && o.format != "csv" && o.format != "terminal") {
        std::fprintf(stderr, "uplcbench: --format must be terminal, json, or csv\n");
        return false;
    }
    if (o.mode != "compiled" && o.mode != "cek") {
        std::fprintf(stderr, "uplcbench: --mode must be 'compiled' or 'cek'\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        std::fputs(kUsage, stderr);
        return 1;
    }

    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(opts.data_dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".flat") continue;
        if (!opts.filter.empty() &&
            e.path().stem().string().find(opts.filter) == std::string::npos) continue;
        files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        std::fprintf(stderr, "uplcbench: no .flat files under %s\n",
                     opts.data_dir.c_str());
        return 1;
    }

    std::fprintf(stderr,
                 "uplcbench: mode=%s data=%s files=%zu warmup=%zu iter>=%zu "
                 "min_time=%.1fs max_time=%.1fs\n",
                 opts.mode.c_str(), opts.data_dir.c_str(), files.size(),
                 opts.warmup, opts.iterations, opts.min_time_s, opts.max_time_s);

    std::size_t name_width = 0;
    for (const auto& p : files) name_width = std::max(name_width, p.stem().string().size());

    std::vector<Stats> results;
    results.reserve(files.size());

    // -------------------------------------------------------------------
    // LLJIT one-time init (compiled mode only). Codegen happens per-script
    // inside the interleaved loop below; only the LLJIT instance itself is
    // shared and reused.
    // -------------------------------------------------------------------
    std::unique_ptr<uplc::JitRunner> jit;
    double jit_init_ms = 0.0;

    if (opts.mode == "compiled") {
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        try {
            jit = std::make_unique<uplc::JitRunner>();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "uplcbench: %s\n", e.what());
            return 1;
        }
        jit_init_ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - t0).count());
    }

    // -------------------------------------------------------------------
    // Open the output stream up front. Terminal mode streams per-row while
    // measurements run; JSON/CSV modes buffer results and write once at
    // the end (they're structured formats, row-at-a-time doesn't make
    // sense there).
    // -------------------------------------------------------------------
    std::ostream* os = &std::cout;
    std::ofstream file_out;
    if (opts.output) {
        file_out.open(*opts.output);
        if (!file_out) {
            std::fprintf(stderr, "uplcbench: cannot open %s for writing\n",
                         opts.output->c_str());
            return 1;
        }
        os = &file_out;
    }

    std::string vm_name = (opts.mode == "compiled") ? "llvm-uplc-jit" : "llvm-uplc-cek";
    bool stream_terminal = (opts.format == "terminal");
    TermLayout layout;
    layout.name_w = std::max<std::size_t>(name_width + 2, 16);
    if (stream_terminal) {
        term_header(*os, vm_name, opts.mode, jit_init_ms, files.size());
    }

    // -------------------------------------------------------------------
    // Interleaved compile + measure. For each script we:
    //   1. codegen it (compiled mode) or read its bytes (cek mode)
    //   2. run the warmup + measurement loop
    //   3. print the row immediately
    // so the user sees results as the run progresses instead of waiting
    // for a full compile-all phase first.
    // -------------------------------------------------------------------
    using clock = std::chrono::steady_clock;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& path = files[i];
        Stats s;
        const std::string& name = path.stem().string();
        std::uint64_t compile_ns = 0;

        if (opts.mode == "compiled") {
            uplc::JitProgram prog;
            auto c0 = clock::now();
            try {
                prog = compile_flat_script(*jit, path, i);
            } catch (const std::exception& e) {
                s.name  = name;
                s.error = std::string("compile failure: ") + e.what();
            }
            compile_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    clock::now() - c0).count());

            if (prog.entry) {
                uplc_program_entry entry = prog.entry;
                s = measure_loop(opts, name, [entry]() {
                    return run_one_compiled(entry);
                });
                /* Dispose this script's JITDylib before moving to the
                 * next one. Without this, every compiled script stays
                 * resident in the shared LLJIT instance — on macOS the
                 * JIT executable-memory allocator runs out of segment
                 * space partway through a full-corpus run. */
                try {
                    jit->remove(*prog.dylib);
                } catch (const std::exception& e) {
                    std::fprintf(stderr,
                                 "uplcbench: warning: remove dylib for %s: %s\n",
                                 name.c_str(), e.what());
                }
            }
        } else {
            std::vector<std::uint8_t> bytes;
            if (!read_bytes(path, bytes)) {
                s.name  = name;
                s.error = "cannot read file";
            } else {
                s = measure_loop(opts, name, [&bytes]() {
                    return run_one_cek(bytes);
                });
            }
        }

        s.compile_ns = compile_ns;
        results.push_back(s);

        if (stream_terminal) {
            term_row(*os, layout, i + 1, files.size(), s);
        } else {
            // JSON/CSV modes: dump a short progress line to stderr so the
            // user sees forward progress without us interleaving structured
            // output with free-form log lines.
            std::fprintf(stderr, "  [%3zu/%zu] %-*s  ",
                         i + 1, files.size(),
                         (int)name_width, name.c_str());
            if (!s.ok) {
                std::fprintf(stderr, "FAIL (%s)\n", s.error.c_str());
            } else {
                std::fprintf(stderr,
                             "compile %s  run %s  (min %s, %zu iters)\n",
                             fmt_ns(s.compile_ns).c_str(),
                             fmt_ns(s.median_ns).c_str(),
                             fmt_ns(s.min_ns).c_str(),
                             s.iterations);
            }
        }
    }

    if      (opts.format == "json") write_json(*os, results);
    else if (opts.format == "csv")  write_csv (*os, results, vm_name);
    else                            term_footer(*os, results);

    int failed = 0;
    for (const auto& r : results) if (!r.ok) ++failed;
    if (failed > 0) {
        std::fprintf(stderr, "uplcbench: %d script(s) failed\n", failed);
    }
    return 0;
}
