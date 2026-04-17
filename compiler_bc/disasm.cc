#include "compiler_bc/disasm.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include <gmp.h>

#include "runtime/core/rterm.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/bytecode.h"

namespace uplc_bc {

namespace {

/* ---- opcode names ------------------------------------------------------ */

const char* opcode_name(uint8_t op) {
    switch (op) {
        case UPLC_BC_VAR_LOCAL:  return "VAR_LOCAL";
        case UPLC_BC_VAR_UPVAL:  return "VAR_UPVAL";
        case UPLC_BC_CONST:      return "CONST";
        case UPLC_BC_BUILTIN:    return "BUILTIN";
        case UPLC_BC_MK_LAM:     return "MK_LAM";
        case UPLC_BC_MK_DELAY:   return "MK_DELAY";
        case UPLC_BC_APPLY:      return "APPLY";
        case UPLC_BC_FORCE:      return "FORCE";
        case UPLC_BC_CONSTR:     return "CONSTR";
        case UPLC_BC_CASE:       return "CASE";
        case UPLC_BC_RETURN:     return "RETURN";
        case UPLC_BC_ERROR:      return "ERROR";
        case UPLC_BC_TAIL_APPLY: return "TAIL_APPLY";
        case UPLC_BC_TAIL_FORCE: return "TAIL_FORCE";
        default:                 return "?";
    }
}

/* ---- formatting helpers ----------------------------------------------- */

void fmt_bytestring(std::ostringstream& os, const uint8_t* bytes, uint32_t len) {
    constexpr uint32_t kMax = 24;
    uint32_t n = len < kMax ? len : kMax;
    os << "#";
    for (uint32_t i = 0; i < n; ++i) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", (unsigned)bytes[i]);
        os << buf;
    }
    if (n < len) os << "…(" << len << " bytes)";
}

void fmt_integer_mpz(std::ostringstream& os, mpz_srcptr z) {
    char* s = mpz_get_str(nullptr, 10, z);
    os << s;
    /* GMP's get_str returns a buffer we own; it uses the currently
     * installed allocator, which is the default malloc here. */
    std::free(s);
}

std::string fmt_value(const uplc_value& v) {
    std::ostringstream os;
    if (v.tag == UPLC_V_CON) {
        if (uplc_value_is_int_inline(v)) {
            os << "integer " << uplc_value_int_inline(v) << " (inline)";
            return os.str();
        }
        auto* c = reinterpret_cast<uplc_rconstant*>(
            (uintptr_t)uplc_value_payload(v));
        if (c == nullptr) { os << "con NULL"; return os.str(); }
        switch ((uplc_rconst_tag)c->tag) {
            case UPLC_RCONST_INTEGER: {
                os << "integer ";
                if (c->integer.value) fmt_integer_mpz(os, c->integer.value);
                else os << "NULL";
                return os.str();
            }
            case UPLC_RCONST_BYTESTRING:
                os << "bytestring ";
                fmt_bytestring(os, c->bytestring.bytes, c->bytestring.len);
                return os.str();
            case UPLC_RCONST_STRING:
                os << "string \"";
                for (uint32_t i = 0; i < c->string.len && i < 40; ++i) {
                    char ch = c->string.utf8[i];
                    if (ch == '"' || ch == '\\') os << '\\';
                    os << ch;
                }
                if (c->string.len > 40) os << "…";
                os << "\"";
                return os.str();
            case UPLC_RCONST_BOOL:
                os << "bool " << (c->boolean.value ? "True" : "False");
                return os.str();
            case UPLC_RCONST_UNIT:
                os << "unit ()";
                return os.str();
            case UPLC_RCONST_DATA:
                os << "data (…)";  /* rdata pretty-printing would be verbose */
                return os.str();
            case UPLC_RCONST_BLS12_381_G1:      os << "bls12-381 G1";       return os.str();
            case UPLC_RCONST_BLS12_381_G2:      os << "bls12-381 G2";       return os.str();
            case UPLC_RCONST_BLS12_381_ML_RESULT: os << "bls12-381 MlRes";    return os.str();
            case UPLC_RCONST_VALUE:             os << "ledger value";        return os.str();
            case UPLC_RCONST_LIST:
                os << "list[" << c->list.n_values << "]";
                return os.str();
            case UPLC_RCONST_PAIR:              os << "pair";                return os.str();
            case UPLC_RCONST_ARRAY:
                os << "array[" << c->array.n_values << "]";
                return os.str();
        }
        os << "con (unknown tag " << (unsigned)c->tag << ")";
        return os.str();
    }
    switch ((uplc_value_tag)v.tag) {
        case UPLC_V_LAM:     os << "lam";     break;
        case UPLC_V_DELAY:   os << "delay";   break;
        case UPLC_V_CONSTR:  os << "constr";  break;
        case UPLC_V_BUILTIN: os << "builtin"; break;
        default:             os << "v?";      break;
    }
    return os.str();
}

/* ---- per-function disassembly ----------------------------------------- */

void dump_fn(std::ostringstream& os,
             const uplc_bc_program& prog,
             uint32_t fn_id,
             const uplc_bc_fn& f) {
    os << "fn[" << fn_id << "] "
       << "n_args=" << f.n_args
       << " n_upvals=" << f.n_upvals
       << " max_stack=" << f.max_stack
       << " n_opcodes=" << f.n_opcodes;
    if (f.n_upvals > 0 && f.upval_outer_db) {
        os << "  upval_outer_db=[";
        for (uint32_t i = 0; i < f.n_upvals; ++i) {
            if (i) os << ",";
            os << f.upval_outer_db[i];
        }
        os << "]";
    }
    os << "\n";

    uint32_t k = 0;
    while (k < f.n_opcodes) {
        uplc_bc_word w = f.opcodes[k];
        uint8_t op = uplc_bc_op_of(w);
        uint32_t imm = uplc_bc_imm24_of(w);

        char addr[8];
        std::snprintf(addr, sizeof(addr), "%04u:", k);
        os << "  " << addr << " " << opcode_name(op);

        uint32_t next = k + 1;
        switch (op) {
            case UPLC_BC_VAR_LOCAL:
            case UPLC_BC_VAR_UPVAL:
                os << " slot=" << imm;
                break;
            case UPLC_BC_CONST: {
                os << " const[" << imm << "]";
                if (imm < prog.n_consts) {
                    os << " ; " << fmt_value(prog.consts[imm]);
                }
                break;
            }
            case UPLC_BC_BUILTIN:
                os << " tag=" << imm;
                break;
            case UPLC_BC_MK_LAM:
            case UPLC_BC_MK_DELAY: {
                uint32_t fn_id2 = imm;
                os << " fn=" << fn_id2;
                if (next < f.n_opcodes) {
                    uint32_t nfree = f.opcodes[next++];
                    os << " nfree=" << nfree << " slots=[";
                    for (uint32_t i = 0; i < nfree && next < f.n_opcodes;
                         ++i, ++next) {
                        if (i) os << ",";
                        os << f.opcodes[next];
                    }
                    os << "]";
                }
                break;
            }
            case UPLC_BC_CONSTR: {
                uint32_t n_fields = imm;
                uint64_t tag = 0;
                if (next < f.n_opcodes) { tag = f.opcodes[next++]; }
                if (next < f.n_opcodes) {
                    tag |= ((uint64_t)f.opcodes[next++]) << 32;
                }
                os << " n_fields=" << n_fields << " tag=" << tag;
                break;
            }
            case UPLC_BC_CASE: {
                uint32_t n_alts = imm;
                os << " n_alts=" << n_alts << " alts=[";
                for (uint32_t i = 0; i < n_alts; ++i) {
                    if (i) os << " ";
                    if (next >= f.n_opcodes) break;
                    uint32_t alt_fn = f.opcodes[next++];
                    uint32_t nfree  = 0;
                    if (next < f.n_opcodes) nfree = f.opcodes[next++];
                    os << "(fn=" << alt_fn << " nfree=" << nfree;
                    if (nfree > 0) {
                        os << " slots=[";
                        for (uint32_t j = 0; j < nfree && next < f.n_opcodes;
                             ++j, ++next) {
                            if (j) os << ",";
                            os << f.opcodes[next];
                        }
                        os << "]";
                    }
                    os << ")";
                }
                os << "]";
                break;
            }
            default:
                /* APPLY / FORCE / TAIL_APPLY / TAIL_FORCE / RETURN /
                 * ERROR — no operand words. */
                break;
        }
        os << "\n";
        k = next;
    }
    os << "\n";
}

}  // namespace

std::string disassemble(const uplc_bc_program& prog) {
    std::ostringstream os;
    os << "program " << prog.version_major << "."
       << prog.version_minor << "." << prog.version_patch
       << "  (" << prog.n_functions << " functions, "
       << prog.n_consts << " consts)\n\n";

    if (prog.n_consts > 0) {
        os << "consts:\n";
        for (uint32_t i = 0; i < prog.n_consts; ++i) {
            char idx[16];
            std::snprintf(idx, sizeof(idx), "  [%4u] ", i);
            os << idx << fmt_value(prog.consts[i]) << "\n";
        }
        os << "\n";
    }

    for (uint32_t i = 0; i < prog.n_functions; ++i) {
        dump_fn(os, prog, i, prog.functions[i]);
    }
    return os.str();
}

}  // namespace uplc_bc
