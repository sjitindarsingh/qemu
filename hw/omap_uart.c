/*
 * TI OMAP processors UART emulation.
 *
 * Copyright (C) 2006-2008 Andrzej Zaborowski  <balrog@zabor.org>
 * Copyright (C) 2007-2009 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu-char.h"
#include "hw.h"
#include "omap.h"
/* We use pc-style serial ports.  */
#include "pc.h"
#include "exec-memory.h"
#include "sysbus.h"

/* The OMAP UART functionality is similar to the TI16C752 rather than
 * the 16550A. When the flag below is enabled, the code will however
 * offer 'only' the basic 16550A emulation. */
/* TODO: real functionality for the TI16C752 enhanced features. Note
 * QEMU's SerialState emulation internally always uses a 16-byte FIFO
 * whereas we would need a 64-byte FIFO for OMAP. */
#define OMAP_UART_16550A

struct omap_uart_s {
    SysBusDevice busdev;
    MemoryRegion iomem;
    CharDriverState *chr;
    SerialState *serial; /* TODO */
    const MemoryRegionOps *serial_ops;
    uint32_t mmio_size;
    uint32_t baudrate;
    qemu_irq tx_drq;
    qemu_irq rx_drq;

    uint8_t lcr_cache;
    uint8_t eblr;
    uint8_t syscontrol;
    uint8_t wkup;
    uint8_t cfps;
    uint8_t mdr[2];
    uint8_t scr;
    uint8_t clksel;
    uint8_t blr;
    uint8_t acreg;

#ifndef OMAP_UART_16550A
    uint8_t mcr_cache;
    uint8_t efr;
    uint8_t tcr;
    uint8_t tlr;
    uint8_t xon[2], xoff[2];
#endif
};

static void omap_uart_reset(DeviceState *qdev)
{
    struct omap_uart_s *s = FROM_SYSBUS(struct omap_uart_s,
                                        sysbus_from_qdev(qdev));
    s->eblr = 0x00;
    s->syscontrol = 0;
    s->wkup = 0x3f;
    s->cfps = 0x69;
    s->clksel = 0;
    s->blr = 0x40;
    s->acreg = 0;
    s->lcr_cache = 0;

#ifndef OMAP_UART_16550A
    s->mcr_cache = 0;
    s->tcr = 0x0f;
    s->tlr = 0;
    s->efr = 0;
    s->xon[0] = s->xon[1] = 0;
    s->xoff[0] = s->xoff[1] = 0;
#endif
}

static uint64_t omap_uart_read(void *opaque, target_phys_addr_t addr,
                               unsigned size)
{
    struct omap_uart_s *s = (struct omap_uart_s *) opaque;

    switch (addr) {
    case 0x00:
    case 0x04:
    case 0x0c:
        return s->serial_ops->read(s->serial, addr, size);
    case 0x08:
#ifndef OMAP_UART_16550A
        if (s->lcr_cache == 0xbf) {
            return s->efr;
        }
#endif
        return s->serial_ops->read(s->serial, addr, size);
    case 0x10:
    case 0x14:
#ifndef OMAP_UART_16550A
        if (s->lcr_cache == 0xbf) {
            return s->xon[(addr & 7) >> 2];
        } else if (addr == 0x10) {
            return s->serial_ops->read(s->serial, addr, size)
                   | (s->mcr_cache & 0xe0);
        }
#endif
        return s->serial_ops->read(s->serial, addr, size);
    case 0x18:
    case 0x1c:
#ifndef OMAP_UART_16550A
        if ((s->efr & 0x10) && (s->mcr_cache & 0x40)) {
            return (addr == 0x18) ? s->tcr : s->tlr;
        }
        if (s->lcr_cache == 0xbf) {
            return s->xoff[(addr & 7) >> 2];
        }
#endif
        return s->serial_ops->read(s->serial, addr, size);
    case 0x20:	/* MDR1 */
        return s->mdr[0];
    case 0x24:	/* MDR2 */
        return s->mdr[1];
    case 0x28: /* SFLSR */
        return 0;
    case 0x2c: /* RESUME */
        return 0;
    case 0x30: /* SFREGL */
        return 0;
    case 0x34: /* SFREGH */
        return 0;
    case 0x38: /* UASR/BLR */
        if ((s->lcr_cache & 0x80)) {
            return 0; /* FIXME: return correct autodetect value */
        }
        return s->blr;
    case 0x3c: /* ACREG */
        return (s->lcr_cache & 0x80) ? 0 : s->acreg;
    case 0x40:	/* SCR */
        return s->scr;
    case 0x44:	/* SSR */
        return 0x0;
    case 0x48:	/* EBLR (OMAP2) */
        return s->eblr;
    case 0x4C:	/* OSC_12M_SEL (OMAP1) */
        return s->clksel;
    case 0x50:	/* MVR */
        return 0x30;
    case 0x54:	/* SYSC (OMAP2) */
        return s->syscontrol;
    case 0x58:	/* SYSS (OMAP2) */
        return 1;
    case 0x5c:	/* WER (OMAP2) */
        return s->wkup;
    case 0x60:	/* CFPS (OMAP2) */
        return s->cfps;
    }

    OMAP_BAD_REG(addr);
    return 0;
}

static void omap_uart_write(void *opaque, target_phys_addr_t addr,
                            uint64_t value, unsigned size)
{
    struct omap_uart_s *s = (struct omap_uart_s *) opaque;

    switch (addr) {
    case 0x00:
    case 0x04:
        s->serial_ops->write(s->serial, addr, value, size);
        break;
    case 0x08:
#ifndef OMAP_UART_16550A
        if (s->lcr_cache == 0xbf) {
            s->efr = value;
        } else
#endif
        s->serial_ops->write(s->serial, addr, value, size);
        break;
    case 0x0c:
        s->lcr_cache = value;
        s->serial_ops->write(s->serial, addr, value, size);
        break;
    case 0x10:
    case 0x14:
#ifndef OMAP_UART_16550A
        if (s->lcr_cache == 0xbf) {
            s->xon[(addr & 7) >> 2] = value;
        } else {
            if (addr == 0x10) {
                s->mcr_cache = value & 0x7f;
            }
#endif
        s->serial_ops->write(s->serial, addr, value, size);
#ifndef OMAP_UART_16550A
        }
#endif
        break;
    case 0x18:
    case 0x1c:
#ifndef OMAP_UART_16550A
        if ((s->efr & 0x10) && (s->mcr_cache & 0x40)) {
            if (addr == 0x18) {
                s->tcr = value & 0xff;
            } else {
                s->tlr = value & 0xff;
            }
        } else if (s->lcr_cache == 0xbf) {
            s->xoff[(addr & 7) >> 2] = value;
        } else
#endif
        s->serial_ops->write(s->serial, addr, value, size);
        break;
    case 0x20:	/* MDR1 */
        s->mdr[0] = value & 0x7f;
        break;
    case 0x24:	/* MDR2 */
        s->mdr[1] = value & 0xff;
        break;
    case 0x28: /* TXFLL */
    case 0x2c: /* TXFLH */
    case 0x30: /* RXFLL */
    case 0x34: /* RXFLH */
        /* ignored */
        break;
    case 0x38: /* BLR */
        if (!(s->lcr_cache & 0x80)) {
            s->blr = value & 0xc0;
        }
        break;
    case 0x3c: /* ACREG */
        if (!(s->lcr_cache & 0x80)) {
            s->acreg = value & 0xff;
        }
        break;
    case 0x40:	/* SCR */
        s->scr = value & 0xff;
        break;
    case 0x44:	/* SSR */
        OMAP_RO_REG(addr);
        break;
    case 0x48:	/* EBLR (OMAP2) */
        s->eblr = value & 0xff;
        break;
    case 0x4C:	/* OSC_12M_SEL (OMAP1) */
        s->clksel = value & 1;
        break;
    case 0x50:	/* MVR */
        OMAP_RO_REG(addr);
        break;
    case 0x54:	/* SYSC (OMAP2) */
        s->syscontrol = value & 0x1d;
        if (value & 2) {
            /* TODO: reset s->serial also. */
            omap_uart_reset(&s->busdev.qdev);
        }
        break;
    case 0x58:	/* SYSS (OMAP2) */
        OMAP_RO_REG(addr);
        break;
    case 0x5c:	/* WER (OMAP2) */
        s->wkup = value & 0x7f;
        break;
    case 0x60:	/* CFPS (OMAP2) */
        s->cfps = value & 0xff;
        break;
    default:
        OMAP_BAD_REG(addr);
    }
}

static const MemoryRegionOps omap_uart_ops = {
    .read = omap_uart_read,
    .write = omap_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int omap_uart_init(SysBusDevice *busdev)
{
    struct omap_uart_s *s = FROM_SYSBUS(struct omap_uart_s, busdev);
    if (!s->chr) {
        s->chr = qemu_chr_new(busdev->qdev.id, "null", NULL);
    }
    /* TODO: DMA support. Current 16550A emulation does not emulate DMA mode
     * transfers via TXRDY/RXRDY pins. We create DMA irq lines here for
     * future use nevertheless. */
    /* Nasty hackery because trying to extend an existing device is
     * not really supported, and the serial driver isn't even qdev.
     */
    s->serial = serial_mm_init(NULL, 0, 2, NULL, s->baudrate, s->chr,
                               DEVICE_NATIVE_ENDIAN);
    s->serial_ops = serial_get_memops(DEVICE_NATIVE_ENDIAN);
    sysbus_init_irq(busdev, serial_get_irq(s->serial));
    sysbus_init_irq(busdev, &s->tx_drq);
    sysbus_init_irq(busdev, &s->rx_drq);
    memory_region_init_io(&s->iomem, &omap_uart_ops, s, "omap_uart",
                          s->mmio_size);
    sysbus_init_mmio(busdev, &s->iomem);
    return 0;
}

static Property omap_uart_properties[] = {
    DEFINE_PROP_UINT32("mmio_size", struct omap_uart_s, mmio_size, 0x400),
    DEFINE_PROP_UINT32("baudrate", struct omap_uart_s, baudrate, 0),
    DEFINE_PROP_CHR("chardev", struct omap_uart_s, chr),
    DEFINE_PROP_END_OF_LIST()
};

static void omap_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);
    k->init = omap_uart_init;
    dc->props = omap_uart_properties;
    dc->reset = omap_uart_reset;
}

static TypeInfo omap_uart_info = {
    .name = "omap_uart",
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct omap_uart_s),
    .class_init = omap_uart_class_init,
};

static void omap_uart_register_types(void)
{
    type_register_static(&omap_uart_info);
}

void omap_uart_attach(DeviceState *qdev, CharDriverState *chr,
                      const char *label)
{
    struct omap_uart_s *s = FROM_SYSBUS(struct omap_uart_s,
                                        sysbus_from_qdev(qdev));
    s->chr = chr ?: qemu_chr_new(label, "null", NULL);
    serial_change_char_driver(s->serial, s->chr);
}

type_init(omap_uart_register_types)
