/* PANDABEGINCOMMENT
 * 
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 * 
 * This work is licensed under the terms of the GNU GPL, version 2. 
 * See the COPYING file in the top-level directory. 
 * 
PANDAENDCOMMENT */
// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <map>

#include "panda/plugin.h"

#include "../callstack_instr/callstack_instr.h"
#include "../callstack_instr/callstack_instr_ext.h"

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {
bool init_plugin(void *);
void uninit_plugin(void *);
void mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr, size_t size, uint8_t *buf);
void mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr, size_t size, uint8_t *buf);
}

struct text_counter {
    std::map<uint8_t,unsigned int> hist;
};

std::map<prog_point,text_counter> read_tracker;
std::map<prog_point,text_counter> write_tracker;

static void mem_callback(CPUState *env, target_ulong pc, target_ulong addr,
                         size_t size, uint8_t *buf,
                         std::map<prog_point, text_counter> &tracker) {
    prog_point p = {};

    get_prog_point(env, &p);

    text_counter &tc = tracker[p];
    for (unsigned int i = 0; i < size; i++) {
        uint8_t val = ((uint8_t *)buf)[i];
        tc.hist[val]++;
    }

    return;
}

void mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                        size_t size, uint8_t *buf) {
    mem_callback(env, pc, addr, size, buf, write_tracker);
    return;
}

void mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       size_t size, uint8_t *buf) {
    mem_callback(env, pc, addr, size, buf, read_tracker);
    return;
}

bool init_plugin(void *self) {
    panda_cb pcb;

    printf("Initializing plugin unigrams\n");

    panda_require("callstack_instr");
    if (!init_callstack_instr_api()) return false;

    // Need this to get EIP with our callbacks
    panda_enable_precise_pc();
    // Enable memory logging
    panda_enable_memcb();

    pcb.virt_mem_after_read = mem_read_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_AFTER_READ, pcb);
    pcb.virt_mem_after_write = mem_write_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_AFTER_WRITE, pcb);

    return true;
}

void write_report(FILE *report, std::map<prog_point,text_counter> &tracker) {
    // Cross platform support: need to know how big a target_ulong is
    uint32_t target_ulong_size = sizeof(target_ulong);
    fwrite(&target_ulong_size, sizeof(uint32_t), 1, report);
    uint32_t stack_type_size = sizeof(stack_type);
    fwrite(&stack_type_size, sizeof(uint32_t), 1, report);

    std::map<prog_point,text_counter>::iterator it;
    for(it = tracker.begin(); it != tracker.end(); it++) {
        // prog_point parts
        fwrite(&it->first.stackKind, stack_type_size, 1, report);
        fwrite(&it->first.caller, target_ulong_size, 1, report);
        fwrite(&it->first.pc, target_ulong_size, 1, report);
        fwrite(&it->first.sidFirst, target_ulong_size, 1, report);
        fwrite(&it->first.sidSecond, target_ulong_size, 1, report);
        fwrite(&it->first.isKernelMode, sizeof(bool), 1, report);

        unsigned int hist[256] = {};
        for(int i = 0; i < 256; i++) {
            if (it->second.hist.find(i) != it->second.hist.end())
                hist[i] = it->second.hist[i];
        }
        fwrite(hist, sizeof(hist), 1, report);
    }
}

void uninit_plugin(void *self) {
    FILE *mem_report;

    // Reads
    mem_report = fopen("unigram_mem_read_report.bin", "w");
    if(!mem_report) {
        printf("Couldn't write report:\n");
        perror("fopen");
        return;
    }
    write_report(mem_report, read_tracker);
    fclose(mem_report);

    // Writes
    mem_report = fopen("unigram_mem_write_report.bin", "w");
    if(!mem_report) {
        printf("Couldn't write report:\n");
        perror("fopen");
        return;
    }    
    write_report(mem_report, write_tracker);
    fclose(mem_report);
}
