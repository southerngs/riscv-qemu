/*
 * QEMU RISCV Host Target Interface (HTIF) Emulation
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


#include "hw/riscv/htif/htif.h"
#include "qemu/timer.h"
#include "exec/address-spaces.h"
//#include "exec/memory.h"
#include "qemu/error-report.h"
#include <fcntl.h>
#include <sys/stat.h>

static void htif_pre_save(void *opaque)
{
    return;
}

static int htif_post_load(void *opaque, int version_id)
{
//    HTIFState *s = opaque;
    return 0;
}

const VMStateDescription vmstate_htif = {
    .name = "htif",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = htif_pre_save,
    .post_load = htif_post_load,
    .fields      = (VMStateField []) { // TODO what
        VMSTATE_UINT64(tohost, HTIFState),
        VMSTATE_UINT64(fromhost, HTIFState),
        VMSTATE_UINT64(tohost_addr, HTIFState),
        VMSTATE_UINT64(fromhost_addr, HTIFState),
        VMSTATE_END_OF_LIST()
    },
};

static void dma_strcopy(HTIFState *htifstate, char *str, hwaddr phys_addr) {
    int i = 0;
    printf("writing %s to mem\n", str);
    while(*(str+i)) {
        stb_p((void*)(memory_region_get_ram_ptr(htifstate->main_mem)+phys_addr+i), *(str+i));
        i++;
    }
    stb_p((void*)(memory_region_get_ram_ptr(htifstate->main_mem)+phys_addr+i), 0);
}


static void htif_handle_tohost_write(HTIFState *htifstate, uint64_t val_written) {
//    printf("cpu wrote to tohost: %016lx\n", val_written);

    uint8_t device = val_written >> 56;
    uint8_t cmd = val_written >> 48;
    uint64_t payload = val_written & 0xFFFFFFFFFFFFULL;

    uint64_t addr = payload >> 8;
    hwaddr real_addr = (hwaddr)addr;
    uint8_t what = payload & 0xFF;


    if (device == 0x1) { // assume device 0x0 is permanently hooked to block dev for now
//        printf("handling: device 0x%x, cmd 0x%x, addr 0x%016lx, what 0x%x\n",
//                device, cmd, addr, what);
        if (cmd == 0xFF) { 
            if (what == 0xFF) { // register
                dma_strcopy(htifstate, htifstate->real_name, real_addr);
            } else if (what == 0x0) {
                dma_strcopy(htifstate, (char*)"read", real_addr);
            } else if (what == 0x1) {
                dma_strcopy(htifstate, (char*)"write", real_addr);
            } else {
                dma_strcopy(htifstate, (char*)"", real_addr);
            }
        }
    } else if (cmd == 0xFF && what == 0xFF) { // all other devices
        stb_p((void*)(memory_region_get_ram_ptr(htifstate->main_mem)+real_addr), 0);
    }
    htifstate->tohost = 0; // clear to indicate we read
    htifstate->fromhost = 0x1; // write to indicate device name placed
}


/*
static void serial_reset(void *opaque)
{
    HTIFState *htifstate = opaque;

    htifstate->tohost = 0;
    htifstate->fromhost = 0;
    htifstate->tohost_addr = 0;
    htifstate->fromhost_addr = 0;
    
    qemu_irq_lower(htifstate->irq);
}
*/
/* Memory mapped interface */

// CPU wants to read an HTIF register
static uint64_t htif_mm_read(void *opaque, hwaddr addr, unsigned size)
{
    HTIFState *htifstate = opaque;



    if (addr == 0x0) {
        return htifstate->tohost & 0xFFFFFFFF;
    } else if (addr == 0x4) {
        return (htifstate->tohost >> 32) & 0xFFFFFFFF;
    } else if (addr == 0x8) {
        return htifstate->fromhost & 0xFFFFFFFF;
    } else if (addr == 0xc) {
        return (htifstate->fromhost >> 32) & 0xFFFFFFFF;
    } else {
        printf("Invalid htif register address %016lx\n", (uint64_t)addr);
        exit(1);
    }
}

// CPU wrote to an HTIF register
static void htif_mm_write(void *opaque, hwaddr addr,
                            uint64_t value, unsigned size)
{
    HTIFState *htifstate = opaque;
    if (addr == 0x0) {
        htifstate->tohost = value & 0xFFFFFFFF;// << 32);
    } else if (addr == 0x4) {
        htif_handle_tohost_write(htifstate, htifstate->tohost | (value << 32));
    } else if (addr == 0x8) {
        htifstate->fromhost = value & 0xFFFFFFFF;
    } else if (addr == 0xc) {
        htifstate->fromhost |= value << 32;
//        printf("cpu wrote to fromhost: %016lu\n", htifstate->fromhost);
    } else {
        printf("Invalid htif register address %016lx\n", (uint64_t)addr);
        exit(1);
    }
}

static const MemoryRegionOps htif_mm_ops[3] = {
    [DEVICE_LITTLE_ENDIAN] = {
        .read = htif_mm_read,
        .write = htif_mm_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
    },
};

HTIFState *htif_mm_init(MemoryRegion *address_space, hwaddr base, qemu_irq irq, 
                        MemoryRegion *main_mem)
{
    HTIFState *htifstate;
    size_t size;
    char *rname;
    char size_str_buf[400];

    htifstate = g_malloc0(sizeof(HTIFState));
    rname = g_malloc0(sizeof(char)*500);
    htifstate->tohost = 0;
    htifstate->fromhost = 0;
    htifstate->tohost_addr = base;
    htifstate->fromhost_addr = base + 0x8;
    htifstate->irq = irq;
    htifstate->address_space = address_space;
    htifstate->main_mem = main_mem;

    vmstate_register(NULL, base, &vmstate_htif, htifstate);

    printf("%016lx\n", base);
    memory_region_init_io(&htifstate->io, NULL, &htif_mm_ops[DEVICE_LITTLE_ENDIAN], 
            htifstate, "htif", 16 /* 2 64-bit registers */);
    memory_region_add_subregion(address_space, base, &htifstate->io);

    /* TODO: FOR NOW, we're going to hardcode a filename to test */
    htifstate->block_fname = "disk.img"; 
    htifstate->block_fd = open(htifstate->block_fname, O_RDWR);

    struct stat st;
    if (fstat(htifstate->block_fd, &st) < 0) {
        printf("BAD FILE\n");
        exit(1);
    }
    size = st.st_size;
    strcpy(rname, "disk size=");
    snprintf(size_str_buf, sizeof(size_str_buf), "%zu", size);
    strcat(rname, size_str_buf);
    htifstate->real_name = rname;

    return htifstate;
}
