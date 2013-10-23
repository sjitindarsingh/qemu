/*
 * ARM mach-virt emulation
 *
 * Copyright (c) 2013 Linaro
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Emulate a virtual board which works by passing Linux all the information
 * it needs about what devices are present via the device tree.
 * There are some restrictions about what we can do here:
 *  + we can only present devices whose Linux drivers will work based
 *    purely on the device tree with no platform data at all
 *  + we want to present a very stripped-down minimalist platform,
 *    both because this reduces the security attack surface from the guest
 *    and also because it reduces our exposure to being broken when
 *    the kernel updates its device tree bindings and requires further
 *    information in a device binding that we aren't providing.
 * This is essentially the same approach kvmtool uses.
 */

#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "hw/arm/primecell.h"
#include "hw/devices.h"
#include "net/net.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "hw/boards.h"
#include "exec/address-spaces.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"

#define NUM_VIRTIO_TRANSPORTS 32

#define GIC_FDT_IRQ_TYPE_SPI 0
#define GIC_FDT_IRQ_TYPE_PPI 1

#define GIC_FDT_IRQ_FLAGS_EDGE_LO_HI 1
#define GIC_FDT_IRQ_FLAGS_EDGE_HI_LO 2
#define GIC_FDT_IRQ_FLAGS_LEVEL_HI 4
#define GIC_FDT_IRQ_FLAGS_LEVEL_LO 8

#define GIC_FDT_IRQ_PPI_CPU_START 8
#define GIC_FDT_IRQ_PPI_CPU_WIDTH 8

enum {
    VIRT_FLASH,
    VIRT_MEM,
    VIRT_CPUPERIPHS,
    VIRT_GIC_DIST,
    VIRT_GIC_CPU,
    VIRT_MMIO,
};

typedef struct MemMapEntry {
    hwaddr base;
    hwaddr size;
} MemMapEntry;

typedef struct VirtBoardInfo {
    struct arm_boot_info bootinfo;
    const char *cpu_model;
    const char *cpu_compatible;
    const char *qdevname;
    const char *gic_compatible;
    const MemMapEntry *memmap;
    int smp_cpus;
    void *fdt;
    int fdt_size;
} VirtBoardInfo;

/* Addresses and sizes of our components.
 * We leave the first 64K free for possible use later for
 * flash (for running boot code such as UEFI); following
 * that is I/O, and then everything else is RAM (which may
 * happily spill over into the high memory region beyond 4GB).
 */
static const MemMapEntry a15memmap[] = {
    [VIRT_FLASH] = { 0, 0x1000000 },
    [VIRT_CPUPERIPHS] = { 0x1000000, 0x8000 },
    /* GIC distributor and CPU interfaces sit inside the CPU peripheral space */
    [VIRT_GIC_DIST] = { 0x1001000, 0x1000 },
    [VIRT_GIC_CPU] = { 0x1002000, 0x1000 },
    [VIRT_MMIO] = { 0x1008000, 0x200 },
    /* ...repeating for a total of NUM_VIRTIO_TRANSPORTS, each of that size */
    [VIRT_MEM] = { 0x8000000, 30ULL * 1024 * 1024 * 1024 },
};

static VirtBoardInfo machines[] = {
    {
        .cpu_model = "cortex-a15",
        .cpu_compatible = "arm,cortex-a15",
        .qdevname = "a15mpcore_priv",
        .gic_compatible = "arm,cortex-a15-gic",
        .memmap = a15memmap,
    },
};

static VirtBoardInfo *find_machine_info(const char *cpu)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(machines); i++) {
        if (strcmp(cpu, machines[i].cpu_model) == 0) {
            return &machines[i];
        }
    }
    return NULL;
}

static void create_fdt(VirtBoardInfo *vbi)
{
    void *fdt = create_device_tree(&vbi->fdt_size);

    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }

    vbi->fdt = fdt;

    /* Header */
    qemu_devtree_setprop_string(fdt, "/", "compatible", "linux,dummy-virt");
    qemu_devtree_setprop_cell(fdt, "/", "#address-cells", 0x2);
    qemu_devtree_setprop_cell(fdt, "/", "#size-cells", 0x2);

    /*
     * /chosen and /memory nodes must exist for load_dtb
     * to fill in necessary properties later
     */
    qemu_devtree_add_subnode(fdt, "/chosen");
    qemu_devtree_add_subnode(fdt, "/memory");
    qemu_devtree_setprop_string(fdt, "/memory", "device_type", "memory");

    /* No PSCI for TCG yet */
#ifdef CONFIG_KVM
    if (kvm_enabled()) {
        qemu_devtree_add_subnode(fdt, "/psci");
        qemu_devtree_setprop_string(fdt, "/psci", "compatible", "arm,psci");
        qemu_devtree_setprop_string(fdt, "/psci", "method", "hvc");
        qemu_devtree_setprop_cell(fdt, "/psci", "cpu_suspend",
                                  KVM_PSCI_FN_CPU_SUSPEND);
        qemu_devtree_setprop_cell(fdt, "/psci", "cpu_off", KVM_PSCI_FN_CPU_OFF);
        qemu_devtree_setprop_cell(fdt, "/psci", "cpu_on", KVM_PSCI_FN_CPU_ON);
        qemu_devtree_setprop_cell(fdt, "/psci", "migrate", KVM_PSCI_FN_MIGRATE);
    }
#endif
}

static void fdt_add_timer_nodes(const VirtBoardInfo *vbi)
{
    /* Note that on A15 h/w these interrupts are level-triggered,
     * but for the GIC implementation provided by both QEMU and KVM
     * they are edge-triggered.
     */
    uint32_t irqflags = GIC_FDT_IRQ_FLAGS_EDGE_LO_HI;

    irqflags = deposit32(irqflags, GIC_FDT_IRQ_PPI_CPU_START,
                         GIC_FDT_IRQ_PPI_CPU_WIDTH, (1 << vbi->smp_cpus) - 1);

    qemu_devtree_add_subnode(vbi->fdt, "/timer");
    qemu_devtree_setprop_string(vbi->fdt, "/timer",
                                "compatible", "arm,armv7-timer");
    qemu_devtree_setprop_cells(vbi->fdt, "/timer", "interrupts",
                               GIC_FDT_IRQ_TYPE_PPI, 13, irqflags,
                               GIC_FDT_IRQ_TYPE_PPI, 14, irqflags,
                               GIC_FDT_IRQ_TYPE_PPI, 11, irqflags,
                               GIC_FDT_IRQ_TYPE_PPI, 10, irqflags);
}

static void fdt_add_cpu_nodes(const VirtBoardInfo *vbi)
{
    int cpu;

    qemu_devtree_add_subnode(vbi->fdt, "/cpus");
    qemu_devtree_setprop_cell(vbi->fdt, "/cpus", "#address-cells", 0x1);
    qemu_devtree_setprop_cell(vbi->fdt, "/cpus", "#size-cells", 0x0);

    for (cpu = 0; cpu < vbi->smp_cpus; cpu++) {
        char *nodename = g_strdup_printf("/cpus/cpu@%d", cpu);

        qemu_devtree_add_subnode(vbi->fdt, nodename);
        qemu_devtree_setprop_string(vbi->fdt, nodename, "device_type", "cpu");
        qemu_devtree_setprop_string(vbi->fdt, nodename, "compatible",
                                    vbi->cpu_compatible);

        if (vbi->smp_cpus > 1) {
            qemu_devtree_setprop_string(vbi->fdt, nodename,
                                        "enable-method", "psci");
        }

        qemu_devtree_setprop_cell(vbi->fdt, nodename, "reg", cpu);
        g_free(nodename);
    }
}

static void fdt_add_gic_node(const VirtBoardInfo *vbi)
{
    uint32_t gic_phandle;

    gic_phandle = qemu_devtree_alloc_phandle(vbi->fdt);
    qemu_devtree_setprop_cell(vbi->fdt, "/", "interrupt-parent", gic_phandle);

    qemu_devtree_add_subnode(vbi->fdt, "/intc");
    qemu_devtree_setprop_string(vbi->fdt, "/intc", "compatible",
                                vbi->gic_compatible);
    qemu_devtree_setprop_cell(vbi->fdt, "/intc", "#interrupt-cells", 3);
    qemu_devtree_setprop(vbi->fdt, "/intc", "interrupt-controller", NULL, 0);
    qemu_devtree_setprop_sized_cells(vbi->fdt, "/intc", "reg",
                                     2, vbi->memmap[VIRT_GIC_DIST].base,
                                     2, vbi->memmap[VIRT_GIC_DIST].size,
                                     2, vbi->memmap[VIRT_GIC_CPU].base,
                                     2, vbi->memmap[VIRT_GIC_CPU].size);
    qemu_devtree_setprop_cell(vbi->fdt, "/intc", "phandle", gic_phandle);
}

static void create_virtio_devices(const VirtBoardInfo *vbi, qemu_irq *pic)
{
    int i;
    hwaddr base = vbi->memmap[VIRT_MMIO].base;
    hwaddr size = vbi->memmap[VIRT_MMIO].size;

    for (i = 0; i < NUM_VIRTIO_TRANSPORTS; i++) {
        char *nodename;
        int irq = i + 16;

        sysbus_create_simple("virtio-mmio", base, pic[irq]);

        nodename = g_strdup_printf("/virtio_mmio@%" PRIx64, base);
        qemu_devtree_add_subnode(vbi->fdt, nodename);
        qemu_devtree_setprop_string(vbi->fdt, nodename,
                                    "compatible", "virtio,mmio");
        qemu_devtree_setprop_sized_cells(vbi->fdt, nodename, "reg",
                                         2, base, 2, size);
        qemu_devtree_setprop_cells(vbi->fdt, nodename, "interrupts",
                                   GIC_FDT_IRQ_TYPE_SPI, irq,
                                   GIC_FDT_IRQ_FLAGS_EDGE_LO_HI);
        g_free(nodename);
        base += size;
    }
}

static void *machvirt_dtb(const struct arm_boot_info *binfo, int *fdt_size)
{
    const VirtBoardInfo *board = (const VirtBoardInfo *)binfo;

    *fdt_size = board->fdt_size;
    return board->fdt;
}

static void machvirt_init(QEMUMachineInitArgs *args)
{
    qemu_irq pic[64];
    MemoryRegion *sysmem = get_system_memory();
    int n;
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    qemu_irq cpu_irq[4];
    DeviceState *dev;
    SysBusDevice *busdev;
    const char *cpu_model = args->cpu_model;
    VirtBoardInfo *vbi;

    if (!cpu_model) {
        cpu_model = "cortex-a15";
    }

    vbi = find_machine_info(cpu_model);

    if (!vbi) {
        error_report("mach-virt: CPU %s not supported", cpu_model);
        exit(1);
    }

    vbi->smp_cpus = smp_cpus;

    /*
     * Only supported method of starting secondary CPUs is PSCI and
     * PSCI is not yet supported with TCG, so limit smp_cpus to 1
     * if we're not using KVM.
     */
    if (!kvm_enabled() && smp_cpus > 1) {
        error_report("mach-virt: must enable KVM to use multiple CPUs");
        exit(1);
    }

    if (args->ram_size > vbi->memmap[VIRT_MEM].size) {
        error_report("mach-virt: cannot model more than 30GB RAM");
        exit(1);
    }

    create_fdt(vbi);
    fdt_add_timer_nodes(vbi);

    for (n = 0; n < smp_cpus; n++) {
        ARMCPU *cpu;
        qemu_irq *irqp;

        cpu = cpu_arm_init(cpu_model);
        irqp = arm_pic_init_cpu(cpu);
        cpu_irq[n] = irqp[ARM_PIC_CPU_IRQ];
    }
    fdt_add_cpu_nodes(vbi);

    memory_region_init_ram(ram, NULL, "mach-virt.ram", args->ram_size);
    vmstate_register_ram_global(ram);
    memory_region_add_subregion(sysmem, vbi->memmap[VIRT_MEM].base, ram);

    dev = qdev_create(NULL, vbi->qdevname);
    qdev_prop_set_uint32(dev, "num-cpu", smp_cpus);
    qdev_init_nofail(dev);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_mmio_map(busdev, 0, vbi->memmap[VIRT_CPUPERIPHS].base);
    fdt_add_gic_node(vbi);
    for (n = 0; n < smp_cpus; n++) {
        sysbus_connect_irq(busdev, n, cpu_irq[n]);
    }

    for (n = 0; n < 64; n++) {
        pic[n] = qdev_get_gpio_in(dev, n);
    }

    /* Create mmio transports, so the user can create virtio backends
     * (which will be automatically plugged in to the transports). If
     * no backend is created the transport will just sit harmlessly idle.
     */
    create_virtio_devices(vbi, pic);

    vbi->bootinfo.ram_size = args->ram_size;
    vbi->bootinfo.kernel_filename = args->kernel_filename;
    vbi->bootinfo.kernel_cmdline = args->kernel_cmdline;
    vbi->bootinfo.initrd_filename = args->initrd_filename;
    vbi->bootinfo.nb_cpus = smp_cpus;
    vbi->bootinfo.board_id = -1;
    vbi->bootinfo.loader_start = vbi->memmap[VIRT_MEM].base;
    vbi->bootinfo.get_dtb = machvirt_dtb;
    arm_load_kernel(ARM_CPU(first_cpu), &vbi->bootinfo);
}

static QEMUMachine machvirt_a15_machine = {
    .name = "virt",
    .desc = "ARM Virtual Machine",
    .init = machvirt_init,
    .max_cpus = 4,
    DEFAULT_MACHINE_OPTIONS,
};

static void machvirt_machine_init(void)
{
    qemu_register_machine(&machvirt_a15_machine);
}

machine_init(machvirt_machine_init);
