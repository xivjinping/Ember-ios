// Pentium III (Coppermine) register file for one guest thread, 32-bit flat
// protected mode — the only mode title code ever runs in.
#pragma once

#include <cstdint>

namespace ember {

enum X86Reg { EAX = 0, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

enum X86Flag : uint32_t {
    kFlagCF = 1u << 0,
    kFlagPF = 1u << 2,
    kFlagZF = 1u << 6,
    kFlagSF = 1u << 7,
    kFlagOF = 1u << 11,
};

struct X86State {
    uint32_t reg[8] = {};
    uint32_t eip    = 0;
    uint32_t eflags = 0x2; // bit 1 always reads as set
    uint32_t fsBase = 0;   // FS points at the thread's KPCR/TIB; title code uses fs:[..] constantly
    // x87, MMX and SSE state belong here too; the reference interpreter does not touch them.
};

// Calls into the kernel are HLE traps: the loader fills an XBE's kernel thunk
// table with addresses in this range (one per export ordinal). They are
// outside guest RAM, so reaching one can only mean "call export N".
constexpr uint32_t kKernelTrapBase  = 0xFFFE0000u;
constexpr uint32_t kKernelTrapCount = 512; // xboxkrnl exports ordinals 1..~378
inline uint32_t kernelTrapAddress(uint32_t ordinal) { return kKernelTrapBase + ordinal * 4; }

enum class StopReason {
    Budget,        // ran the requested number of instructions
    Halt,          // hlt
    Breakpoint,    // int3
    KernelCall,    // EIP reached a kernel trap (RunResult::ordinal)
    Unimplemented, // opcode outside the interpreter's subset (RunResult::opcode at ::address)
    Fault          // access outside guest RAM (RunResult::address)
};

struct RunResult {
    StopReason reason   = StopReason::Budget;
    uint64_t   executed = 0;
    uint32_t   opcode   = 0;
    uint32_t   address  = 0;
    uint32_t   ordinal  = 0;
};

class XboxMemory;

// Reference interpreter. Covers the 32-bit integer core that the unit tests
// exercise: mov / lea / push / pop, the eight ALU ops in their r/m32 forms,
// inc / dec, test, call / ret / jmp / jcc, with full ModRM + SIB addressing
// and the FS override. Everything else — 8- and 16-bit forms, shifts,
// mul/div, string ops, x87, MMX, SSE — stops with Unimplemented and the
// opcode, so gaps are loud. It is a correctness reference and a way to see
// how far a title gets, not a way to run one; see docs/ARCHITECTURE.md §4.
RunResult interpret(X86State& cpu, XboxMemory& mem, uint64_t maxInstructions);

} // namespace ember
