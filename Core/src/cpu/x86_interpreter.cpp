#include "x86_state.h"

#include "../mem/xbox_memory.h"

namespace ember {

namespace {

struct Operand {
    bool     isReg;
    uint32_t index; // register number when isReg
    uint32_t ea;    // effective address otherwise
};

// One instruction's worth of decode state. Any failed memory access sets
// `fault`; the main loop checks it once per instruction and discards the
// partially executed instruction's EIP update.
struct Step {
    X86State&   s;
    XboxMemory& mem;
    uint32_t    ip;
    uint32_t    segBase   = 0;
    bool        fault     = false;
    uint32_t    faultAddr = 0;

    template <typename T> T load(uint32_t addr) {
        T v{};
        if (!fault && !mem.read(addr, v)) {
            fault = true;
            faultAddr = addr;
        }
        return v;
    }
    void store32(uint32_t addr, uint32_t v) {
        if (!fault && !mem.write(addr, v)) {
            fault = true;
            faultAddr = addr;
        }
    }
    uint8_t  fetch8() { const uint8_t v = load<uint8_t>(ip); ip += 1; return v; }
    uint32_t fetch32() { const uint32_t v = load<uint32_t>(ip); ip += 4; return v; }
    uint32_t fetchSigned8() { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(fetch8()))); }

    // ModRM (+ SIB + displacement), 32-bit addressing.
    Operand modrm(uint32_t& regField) {
        const uint8_t byte = fetch8();
        const uint32_t mod = byte >> 6, rm = byte & 7;
        regField = (byte >> 3) & 7;
        if (mod == 3) return Operand{true, rm, 0};

        uint32_t ea = 0;
        if (rm == 4) {
            const uint8_t sib = fetch8();
            const uint32_t scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
            if (index != 4) ea += s.reg[index] << scale;
            if (base == 5 && mod == 0) ea += fetch32();
            else ea += s.reg[base];
        } else if (rm == 5 && mod == 0) {
            ea = fetch32();
        } else {
            ea = s.reg[rm];
        }
        if (mod == 1) ea += fetchSigned8();
        else if (mod == 2) ea += fetch32();
        return Operand{false, 0, ea + segBase};
    }

    uint32_t read(const Operand& op) { return op.isReg ? s.reg[op.index] : load<uint32_t>(op.ea); }
    void write(const Operand& op, uint32_t v) {
        if (op.isReg) s.reg[op.index] = v;
        else store32(op.ea, v);
    }

    void push(uint32_t v) {
        store32(s.reg[ESP] - 4, v);
        if (!fault) s.reg[ESP] -= 4;
    }
    uint32_t pop() {
        const uint32_t v = load<uint32_t>(s.reg[ESP]);
        if (!fault) s.reg[ESP] += 4;
        return v;
    }
};

inline void setFlag(X86State& s, uint32_t flag, bool on) {
    s.eflags = on ? (s.eflags | flag) : (s.eflags & ~flag);
}

inline void setResultFlags(X86State& s, uint32_t r) {
    setFlag(s, kFlagZF, r == 0);
    setFlag(s, kFlagSF, (r >> 31) != 0);
    setFlag(s, kFlagPF, !__builtin_parity(r & 0xFF));
}

// The eight classic ALU operations, selected by bits 3..5 of the opcode or
// by the ModRM reg field of the 81/83 groups. Returns the result; `store` is
// false for CMP.
uint32_t alu(X86State& s, uint32_t op, uint32_t a, uint32_t b, bool& store) {
    uint32_t r = 0;
    bool cf = false, of = false;
    store = true;
    const uint32_t carryIn = (s.eflags & kFlagCF) ? 1 : 0;
    switch (op) {
    case 0: r = a + b; cf = r < a; of = ((~(a ^ b) & (a ^ r)) >> 31) != 0; break;                 // ADD
    case 1: r = a | b; break;                                                                     // OR
    case 2: { const uint64_t w = uint64_t(a) + b + carryIn; r = uint32_t(w); cf = (w >> 32) != 0; // ADC
              of = ((~(a ^ b) & (a ^ r)) >> 31) != 0; break; }
    case 3: { const uint64_t w = uint64_t(a) - b - carryIn; r = uint32_t(w); cf = (w >> 32) != 0; // SBB
              of = (((a ^ b) & (a ^ r)) >> 31) != 0; break; }
    case 4: r = a & b; break;                                                                     // AND
    case 6: r = a ^ b; break;                                                                     // XOR
    case 7: store = false; [[fallthrough]];                                                       // CMP
    case 5: r = a - b; cf = a < b; of = (((a ^ b) & (a ^ r)) >> 31) != 0; break;                  // SUB
    }
    setFlag(s, kFlagCF, cf);
    setFlag(s, kFlagOF, of);
    setResultFlags(s, r);
    return r;
}

uint32_t incDec(X86State& s, uint32_t v, bool decrement) { // CF is preserved
    const uint32_t r = decrement ? v - 1 : v + 1;
    setFlag(s, kFlagOF, r == (decrement ? 0x7FFFFFFFu : 0x80000000u));
    setResultFlags(s, r);
    return r;
}

bool condition(const X86State& s, uint32_t cc) {
    const bool cf = s.eflags & kFlagCF, zf = s.eflags & kFlagZF, sf = s.eflags & kFlagSF;
    const bool of = s.eflags & kFlagOF, pf = s.eflags & kFlagPF;
    bool r = false;
    switch (cc >> 1) {
    case 0: r = of; break;              // O
    case 1: r = cf; break;              // B / C
    case 2: r = zf; break;              // E / Z
    case 3: r = cf || zf; break;        // BE
    case 4: r = sf; break;              // S
    case 5: r = pf; break;              // P
    case 6: r = sf != of; break;        // L
    case 7: r = zf || (sf != of); break; // LE
    }
    return (cc & 1) ? !r : r;
}

} // namespace

RunResult interpret(X86State& s, XboxMemory& mem, uint64_t maxInstructions) {
    RunResult result;
    auto stop = [&](StopReason why) {
        result.reason = why;
        return result;
    };

    while (result.executed < maxInstructions) {
        if (s.eip - kKernelTrapBase < kKernelTrapCount * 4) {
            result.ordinal = (s.eip - kKernelTrapBase) / 4;
            return stop(StopReason::KernelCall);
        }

        Step d{s, mem, s.eip};
        uint8_t opcode = d.fetch8();
        if (opcode == 0x64) { // FS segment override
            d.segBase = s.fsBase;
            opcode = d.fetch8();
        }
        result.opcode  = opcode;
        result.address = s.eip;
        uint32_t reg = 0;
        bool unimplemented = false, store = false;

        if (opcode < 0x40 && (opcode & 7) < 6) {
            const uint32_t op = opcode >> 3, form = opcode & 7;
            if (form == 1) {            // op r/m32, r32
                const Operand rm = d.modrm(reg);
                const uint32_t r = alu(s, op, d.read(rm), s.reg[reg], store);
                if (store && !d.fault) d.write(rm, r);
            } else if (form == 3) {     // op r32, r/m32
                const Operand rm = d.modrm(reg);
                const uint32_t b = d.read(rm);
                if (!d.fault) {
                    const uint32_t r = alu(s, op, s.reg[reg], b, store);
                    if (store) s.reg[reg] = r;
                }
            } else if (form == 5) {     // op eax, imm32
                const uint32_t imm = d.fetch32();
                if (!d.fault) {
                    const uint32_t r = alu(s, op, s.reg[EAX], imm, store);
                    if (store) s.reg[EAX] = r;
                }
            } else {
                unimplemented = true;   // 8-bit forms
            }
        } else if (opcode >= 0x40 && opcode <= 0x4F) {          // inc / dec r32
            const uint32_t r = opcode & 7;
            s.reg[r] = incDec(s, s.reg[r], opcode >= 0x48);
        } else if (opcode >= 0x50 && opcode <= 0x57) {          // push r32
            d.push(s.reg[opcode & 7]);
        } else if (opcode >= 0x58 && opcode <= 0x5F) {          // pop r32
            const uint32_t v = d.pop();
            if (!d.fault) s.reg[opcode & 7] = v;
        } else if (opcode >= 0x70 && opcode <= 0x7F) {          // jcc rel8
            const uint32_t rel = d.fetchSigned8();
            if (condition(s, opcode & 15)) d.ip += rel;
        } else if (opcode >= 0xB8 && opcode <= 0xBF) {          // mov r32, imm32
            const uint32_t imm = d.fetch32();
            if (!d.fault) s.reg[opcode & 7] = imm;
        } else {
            switch (opcode) {
            case 0x0F: {
                const uint8_t second = d.fetch8();
                if (second >= 0x80 && second <= 0x8F) {         // jcc rel32
                    const uint32_t rel = d.fetch32();
                    if (condition(s, second & 15)) d.ip += rel;
                } else {
                    result.opcode = 0x0F00u | second;
                    unimplemented = true;
                }
                break;
            }
            case 0x68: d.push(d.fetch32()); break;              // push imm32
            case 0x6A: d.push(d.fetchSigned8()); break;         // push imm8
            case 0x81: case 0x83: {                             // group 1: op r/m32, imm
                const Operand rm = d.modrm(reg);
                const uint32_t imm = opcode == 0x81 ? d.fetch32() : d.fetchSigned8();
                const uint32_t a = d.read(rm);
                if (!d.fault) {
                    const uint32_t r = alu(s, reg, a, imm, store);
                    if (store) d.write(rm, r);
                }
                break;
            }
            case 0x85: {                                        // test r/m32, r32
                const Operand rm = d.modrm(reg);
                const uint32_t a = d.read(rm);
                if (!d.fault) alu(s, 4, a, s.reg[reg], store);
                break;
            }
            case 0x89: { const Operand rm = d.modrm(reg); d.write(rm, s.reg[reg]); break; }   // mov r/m32, r32
            case 0x8B: {                                        // mov r32, r/m32
                const Operand rm = d.modrm(reg);
                const uint32_t v = d.read(rm);
                if (!d.fault) s.reg[reg] = v;
                break;
            }
            case 0x8D: {                                        // lea r32, m
                const Operand rm = d.modrm(reg);
                if (rm.isReg) unimplemented = true;
                else s.reg[reg] = rm.ea - d.segBase;            // lea ignores segment bases
                break;
            }
            case 0x90: break;                                   // nop
            case 0xC2: {                                        // ret imm16 (stdcall epilogue)
                const uint32_t bytes = d.load<uint16_t>(d.ip);
                const uint32_t target = d.pop();
                if (!d.fault) { s.reg[ESP] += bytes; d.ip = target; }
                break;
            }
            case 0xC3: { const uint32_t target = d.pop(); if (!d.fault) d.ip = target; break; } // ret
            case 0xC7: {                                        // mov r/m32, imm32
                const Operand rm = d.modrm(reg);
                if (reg != 0) { unimplemented = true; break; }
                d.write(rm, d.fetch32());
                break;
            }
            case 0xCC:
                s.eip = d.ip;
                result.executed++;
                return stop(StopReason::Breakpoint);
            case 0xE8: { const uint32_t rel = d.fetch32(); d.push(d.ip); d.ip += rel; break; }  // call rel32
            case 0xE9: { const uint32_t rel = d.fetch32(); d.ip += rel; break; }                // jmp rel32
            case 0xEB: { const uint32_t rel = d.fetchSigned8(); d.ip += rel; break; }           // jmp rel8
            case 0xF4:
                s.eip = d.ip;
                result.executed++;
                return stop(StopReason::Halt);
            case 0xFF: {                                        // group 5
                const Operand rm = d.modrm(reg);
                const uint32_t v = d.read(rm);
                if (d.fault) break;
                switch (reg) {
                case 0: d.write(rm, incDec(s, v, false)); break;
                case 1: d.write(rm, incDec(s, v, true)); break;
                case 2: d.push(d.ip); d.ip = v; break;          // call r/m32 (how thunks are called)
                case 4: d.ip = v; break;                        // jmp r/m32
                case 6: d.push(v); break;
                default: unimplemented = true;
                }
                break;
            }
            default:
                unimplemented = true;
            }
        }

        if (d.fault) {
            result.address = d.faultAddr;
            return stop(StopReason::Fault);
        }
        if (unimplemented) return stop(StopReason::Unimplemented);

        s.eip = d.ip;
        result.executed++;
    }
    return result;
}

} // namespace ember
