/*
 * QEMU Board board support
 *
 * Copyright (c) 2006 Aurelien Jarno
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/char/serial.h"
#include "hw/block/fdc.h"
#include "net/net.h"
#include "hw/boards.h"
#include "hw/i2c/smbus.h"
#include "block/block.h"
#include "hw/block/flash.h"
#include "hw/riscv/riscv.h"
#include "hw/riscv/cpudevs.h"
#include "hw/pci/pci.h"
#include "sysemu/char.h"
#include "sysemu/sysemu.h"
#include "sysemu/arch_init.h"
#include "qemu/log.h"
#include "hw/riscv/bios.h"
#include "hw/ide.h"
#include "hw/loader.h"
#include "elf.h"
#include "hw/timer/mc146818rtc.h"
#include "hw/timer/i8254.h"
#include "sysemu/blockdev.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"             /* SysBusDevice */
#include "qemu/host-utils.h"
#include "sysemu/qtest.h"
#include "qemu/error-report.h"
#include "hw/empty_slot.h"

//#define DEBUG_BOARD_INIT

#define ENVP_ADDR		0x80002000l
#define ENVP_NB_ENTRIES	 	16
#define ENVP_ENTRY_SIZE	 	256

/* Hardware addresses */
#define FLASH_ADDRESS 0x1e000000ULL
#define RESET_ADDRESS 0x10000ULL

#define FLASH_SIZE    0x400000

#define MAX_IDE_BUS 2

typedef struct {
    MemoryRegion iomem;
    MemoryRegion iomem_lo; /* 0 - 0x900 */
    MemoryRegion iomem_hi; /* 0xa00 - 0x100000 */
    uint32_t leds;
    uint32_t brk;
    uint32_t gpout;
    uint32_t i2cin;
    uint32_t i2coe;
    uint32_t i2cout;
    uint32_t i2csel;
    CharDriverState *display;
    char display_text[9];
    SerialState *uart;
} BoardFPGAState;

#define TYPE_RISCV_BOARD "riscv-board"
#define RISCV_BOARD(obj) OBJECT_CHECK(BoardState, (obj), TYPE_RISCV_BOARD)

typedef struct {
    SysBusDevice parent_obj;

    qemu_irq *i8259;
} BoardState;

static ISADevice *pit;

static struct _loaderparams {
    int ram_size;
    const char *kernel_filename;
    const char *kernel_cmdline;
    const char *initrd_filename;
} loaderparams;

/* Network support */
static void network_init(PCIBus *pci_bus)
{
    int i;

    for(i = 0; i < nb_nics; i++) {
        NICInfo *nd = &nd_table[i];
        const char *default_devaddr = NULL;

        if (i == 0 && (!nd->model || strcmp(nd->model, "pcnet") == 0))
            /* The board board has a PCNet card using PCI SLOT 11 */
            default_devaddr = "0b";

        pci_nic_init_nofail(nd, pci_bus, "pcnet", default_devaddr);
    }
}

/* ROM and pseudo bootloader

   The following code implements a very very simple bootloader. It first
   loads the registers a0 to a3 to the values expected by the OS, and
   then jump at the kernel address.

   The bootloader should pass the locations of the kernel arguments and
   environment variables tables. Those tables contain the 32-bit address
   of NULL terminated strings. The environment variables table should be
   terminated by a NULL address.

   For a simpler implementation, the number of kernel arguments is fixed
   to two (the name of the kernel and the command line), and the two
   tables are actually the same one.

   The registers a0 to a3 should contain the following values:
     a0 - number of kernel arguments
     a1 - 32-bit address of the kernel arguments table
     a2 - 32-bit address of the environment variables table
     a3 - RAM size in bytes
*/

static void write_bootloader (CPURISCVState *env, uint8_t *base,
                              int64_t kernel_entry)
{
    uint32_t *p;

    /* Small bootloader */
    p = (uint32_t *)base;

    // risc bootloader here
    stl_raw(p++, 0x000100b7); /* lui ra, 0xbfc00 #TODO: probably want to tweak this to be pc relative */
    stl_raw(p++, 0x5800809b); /* addiw ra,ra,1408 */
    stl_raw(p++, 0x00008067); /* jr ra */

    // end risc bootloader

    /* YAMON service vector */
    stl_raw(base + 0x500, 0xbfc00580);      /* start: */
    stl_raw(base + 0x504, 0xbfc0083c);      /* print_count: */
    stl_raw(base + 0x520, 0xbfc00580);      /* start: */
    stl_raw(base + 0x52c, 0xbfc00800);      /* flush_cache: */
    stl_raw(base + 0x534, 0xbfc00808);      /* print: */
    stl_raw(base + 0x538, 0xbfc00800);      /* reg_cpu_isr: */
    stl_raw(base + 0x53c, 0xbfc00800);      /* unred_cpu_isr: */
    stl_raw(base + 0x540, 0xbfc00800);      /* reg_ic_isr: */
    stl_raw(base + 0x544, 0xbfc00800);      /* unred_ic_isr: */
    stl_raw(base + 0x548, 0xbfc00800);      /* reg_esr: */
    stl_raw(base + 0x54c, 0xbfc00800);      /* unreg_esr: */
    stl_raw(base + 0x550, 0xbfc00800);      /* getchar: */
    stl_raw(base + 0x554, 0xbfc00800);      /* syscon_read: */


    /* Second part of the bootloader */
    p = (uint32_t *) (base + 0x580);

    // initialize status reg. this doesn't really belong in the bootloader:
    // TODO: move somewhere else 
    stl_raw(p++, 0x07100d13); // li t0, 0x71 // VM=0, S64=1, U64=1, EF=1, PEI=0, EI=0, PS=0, S=1
    stl_raw(p++, 0x50ad1073); // csrw status,t0 

    // store memamt to 0 as a 32 bit quantity in MiB
    stl_raw(p++, 0x000000b7 | ((loaderparams.ram_size >> 20) & 0xFFFFF000));    /* lui ra, hi20(ram_size) */
    stl_raw(p++, 0x0000809b | (((loaderparams.ram_size >> 20) & 0xFFF) << 20)); /* addiw ra,ra,low12(ram_size) */
    stl_raw(p++, 0x00102023);    /* sw rs2, 0(zero) */


    stl_raw(p++, 0x000000b7 | (kernel_entry & 0xFFFFF000));    /* lui ra, hi20(kernel_entry) */
    stl_raw(p++, 0x0000809b | ((kernel_entry & 0xFFF) << 20)); /* addiw ra,ra,low12(kernel_entry) */
    stl_raw(p++, 0x00008067);                                  /* jr ra */



    stl_raw(p++, 0x24040002);                                      /* addiu a0, zero, 2 */
    stl_raw(p++, 0x3c1d0000 | (((ENVP_ADDR - 64) >> 16) & 0xffff)); /* lui sp, high(ENVP_ADDR) */
    stl_raw(p++, 0x37bd0000 | ((ENVP_ADDR - 64) & 0xffff));        /* ori sp, sp, low(ENVP_ADDR) */
    stl_raw(p++, 0x3c050000 | ((ENVP_ADDR >> 16) & 0xffff));       /* lui a1, high(ENVP_ADDR) */
    stl_raw(p++, 0x34a50000 | (ENVP_ADDR & 0xffff));               /* ori a1, a1, low(ENVP_ADDR) */
    stl_raw(p++, 0x3c060000 | (((ENVP_ADDR + 8) >> 16) & 0xffff)); /* lui a2, high(ENVP_ADDR + 8) */
    stl_raw(p++, 0x34c60000 | ((ENVP_ADDR + 8) & 0xffff));         /* ori a2, a2, low(ENVP_ADDR + 8) */
    stl_raw(p++, 0x3c070000 | (loaderparams.ram_size >> 16));     /* lui a3, high(ram_size) */
    stl_raw(p++, 0x34e70000 | (loaderparams.ram_size & 0xffff));  /* ori a3, a3, low(ram_size) */

    /* Load BAR registers as done by YAMON */
    stl_raw(p++, 0x3c09b400);                                      /* lui t1, 0xb400 */

#ifdef TARGET_WORDS_BIGENDIAN
    stl_raw(p++, 0x3c08df00);                                      /* lui t0, 0xdf00 */
#else
    stl_raw(p++, 0x340800df);                                      /* ori t0, r0, 0x00df */
#endif
    stl_raw(p++, 0xad280068);                                      /* sw t0, 0x0068(t1) */

    stl_raw(p++, 0x3c09bbe0);                                      /* lui t1, 0xbbe0 */

#ifdef TARGET_WORDS_BIGENDIAN
    stl_raw(p++, 0x3c08c000);                                      /* lui t0, 0xc000 */
#else
    stl_raw(p++, 0x340800c0);                                      /* ori t0, r0, 0x00c0 */
#endif
    stl_raw(p++, 0xad280048);                                      /* sw t0, 0x0048(t1) */
#ifdef TARGET_WORDS_BIGENDIAN
    stl_raw(p++, 0x3c084000);                                      /* lui t0, 0x4000 */
#else
    stl_raw(p++, 0x34080040);                                      /* ori t0, r0, 0x0040 */
#endif
    stl_raw(p++, 0xad280050);                                      /* sw t0, 0x0050(t1) */

#ifdef TARGET_WORDS_BIGENDIAN
    stl_raw(p++, 0x3c088000);                                      /* lui t0, 0x8000 */
#else
    stl_raw(p++, 0x34080080);                                      /* ori t0, r0, 0x0080 */
#endif
    stl_raw(p++, 0xad280058);                                      /* sw t0, 0x0058(t1) */
#ifdef TARGET_WORDS_BIGENDIAN
    stl_raw(p++, 0x3c083f00);                                      /* lui t0, 0x3f00 */
#else
    stl_raw(p++, 0x3408003f);                                      /* ori t0, r0, 0x003f */
#endif
    stl_raw(p++, 0xad280060);                                      /* sw t0, 0x0060(t1) */

#ifdef TARGET_WORDS_BIGENDIAN
    stl_raw(p++, 0x3c08c100);                                      /* lui t0, 0xc100 */
#else
    stl_raw(p++, 0x340800c1);                                      /* ori t0, r0, 0x00c1 */
#endif
    stl_raw(p++, 0xad280080);                                      /* sw t0, 0x0080(t1) */
#ifdef TARGET_WORDS_BIGENDIAN
    stl_raw(p++, 0x3c085e00);                                      /* lui t0, 0x5e00 */
#else
    stl_raw(p++, 0x3408005e);                                      /* ori t0, r0, 0x005e */
#endif
    stl_raw(p++, 0xad280088);                                      /* sw t0, 0x0088(t1) */

    /* Jump to kernel code */
    stl_raw(p++, 0x3c1f0000 | ((kernel_entry >> 16) & 0xffff));    /* lui ra, high(kernel_entry) */
    stl_raw(p++, 0x37ff0000 | (kernel_entry & 0xffff));            /* ori ra, ra, low(kernel_entry) */
    stl_raw(p++, 0x03e00008);                                      /* jr ra */
    stl_raw(p++, 0x00000000);                                      /* nop */

    /* YAMON subroutines */
    p = (uint32_t *) (base + 0x800);
    stl_raw(p++, 0x03e00008);                                     /* jr ra */
    stl_raw(p++, 0x24020000);                                     /* li v0,0 */
   /* 808 YAMON print */
    stl_raw(p++, 0x03e06821);                                     /* move t5,ra */
    stl_raw(p++, 0x00805821);                                     /* move t3,a0 */
    stl_raw(p++, 0x00a05021);                                     /* move t2,a1 */
    stl_raw(p++, 0x91440000);                                     /* lbu a0,0(t2) */
    stl_raw(p++, 0x254a0001);                                     /* addiu t2,t2,1 */
    stl_raw(p++, 0x10800005);                                     /* beqz a0,834 */
    stl_raw(p++, 0x00000000);                                     /* nop */
    stl_raw(p++, 0x0ff0021c);                                     /* jal 870 */
    stl_raw(p++, 0x00000000);                                     /* nop */
    stl_raw(p++, 0x08000205);                                     /* j 814 */
    stl_raw(p++, 0x00000000);                                     /* nop */
    stl_raw(p++, 0x01a00008);                                     /* jr t5 */
    stl_raw(p++, 0x01602021);                                     /* move a0,t3 */
    /* 0x83c YAMON print_count */
    stl_raw(p++, 0x03e06821);                                     /* move t5,ra */
    stl_raw(p++, 0x00805821);                                     /* move t3,a0 */
    stl_raw(p++, 0x00a05021);                                     /* move t2,a1 */
    stl_raw(p++, 0x00c06021);                                     /* move t4,a2 */
    stl_raw(p++, 0x91440000);                                     /* lbu a0,0(t2) */
    stl_raw(p++, 0x0ff0021c);                                     /* jal 870 */
    stl_raw(p++, 0x00000000);                                     /* nop */
    stl_raw(p++, 0x254a0001);                                     /* addiu t2,t2,1 */
    stl_raw(p++, 0x258cffff);                                     /* addiu t4,t4,-1 */
    stl_raw(p++, 0x1580fffa);                                     /* bnez t4,84c */
    stl_raw(p++, 0x00000000);                                     /* nop */
    stl_raw(p++, 0x01a00008);                                     /* jr t5 */
    stl_raw(p++, 0x01602021);                                     /* move a0,t3 */
    /* 0x870 */
    stl_raw(p++, 0x3c08b800);                                     /* lui t0,0xb400 */
    stl_raw(p++, 0x350803f8);                                     /* ori t0,t0,0x3f8 */
    stl_raw(p++, 0x91090005);                                     /* lbu t1,5(t0) */
    stl_raw(p++, 0x00000000);                                     /* nop */
    stl_raw(p++, 0x31290040);                                     /* andi t1,t1,0x40 */
    stl_raw(p++, 0x1120fffc);                                     /* beqz t1,878 <outch+0x8> */
    stl_raw(p++, 0x00000000);                                     /* nop */
    stl_raw(p++, 0x03e00008);                                     /* jr ra */
    stl_raw(p++, 0xa1040000);                                     /* sb a0,0(t0) */

}

static void GCC_FMT_ATTR(3, 4) prom_set(uint32_t* prom_buf, int index,
                                        const char *string, ...)
{
    va_list ap;
    int32_t table_addr;

    if (index >= ENVP_NB_ENTRIES)
        return;

    if (string == NULL) {
        prom_buf[index] = 0;
        return;
    }

    table_addr = sizeof(int32_t) * ENVP_NB_ENTRIES + index * ENVP_ENTRY_SIZE;
    prom_buf[index] = tswap32(ENVP_ADDR + table_addr);

    va_start(ap, string);
    vsnprintf((char *)prom_buf + table_addr, ENVP_ENTRY_SIZE, string, ap);
    va_end(ap);
}

/* Kernel */
static int64_t load_kernel (void)
{
    int64_t kernel_entry, kernel_high;
    long initrd_size;
    ram_addr_t initrd_offset;
    int big_endian;
    uint32_t *prom_buf;
    long prom_size;
    int prom_index = 0;

    big_endian = 0;

    if (load_elf(loaderparams.kernel_filename, cpu_riscv_kseg0_to_phys, NULL,
                 (uint64_t *)&kernel_entry, NULL, (uint64_t *)&kernel_high,
                 big_endian, ELF_MACHINE, 1) < 0) {
        fprintf(stderr, "qemu: could not load kernel '%s'\n",
                loaderparams.kernel_filename);
        exit(1);
    }

    /* load initrd */
    initrd_size = 0;
    initrd_offset = 0;
    if (loaderparams.initrd_filename) {
        initrd_size = get_image_size (loaderparams.initrd_filename);
        if (initrd_size > 0) {
            initrd_offset = (kernel_high + ~INITRD_PAGE_MASK) & INITRD_PAGE_MASK;
            if (initrd_offset + initrd_size > ram_size) {
                fprintf(stderr,
                        "qemu: memory too small for initial ram disk '%s'\n",
                        loaderparams.initrd_filename);
                exit(1);
            }
            initrd_size = load_image_targphys(loaderparams.initrd_filename,
                                              initrd_offset,
                                              ram_size - initrd_offset);
        }
        if (initrd_size == (target_ulong) -1) {
            fprintf(stderr, "qemu: could not load initial ram disk '%s'\n",
                    loaderparams.initrd_filename);
            exit(1);
        }
    }

    /* Setup prom parameters. */
    prom_size = ENVP_NB_ENTRIES * (sizeof(int32_t) + ENVP_ENTRY_SIZE);
    prom_buf = g_malloc(prom_size);

    prom_set(prom_buf, prom_index++, "%s", loaderparams.kernel_filename);
    if (initrd_size > 0) {
        prom_set(prom_buf, prom_index++, "rd_start=0x%" PRIx64 " rd_size=%li %s",
                 cpu_riscv_phys_to_kseg0(NULL, initrd_offset), initrd_size,
                 loaderparams.kernel_cmdline);
    } else {
        prom_set(prom_buf, prom_index++, "%s", loaderparams.kernel_cmdline);
    }

    prom_set(prom_buf, prom_index++, "memsize");
    prom_set(prom_buf, prom_index++, "%i",
             MIN(loaderparams.ram_size, 256 << 20));
    prom_set(prom_buf, prom_index++, "modetty0");
    prom_set(prom_buf, prom_index++, "38400n8r");
    prom_set(prom_buf, prom_index++, NULL);

    rom_add_blob_fixed("prom", prom_buf, prom_size,
                       cpu_riscv_kseg0_to_phys(NULL, ENVP_ADDR));

    return kernel_entry;
}

static void main_cpu_reset(void *opaque)
{
    RISCVCPU *cpu = opaque;
    cpu_reset(CPU(cpu));
}

static void cpu_request_exit(void *opaque, int irq, int level)
{
    CPUState *cpu = current_cpu;

    if (cpu && level) {
        cpu_exit(cpu);
    }
}

static
void riscv_board_init(QEMUMachineInitArgs *args)
{
    ram_addr_t ram_size = args->ram_size;
    const char *cpu_model = args->cpu_model;
    const char *kernel_filename = args->kernel_filename;
    const char *kernel_cmdline = args->kernel_cmdline;
    const char *initrd_filename = args->initrd_filename;
//    char *filename;
    pflash_t *fl;
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *ram_high = g_new(MemoryRegion, 1);
//    MemoryRegion *ram_low_preio = g_new(MemoryRegion, 1);
//    MemoryRegion *ram_low_postio;
    MemoryRegion *bios, *bios_copy = g_new(MemoryRegion, 1);
    target_long bios_size = FLASH_SIZE;
    const size_t smbus_eeprom_size = 8 * 256;
    uint8_t *smbus_eeprom_buf = g_malloc0(smbus_eeprom_size);
    int64_t kernel_entry;
    PCIBus *pci_bus;
    ISABus *isa_bus;
    RISCVCPU *cpu;
    CPURISCVState *env;
    qemu_irq *isa_irq;
    qemu_irq *cpu_exit_irq;
    int piix4_devfn;
    I2CBus *smbus;
    int i;
    DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    DriveInfo *fd[MAX_FD];
    int fl_idx = 0;
    int fl_sectors = bios_size >> 16;
    int be;

    DeviceState *dev = qdev_create(NULL, TYPE_RISCV_BOARD);
    BoardState *s = RISCV_BOARD(dev);

    /* The whole address space decoded by the GT-64120A doesn't generate
       exception when accessing invalid memory. Create an empty slot to
       emulate this feature. */
//    empty_slot_init(0, 0x20000000);

    qdev_init_nofail(dev);

    /* Make sure the first 3 serial ports are associated with a device. */
    for(i = 0; i < 3; i++) {
        if (!serial_hds[i]) {
            char label[32];
            snprintf(label, sizeof(label), "serial%d", i);
            serial_hds[i] = qemu_chr_new(label, "null", NULL);
        }
    }

    /* init CPUs */
    if (cpu_model == NULL) {
        cpu_model = "riscv-generic";
    }

    for (i = 0; i < smp_cpus; i++) {
        cpu = cpu_riscv_init(cpu_model);
        if (cpu == NULL) {
            fprintf(stderr, "Unable to find CPU definition\n");
            exit(1);
        }
        env = &cpu->env;

        /* Init internal devices */
        cpu_riscv_irq_init_cpu(env);
        cpu_riscv_clock_init(env);
        qemu_register_reset(main_cpu_reset, cpu);
    }
    cpu = RISCV_CPU(first_cpu);
    env = &cpu->env;

    /* allocate RAM */
    if (ram_size > (2048u << 20)) {
        fprintf(stderr,
                "qemu: Too much memory for this machine: %d MB, maximum 2048 MB\n",
                ((unsigned int)ram_size / (1 << 20)));
        exit(1);
    }

    /* register RAM at high address where it is undisturbed by IO */
    memory_region_init_ram(ram_high, NULL, "riscv_board.ram", ram_size);
    vmstate_register_ram_global(ram_high);
    memory_region_add_subregion(system_memory, 0x0, ram_high);

    /* RISCV is little endian by spec */
    be = 0;

    fl = pflash_cfi01_register(FLASH_ADDRESS, NULL, "riscv_board.bios",
                               BIOS_SIZE, NULL,
                               65536, fl_sectors,
                               4, 0x0000, 0x0000, 0x0000, 0x0000, be);
    bios = pflash_cfi01_get_memory(fl);
    fl_idx++;
    if (kernel_filename) {
        /* Write a small bootloader to the flash location. */
        loaderparams.ram_size = MIN(ram_size, 256 << 20);
        loaderparams.kernel_filename = kernel_filename;
        loaderparams.kernel_cmdline = kernel_cmdline;
        loaderparams.initrd_filename = initrd_filename;
        kernel_entry = load_kernel();
        write_bootloader(env, memory_region_get_ram_ptr(bios), kernel_entry);
    } 


    serial_mm_init(system_memory, 0x3f8, 0, env->irq[4], 1843200/16, serial_hds[0], 
            DEVICE_NATIVE_ENDIAN);
/*    serial_mm_init(system_memory, 0x2f8, 0, NULL, 1843200/16, serial_hds[1], 
            DEVICE_NATIVE_ENDIAN);
    serial_mm_init(system_memory, 0x3e8, 0, NULL, 1843200/16, serial_hds[2], 
            DEVICE_NATIVE_ENDIAN);*/


    /*
     * Map the BIOS at a 2nd physical location, as on the real board.
     * Copy it so that we can patch in the RISCV revision, which cannot be
     * handled by an overlapping region as the resulting ROM code subpage
     * regions are not executable.
     */
    memory_region_init_ram(bios_copy, NULL, "bios.1fc", BIOS_SIZE);
    if (!rom_copy(memory_region_get_ram_ptr(bios_copy),
                  FLASH_ADDRESS, BIOS_SIZE)) {
        memcpy(memory_region_get_ram_ptr(bios_copy),
               memory_region_get_ram_ptr(bios), BIOS_SIZE);
    }

    // TODO: want to actually make this read only, but we mess with the linux
    // gp this way. eventually reduce bios size to prevent this overlap/accidental
    // write-protection of the gp
//    memory_region_set_readonly(bios_copy, true);
    memory_region_add_subregion(system_memory, RESET_ADDRESS, bios_copy);

    /* Board ID = 0x420 (Board Board with CoreLV) */
    stl_p(memory_region_get_ram_ptr(bios_copy) + 0x10, 0x00000420);

    /* Init internal devices */
    cpu_riscv_irq_init_cpu(env);
    cpu_riscv_clock_init(env);

    /*
     * We have a circular dependency problem: pci_bus depends on isa_irq,
     * isa_irq is provided by i8259, i8259 depends on ISA, ISA depends
     * on piix4, and piix4 depends on pci_bus.  To stop the cycle we have
     * qemu_irq_proxy() adds an extra bit of indirection, allowing us
     * to resolve the isa_irq -> i8259 dependency after i8259 is initialized.
     */
    isa_irq = qemu_irq_proxy(&s->i8259, 16);

    /* Northbridge */
    pci_bus = gt64120_register(isa_irq);

    /* Southbridge */
    ide_drive_get(hd, MAX_IDE_BUS);

    piix4_devfn = piix4_init(pci_bus, &isa_bus, 80);

    /* Interrupt controller */
    /* The 8259 is attached to the RISCV CPU INT0 pin, ie interrupt 2 */
    s->i8259 = i8259_init(isa_bus, env->irq[2]);

    isa_bus_irqs(isa_bus, s->i8259);
    pci_piix4_ide_init(pci_bus, hd, piix4_devfn + 1);
    pci_create_simple(pci_bus, piix4_devfn + 2, "piix4-usb-uhci");
    smbus = piix4_pm_init(pci_bus, piix4_devfn + 3, 0x1100,
                          isa_get_irq(NULL, 9), NULL, 0, NULL);
    smbus_eeprom_init(smbus, 8, smbus_eeprom_buf, smbus_eeprom_size);
    g_free(smbus_eeprom_buf);
    pit = pit_init(isa_bus, 0x40, 0, NULL);
    cpu_exit_irq = qemu_allocate_irqs(cpu_request_exit, NULL, 1);
    DMA_init(0, cpu_exit_irq);

    /* Super I/O */
    isa_create_simple(isa_bus, "i8042");

    rtc_init(isa_bus, 2000, NULL);
//    serial_isa_init(isa_bus, 0, serial_hds[0]);
//    serial_isa_init(isa_bus, 1, serial_hds[1]);
    if (parallel_hds[0])
        parallel_init(isa_bus, 0, parallel_hds[0]);
    for(i = 0; i < MAX_FD; i++) {
        fd[i] = drive_get(IF_FLOPPY, 0, i);
    }
    fdctrl_init_isa(isa_bus, fd);

    /* Network card */
    network_init(pci_bus);

    /* Optional PCI video card */
    pci_vga_init(pci_bus);
}

static int riscv_board_sysbus_device_init(SysBusDevice *sysbusdev)
{
    return 0;
}

static void riscv_board_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = riscv_board_sysbus_device_init;
}

static const TypeInfo riscv_board_device = {
    .name          = TYPE_RISCV_BOARD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BoardState),
    .class_init    = riscv_board_class_init,
};

static QEMUMachine riscv_board_machine = {
    .name = "board",
    .desc = "RISCV Board",
    .init = riscv_board_init,
    .max_cpus = 16,
    .is_default = 1,
};

static void riscv_board_register_types(void)
{
    type_register_static(&riscv_board_device);
}

static void riscv_board_machine_init(void)
{
    qemu_register_machine(&riscv_board_machine);
}

type_init(riscv_board_register_types)
machine_init(riscv_board_machine_init);