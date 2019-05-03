// -*- C++ -*-
#ifndef CPU_H
#define CPU_H

#include "memory.h"
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <cassert>

// Currently only supports the base RISC-V ISA that has fixed-length 32-bit
// instructions.  TODO: implement RISC-V ISA v2.2 1.2 Instruction Length
// Encoding
typedef uint32_t Instruction;

static const char *register_names[]{
    "0",  "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "fp", "s1", "a0",
    "a1", "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
    "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

struct RegFile {
    uint32_t regs[32]; // Integer registers

    RegFile() {
        // Mark registers as uninitialized
        std::memset(regs, 0, sizeof(regs));
    }

    uint32_t &operator[](int index) { return regs[index]; }

    static const char *get_name(int index) { return register_names[index]; }
};

// Programmer visible states for each hardware thread.
struct Context {
    RegFile regs;
    MemAddr program_counter = 0;
};

class Cpu {
public:
    Cpu(Memory &mem): mem(mem), regs() {}

    // Load an ELF program at `path` into memory and initialize architectural
    // states for execution.
    void load_program(const char *path);
    void cycle();

    MemAddr program_counter = 0;
    long n_cycle = 0;

private:
    void fetch();
    void decode();
    void read_elf_header(std::ifstream &ifs);
    // Dump out register and PC values in a readable format.
    void dump_regs();

    // Fetch-Decode instruction buffer.
    Instruction instruction_buffer;

    Memory &mem;
    RegFile regs;
    MemAddr next_program_counter = 0;
};

/* TODO */
struct FetchBuffer {
    int head;
    int tail;
    struct FetchBufferEntry {
    } *entry;
};

void fatal(const char *fmt, ...);

#endif
