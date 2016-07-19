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
 
extern "C" {

#include "config.h"
#include "qemu-common.h"
#include "monitor.h"
#include "cpu.h"

#include "panda_plugin.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <string>

#include <iostream>
#include <unordered_map>
using namespace std;

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {

bool init_plugin(void *);
void uninit_plugin(void *);
int mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr, target_ulong size, void *buf);
int mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr, target_ulong size, void *buf);

#include "critbit.h"

}

unordered_map<string,int> matches;

#define MINWORD 4
#define WINDOW_SIZE 20
unsigned int ridx = 0;
uint8_t read_window[WINDOW_SIZE];
unsigned int widx = 0;
uint8_t write_window[WINDOW_SIZE];

critbit0_tree t;

int mem_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf, bool is_write,
                       uint8_t (&window)[WINDOW_SIZE]) {
    unsigned int idx;
    if (is_write) idx = widx;
    else idx = ridx;
    for (unsigned int i = 0; i < size; i++) {
        uint8_t val = ((uint8_t *)buf)[i];
        // Hack: skip NULLs to get free UTF-16 support
        // Also skip punctuation
        switch (val) {
            case 0: case '!': case '"': case '#': case '$':
            case '%': case '&': case '\'': case '(': case ')':
            case '*': case '+': case ',': case '-': case '.':
            case '/': case ':': case ';': case '<': case '=':
            case '>': case '?': case '@': case '[': case '\\':
            case ']': case '^': case '_': case '`': case '{':
            case '|': case '}': case '~':
                continue;
        }

        if ('a' <= val && val <= 'z') val &= ~0x20;
        window[idx++] = val;
        if (idx >= WINDOW_SIZE) idx -= WINDOW_SIZE;
    }

    unsigned int midx = idx;
    // Makes WINDOW_SIZE - MINWORD queries against the hash table
    char search[WINDOW_SIZE+1] = {};
    memcpy(search, window+midx, WINDOW_SIZE-midx);
    memcpy(search+(WINDOW_SIZE-midx), window, midx);
    for (int i = WINDOW_SIZE-1; i >= MINWORD-1; i--) {
        critbit0_contains(&t, search);
        search[i] = '\0';
    }
    if (is_write) widx = idx;
    else ridx = idx;
    return 1;
}

int mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf) {
    return mem_callback(env, pc, addr, size, buf, false, read_window);

}

int mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf) {
    return mem_callback(env, pc, addr, size, buf, true, write_window);
}

FILE *mem_report = NULL;

bool init_plugin(void *self) {
    panda_cb pcb;

    printf("Initializing plugin manyss_crit\n");

    panda_arg_list *args = panda_get_args("manyss_crit");

    const char *prefix = panda_parse_string(args, "name", "manyss_crit");
    char stringsfile[128] = {};
    sprintf(stringsfile, "%s_search_strings.txt", prefix);

    printf ("search strings file [%s]\n", stringsfile);

    std::ifstream search_strings(stringsfile);
    if (!search_strings) {
        printf("Couldn't open %s; no strings to search for. Exiting.\n", stringsfile);
        return false;
    }

    // Format: strings, one per line, uppercase
    std::string line;
    size_t nstrings = 0;
    while(std::getline(search_strings, line)) {
        critbit0_insert(&t, line.c_str());
        if (nstrings % 100000 == 1) {
            printf("*");
            fflush(stdout);
        }
        nstrings++;
    }
    printf("\nAdded %zu strings to the hash table.\n", nstrings);

    char matchfile[128] = {};
    sprintf(matchfile, "%s_string_matches.txt", prefix);
    mem_report = fopen(matchfile, "w");
    if(!mem_report) {
        printf("Couldn't write report:\n");
        perror("fopen");
        return false;
    }

    // Enable memory logging
    panda_enable_memcb();

    pcb.virt_mem_write = mem_write_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_WRITE, pcb);
    pcb.virt_mem_read = mem_read_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_READ, pcb);


    return true;
}

int print_fn(const char *lf, void *v) {
    fprintf(mem_report, "%s\n", lf);
    return 1;
}

void uninit_plugin(void *self) {
    critbit0_allprefixed(&t, "", print_fn, NULL);
    fclose(mem_report);
}