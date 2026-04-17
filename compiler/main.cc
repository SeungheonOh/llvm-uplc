// uplcc — UPLC -> LLVM AOT compiler.
//
// Commands:
//   uplcc --version              print compiler version
//   uplcc --help                 print usage
//   uplcc parse FILE             parse text, convert to deBruijn, pretty-print
//   uplcc check FILE             parse and validate; no output on success
//   uplcc roundtrip FILE         parse, convert, dename, pretty-print
//   uplcc flat FILE              decode a flat-encoded program, pretty-print
//   uplcc cbor FILE              cbor-unwrap then decode a flat program
//   uplcc emit-ir FILE           emit LLVM IR for FILE to stdout
//   uplcc emit-obj FILE -o OUT   compile FILE to a native object file

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "uplc/build_paths.h"

// LLVM headers needed for native compilation and JIT.
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/LegacyPassManager.h>

#include "compiler/analysis/compile_plan.h"
#include "compiler/ast/arena.h"
#include "compiler/ast/pretty.h"
#include "compiler/ast/term.h"
#include "compiler/codegen/llvm_codegen.h"
#include "compiler/driver.h"
#include "compiler/frontend/cbor_unwrap.h"
#include "compiler/frontend/flat.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/jit/codegen_pipeline.h"
#include "compiler/jit/jit_runner.h"
#include "compiler/lowering.h"
#include "runtime/core/arena.h"
#include "runtime/compiled/entry.h"
#include "runtime/cek/readback.h"
#include "uplc/abi.h"
#include "uplc/budget.h"
#include "uplc/version.h"

#ifndef UPLCC_LLVM_VERSION
#define UPLCC_LLVM_VERSION "unknown"
#endif

namespace {

constexpr const char* kUsage =
    "usage: uplcc [--version | --help] | <command> [options] <file>\n"
    "\n"
    "Frontend commands:\n"
    "  parse FILE        parse UPLC text, convert to deBruijn, pretty-print\n"
    "  check FILE        parse + validate; exit 0 on success\n"
    "  roundtrip FILE    parse, convert, rename, pretty-print\n"
    "  flat FILE         decode a flat-encoded program, pretty-print\n"
    "  cbor FILE         cbor-unwrap then flat-decode a program, pretty-print\n"
    "\n"
    "Codegen commands:\n"
    "  emit-ir FILE      emit LLVM IR for the program to stdout\n"
    "  emit-obj FILE -o OUT\n"
    "                    compile the program to a native object file\n"
    "  emit-exe FILE -o OUT\n"
    "                    compile + statically link a standalone executable\n"
    "  run FILE          JIT-compile and evaluate the program, print result\n"
    "\n"
    "FILE may be a .uplc text file, a .flat binary, or a .cbor-wrapped flat\n"
    "binary; the encoding is inferred from the file extension.\n"
    "\n"
    "Options:\n"
    "  --version         print compiler version and exit\n"
    "  --help, -h        print this message and exit\n";

void print_version() {
    std::printf("uplcc %s (llvm %s)\n", UPLC_VERSION_STRING, UPLCC_LLVM_VERSION);
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool read_file_bytes(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return true;
}

// ---------------------------------------------------------------------------
// Frontend commands (unchanged from M1)
// ---------------------------------------------------------------------------

int run_text_command(const std::string& cmd, const std::string& path) {
    std::string source;
    if (!read_file(path, source)) {
        std::fprintf(stderr, "uplcc: cannot read %s\n", path.c_str());
        return 1;
    }
    try {
        if (cmd == "parse") {
            std::string out = uplc::frontend_parse_to_debruijn_text(source);
            std::printf("%s\n", out.c_str());
        } else if (cmd == "check") {
            uplc::frontend_check(source);
        } else if (cmd == "roundtrip") {
            std::string out = uplc::frontend_roundtrip_named(source);
            std::printf("%s\n", out.c_str());
        } else {
            std::fprintf(stderr, "uplcc: unknown command %s\n", cmd.c_str());
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uplcc: %s: %s\n", path.c_str(), e.what());
        return 2;
    }
    return 0;
}

int run_binary_command(const std::string& cmd, const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(path, bytes)) {
        std::fprintf(stderr, "uplcc: cannot read %s\n", path.c_str());
        return 1;
    }
    try {
        if (cmd == "flat") {
            std::string out = uplc::frontend_parse_flat_bytes(bytes.data(), bytes.size());
            std::printf("%s\n", out.c_str());
        } else if (cmd == "cbor") {
            std::string out = uplc::frontend_parse_cbor_bytes(bytes.data(), bytes.size());
            std::printf("%s\n", out.c_str());
        } else {
            std::fprintf(stderr, "uplcc: unknown command %s\n", cmd.c_str());
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uplcc: %s: %s\n", path.c_str(), e.what());
        return 2;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Shared pipeline lives in compiler/jit/codegen_pipeline.{h,cc}.
//
// Local helper: wrap build_pipeline_from_path with the uplcc-style
// "print to stderr, return bool" convention used by the existing commands.
// ---------------------------------------------------------------------------

using uplc::Pipeline;

static bool build_pipeline(const std::string& path, Pipeline& out) {
    try {
        uplc::build_pipeline_from_path(path, out);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uplcc: %s: %s\n", path.c_str(), e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// emit-ir: print LLVM IR text to stdout
// ---------------------------------------------------------------------------

int cmd_emit_ir(const std::string& path) {
    Pipeline p;
    if (!build_pipeline(path, p)) return 2;

    try {
        llvm::LLVMContext ctx;
        uplc::LlvmCodegen cg(ctx, path);
        cg.emit(p.db, p.plan);
        std::printf("%s", cg.get_ir().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uplcc: emit-ir: %s\n", e.what());
        return 2;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// emit-obj: compile to a native object file
// ---------------------------------------------------------------------------

int cmd_emit_obj(const std::string& path, const std::string& out_path) {
    Pipeline p;
    if (!build_pipeline(path, p)) return 2;

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    try {
        llvm::LLVMContext ctx;
        std::unique_ptr<llvm::TargetMachine> tm =
            uplc::make_host_target_machine("emit-obj");
        if (!tm) return 2;

        std::unique_ptr<llvm::Module> mod_ptr =
            uplc::compile_pipeline_to_optimized_module(ctx, path, p, tm.get());
        llvm::Module& mod = *mod_ptr;

        std::error_code ec;
        llvm::raw_fd_ostream dest(out_path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            std::fprintf(stderr, "uplcc: cannot open %s: %s\n",
                         out_path.c_str(), ec.message().c_str());
            return 2;
        }

        // Backend codegen pipeline (still legacy PM — addPassesToEmitFile is
        // only available there).
        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, dest, nullptr,
                                    llvm::CodeGenFileType::ObjectFile)) {
            std::fprintf(stderr, "uplcc: emit-obj: target cannot emit object files\n");
            return 2;
        }
        pm.run(mod);
        dest.flush();

        std::fprintf(stderr, "uplcc: wrote %s\n", out_path.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uplcc: emit-obj: %s\n", e.what());
        return 2;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// emit-exe: compile + link a fully-static standalone executable
//
// Pipeline:
//   1. emit-obj into a temporary .o
//   2. shell out to ${CMAKE_CXX_COMPILER} to link program.o + the
//      pre-built libuplcrunner.a (main stub) + libuplcrt.a + libuplcfrontend.a
//      + the static crypto archives → final executable
//   3. delete the temp .o
//
// The resulting binary contains zero LLVM, zero LLJIT, zero dlopen. It
// runs program_entry directly via the OS exec path.
// ---------------------------------------------------------------------------

// Quote a path for shell execution. We use posix_spawn() in the future,
// for now system() with minimal shell-quoting works because none of the
// configure-time paths contain spaces or shell metachars on a typical
// install.
std::string sh_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out.push_back('\'');
    return out;
}

int cmd_emit_exe(const std::string& path, const std::string& out_path) {
    // Stage 1: object file in a temp location next to the output.
    std::string tmp_obj = out_path + ".uplc.o";

    int rc = cmd_emit_obj(path, tmp_obj);
    if (rc != 0) return rc;

    // Stage 2: link.
    //
    // We invoke the same C++ compiler that built uplcc, treating it as
    // the linker driver so we automatically get the host C++ runtime,
    // libm, libSystem, and the right -macosx_version_min flags.
    //
    // Object inputs and library order:
    //   program.o                user code (defines program_entry, version)
    //   libuplcrunner.a          force-loaded — pulls in main()
    //   libuplcfrontend.a        readback / lift / pretty-print
    //   libuplcrt.a              the C runtime + builtins
    //   libblst.a                pairing
    //   libcrypto.a (openssl)    sha3 / keccak / ripemd
    //   libsodium.a              sha2 / blake2b / ed25519
    //   libsecp256k1.a           ECDSA / Schnorr
    //   libgmp.a                 bigints
    //
    // libuplcrunner.a is force-loaded so its `main` is included even
    // though no other input references it.
    std::string cmd;
    cmd  = sh_quote(UPLC_CXX_COMPILER);
    cmd += " -o " + sh_quote(out_path);
    cmd += " " + sh_quote(tmp_obj);
#ifdef __APPLE__
    cmd += " -Wl,-force_load," + sh_quote(UPLC_RUNNER_LIB);
#else
    cmd += " -Wl,--whole-archive " + sh_quote(UPLC_RUNNER_LIB);
    cmd += " -Wl,--no-whole-archive";
#endif
    cmd += " " + sh_quote(UPLC_FRONTEND_LIB);
    cmd += " " + sh_quote(UPLC_RUNTIME_LIB);
    cmd += " " + sh_quote(UPLC_BLST_LIB);
    if (UPLC_CRYPTO_STATIC[0])    cmd += " " + sh_quote(UPLC_CRYPTO_STATIC);
    if (UPLC_SODIUM_STATIC[0])    cmd += " " + sh_quote(UPLC_SODIUM_STATIC);
    if (UPLC_SECP256K1_STATIC[0]) cmd += " " + sh_quote(UPLC_SECP256K1_STATIC);
    if (UPLC_GMP_STATIC[0])       cmd += " " + sh_quote(UPLC_GMP_STATIC);
#ifdef __APPLE__
    /* libSystem on macOS is only available as a dylib — that's fine, it's
     * mapped from the dyld shared cache at exec time and adds essentially
     * nothing to startup. */
    cmd += " -framework CoreFoundation";
#else
    cmd += " -lpthread -ldl -lm";
#endif

    int wstatus = std::system(cmd.c_str());
    int link_rc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;

    // Stage 3: clean up the temp object file regardless of link outcome.
    std::remove(tmp_obj.c_str());

    if (link_rc != 0) {
        std::fprintf(stderr, "uplcc: emit-exe: linker exited %d\n", link_rc);
        return 2;
    }

    std::fprintf(stderr, "uplcc: wrote %s\n", out_path.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// run: JIT-compile + evaluate, print result and budget
// ---------------------------------------------------------------------------

// Turn an llvm::Error into a string for stderr.
static std::string err_str(llvm::Error e) {
    std::string s;
    llvm::raw_string_ostream os(s);
    llvm::logAllUnhandledErrors(std::move(e), os, "");
    return s;
}

int cmd_run(const std::string& path) {
    Pipeline p;
    if (!build_pipeline(path, p)) return 2;

    /* All the LLJIT setup, symbol registration, codegen, optimization,
     * and module loading lives behind uplc::JitRunner now. The runner
     * must outlive entry_fn since the JIT-allocated executable pages
     * are owned by it. */
    std::unique_ptr<uplc::JitRunner> jit;
    uplc_program_entry entry_fn = nullptr;
    try {
        jit = std::make_unique<uplc::JitRunner>();
        entry_fn = jit->add_pipeline(p, path).entry;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uplcc: run: %s\n", e.what());
        return 2;
    }

    // Evaluate via the compiled-mode runner (handles setjmp/fail trampoline).
    uplc_arena*  arena = uplc_arena_create();
    uplc_budget  budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);

    uplc_compiled_result result = uplcrt_run_compiled(entry_fn, arena, &budget);

    int rc = 0;
    if (!result.ok) {
        const char* msg = result.fail_message ? result.fail_message : "(no message)";
        std::fprintf(stderr, "uplcc: run: evaluation failure: %s\n", msg);
        rc = 3;
    } else {
        // Readback value -> rterm -> compiler AST -> pretty-print.
        uplc_rterm* rt = uplcrt_readback(arena, result.value);
        uplc::Arena ca;
        uplc_rprogram rp;
        rp.version.major = p.db.version.major;
        rp.version.minor = p.db.version.minor;
        rp.version.patch = p.db.version.patch;
        rp.term = rt;
        uplc::Program lifted = uplc::lift_program_from_runtime(ca, rp);
        std::printf("%s\n", uplc::pretty_print_program(lifted).c_str());
    }

    // Report budget consumed (saturating: initial - remaining).
    int64_t cpu_used = uplcrt_sat_add_i64(INT64_MAX, -budget.cpu);
    int64_t mem_used = uplcrt_sat_add_i64(INT64_MAX, -budget.mem);
    std::fprintf(stderr, "ExBudget { cpu = %lld, mem = %lld }\n",
                 (long long)cpu_used, (long long)mem_used);

    uplc_arena_destroy(arena);
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs(kUsage, stderr);
        return 1;
    }

    const std::string_view first = argv[1];

    if (first == "--version") { print_version(); return 0; }
    if (first == "--help" || first == "-h") { std::fputs(kUsage, stdout); return 0; }

    if (first == "parse" || first == "check" || first == "roundtrip") {
        if (argc < 3) {
            std::fprintf(stderr, "uplcc: %.*s requires a file argument\n",
                         static_cast<int>(first.size()), first.data());
            return 1;
        }
        return run_text_command(std::string(first), argv[2]);
    }

    if (first == "flat" || first == "cbor") {
        if (argc < 3) {
            std::fprintf(stderr, "uplcc: %.*s requires a file argument\n",
                         static_cast<int>(first.size()), first.data());
            return 1;
        }
        return run_binary_command(std::string(first), argv[2]);
    }

    if (first == "emit-ir") {
        if (argc < 3) {
            std::fprintf(stderr, "uplcc: emit-ir requires a file argument\n");
            return 1;
        }
        return cmd_emit_ir(argv[2]);
    }

    if (first == "emit-obj") {
        // emit-obj FILE -o OUT
        if (argc < 5 || std::string_view(argv[3]) != "-o") {
            std::fprintf(stderr, "usage: uplcc emit-obj FILE -o OUTPUT\n");
            return 1;
        }
        return cmd_emit_obj(argv[2], argv[4]);
    }

    if (first == "emit-exe") {
        // emit-exe FILE -o OUT
        if (argc < 5 || std::string_view(argv[3]) != "-o") {
            std::fprintf(stderr, "usage: uplcc emit-exe FILE -o OUTPUT\n");
            return 1;
        }
        return cmd_emit_exe(argv[2], argv[4]);
    }

    if (first == "run") {
        if (argc < 3) {
            std::fprintf(stderr, "uplcc: run requires a file argument\n");
            return 1;
        }
        return cmd_run(argv[2]);
    }

    std::fputs(kUsage, stderr);
    return 1;
}
