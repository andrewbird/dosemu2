/*
 *  Copyright (C) 2023  stsp
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <djdev64/djdev64.h>
#include <djdev64/dj64init.h>
#include "init.h"
#include "emu.h"
#include "dosemu_debug.h"
#include "utilities.h"
#include "emudpmi.h"
#include "dos2linux.h"

static uint8_t *dj64_addr2ptr(uint32_t addr)
{
    return dosaddr_to_unixaddr(addr);
}

static uint32_t dj64_ptr2addr(uint8_t *ptr)
{
    if (ptr >= MEM_BASE32(config.dpmi_base) &&
            ptr < MEM_BASE32(config.dpmi_base) + dpmi_mem_size())
        return DOSADDR_REL(ptr);
    dosemu_error("bad ptr2addr %p\n", ptr);
    return -1;
}

static void dj64_print(int prio, const char *format, va_list ap)
{
    switch(prio) {
    case DJ64_PRINT_TERMINAL:
        vfprintf(stderr, format, ap);
        break;
    case DJ64_PRINT_LOG:
        if (debug_level('M')) {
            log_printf(-1, "dj64: ");
            vlog_printf(-1, format, ap);
        }
        break;
    }
}

static int dj64_asm_call(dpmi_regs *regs, uint32_t offs, uint8_t *sp,
        uint8_t len)
{
    /* TODO! */
    error("asm call to 0x%x\n", offs);
    return ASM_CALL_OK;
}

const struct dj64_api api = {
    .addr2ptr = dj64_addr2ptr,
    .ptr2addr = dj64_ptr2addr,
    .print = dj64_print,
    .asm_call = dj64_asm_call,
};

#if DJ64_API_VER != 1
#error wrong dj64 version
#endif

static int do_open(const char *path)
{
    return djdev64_open(path, &api, DJ64_API_VER);
}

static const struct djdev64_ops ops = {
    do_open,
    djdev64_call,
    djdev64_ctrl,
    djdev64_close,
};

CONSTRUCTOR(static void djdev64_init(void))
{
    register_djdev64(&ops);
}
