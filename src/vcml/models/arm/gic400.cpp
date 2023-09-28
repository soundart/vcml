/******************************************************************************
 *                                                                            *
 * Copyright (C) 2022 MachineWare GmbH                                        *
 * All Rights Reserved                                                        *
 *                                                                            *
 * This is work is licensed under the terms described in the LICENSE file     *
 * found in the root directory of this source tree.                           *
 *                                                                            *
 ******************************************************************************/

#include "vcml/models/arm/gic400.h"

namespace vcml {
namespace arm {

constexpr bool is_software_interrupt(unsigned int irq) {
    return irq < gic400::NSGI;
}

gic400::irq_state::irq_state():
    enabled(0),
    pending(0),
    active(0),
    level(0),
    signaled(0),
    model(N_N),
    trigger(EDGE) {
    // nothing to do
}

gic400::list_entry::list_entry():
    pending(false),
    active(false),
    hw(0),
    prio(0),
    virtual_id(0),
    physical_id(0),
    cpu_id(0) {
    // nothing to do
}

u32 gic400::distif::int_pending_mask(int cpu) {
    u32 value = 0;

    unsigned int mask = 1 << cpu;
    for (unsigned int irq = 0; irq < NPRIV; irq++)
        if (m_parent->test_pending(irq, mask))
            value |= (1 << irq);
    return value;
}

u32 gic400::distif::spi_pending_mask(int cpu) {
    u32 value = 0;

    unsigned int offset = NPRIV + cpu * 32;
    for (unsigned int irq = 0; irq < 32; irq++)
        if (m_parent->test_pending(offset + irq, gic400::ALL_CPU))
            value |= (1 << irq);
    return value;
}

u16 gic400::distif::ppi_enabled_mask(int cpu) {
    u16 value = 0;

    unsigned int mask = 1 << cpu;
    for (unsigned int irq = 0; irq < NPPI; irq++)
        if (m_parent->is_irq_enabled(irq + NSGI, mask))
            value |= (1 << irq);
    return value;
}

enum distif_ctlr_bits : u32 {
    CTRL_ENABLE_GROUP0 = bit(0),
    CTRL_ENABLE_GROUP1 = bit(1),
    CTRL_MASK = CTRL_ENABLE_GROUP0 | CTRL_ENABLE_GROUP1,
};

void gic400::distif::write_ctlr(u32 val) {
    VCML_LOG_REG_BIT_CHANGE(CTRL_ENABLE_GROUP0, ctlr, val);
    VCML_LOG_REG_BIT_CHANGE(CTRL_ENABLE_GROUP1, ctlr, val);
    ctlr = val & CTRL_MASK;
    m_parent->update();
}

u32 gic400::distif::read_typer() {
    u32 itlines = ((m_parent->get_irq_num() + 31) / 32 - 1) & 0x1f;
    u32 cpu_num = (m_parent->get_cpu_num() - 1) & 0x3;
    return (cpu_num << 5) | itlines;
}

u32 gic400::distif::read_isenabler_ppi() {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(iser) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    u32 mask = ppi_enabled_mask(cpu);
    return (mask << 16) | 0xffff; // SGIs are always enabled
}

void gic400::distif::write_isenabler_ppi(u32 val) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(iser) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    unsigned int irq = NSGI;
    unsigned int mask = 1 << cpu;

    for (; irq < NPRIV; irq++) {
        if (val & (1 << irq)) {
            m_parent->enable_irq(irq, mask);
            if (m_parent->get_irq_level(irq, mask) &&
                m_parent->get_irq_trigger(irq) == gic400::LEVEL) {
                m_parent->set_irq_pending(irq, true, mask);
            }
        }
    }

    m_parent->update();
}

u32 gic400::distif::read_isenabler_spi(size_t idx) {
    u32 value = 0;

    unsigned int irq = NPRIV + idx * 32;
    for (unsigned int i = 0; i < 32; i++) {
        if (m_parent->is_irq_enabled(irq + i, gic400::ALL_CPU))
            value |= (1 << i);
    }

    return value;
}

void gic400::distif::write_isenabler_spi(u32 val, size_t idx) {
    unsigned int irq = NPRIV + idx * 32;
    for (unsigned int i = 0; i < 32; i++) {
        if (val & (1 << i)) {
            m_parent->enable_irq(irq + i, gic400::ALL_CPU);
            if ((m_parent->get_irq_level(irq + i, gic400::ALL_CPU)) &&
                (m_parent->get_irq_trigger(irq + i) == gic400::LEVEL)) {
                m_parent->set_irq_pending(irq + i, true, gic400::ALL_CPU);
            }
        }
    }

    m_parent->update();
}

u32 gic400::distif::read_icenabler_ppi() {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(icer) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    u32 mask = ppi_enabled_mask(cpu);
    return (mask << 16) | 0xffff; // SGIs are always enabled
}

void gic400::distif::write_icenabler_ppi(u32 val) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(icer) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    unsigned int irq = NSGI;
    unsigned int mask = 1 << cpu;

    for (; irq < NPRIV; irq++) {
        if (val & (1 << irq))
            m_parent->disable_irq(irq, mask);
    }

    m_parent->update();
}

u32 gic400::distif::read_icenabler_spi(size_t idx) {
    u32 value = 0;

    unsigned int irq = NPRIV + idx * 32;
    for (unsigned int i = 0; i < 32; i++) {
        if (m_parent->is_irq_enabled(irq + i, gic400::ALL_CPU))
            value |= (1 << i);
    }

    return value;
}

void gic400::distif::write_icenabler_spi(u32 val, size_t idx) {
    unsigned int irq = NPRIV + idx * 32;
    for (unsigned int i = 0; i < 32; i++) {
        if (val & (1 << i))
            m_parent->disable_irq(irq + i, gic400::ALL_CPU);
    }

    m_parent->update();
}

u32 gic400::distif::read_ispendr_ppi() {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(ispr) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    return int_pending_mask(cpu);
}

void gic400::distif::write_ispendr_ppi(u32 value) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(ispr) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    unsigned int irq = NSGI;
    unsigned int mask = 1 << cpu;

    for (; irq < NPRIV; irq++) {
        if (value & (1 << irq))
            m_parent->set_irq_pending(irq, true, mask);
    }

    m_parent->update();
}

u32 gic400::distif::read_sspr(size_t idx) {
    return spi_pending_mask(idx);
}

void gic400::distif::write_sspr(u32 value, size_t idx) {
    unsigned int irq = NPRIV + idx * 32;
    for (unsigned int i = 0; i < 32; i++) {
        if (value & (1 << i))
            m_parent->set_irq_pending(irq + i, true, itargets_spi[i]);
    }

    m_parent->update();
}

u32 gic400::distif::read_icpendr_ppi() {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(icpendr0) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    return int_pending_mask(cpu);
}

void gic400::distif::write_icpendr_ppi(u32 value) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(icpendr0) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    unsigned int irq = NSGI;
    unsigned int mask = 1 << cpu;

    for (; irq < NPRIV; irq++) {
        if (value & (1 << irq))
            m_parent->set_irq_pending(irq, false, mask);
    }

    m_parent->update();
}

u32 gic400::distif::read_icpendr_spi(size_t idx) {
    return spi_pending_mask(idx);
}

void gic400::distif::write_icpendr_spi(u32 val, size_t idx) {
    unsigned int irq = NPRIV + idx * 32;
    for (unsigned int i = 0; i < 32; i++) {
        if (val & (1 << i))
            m_parent->set_irq_pending(irq + i, false, gic400::ALL_CPU);
    }

    m_parent->update();
}

u32 gic400::distif::read_isactiver_ppi() {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(isactivr0) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    u32 value = 0;

    unsigned int mask = 1 << cpu;
    for (unsigned int l = 0; l < NPRIV; l++) {
        if (m_parent->is_irq_active(l, mask))
            value |= (1 << l);
    }

    return value;
}

u32 gic400::distif::read_isactiver_spi(size_t idx) {
    u32 value = 0;

    unsigned int irq = NPRIV + idx * 32;
    for (unsigned int i = 0; i < 32; i++) {
        if (m_parent->is_irq_active(irq + i, gic400::ALL_CPU))
            value |= (1 << i);
    }

    return value;
}

void gic400::distif::write_icactiver_ppi(u32 val) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(icar) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    unsigned int mask = 1 << cpu;
    for (unsigned int irq = 0; irq < 32; irq++) {
        if (val & (1 << irq))
            m_parent->set_irq_active(irq, false, mask);
    }
}

void gic400::distif::write_icactiver_spi(u32 val, size_t idx) {
    unsigned int irq = NPRIV + idx * 32;
    for (unsigned int i = 0; i < 32; i++) {
        if (val & (1 << i))
            m_parent->set_irq_active(irq + i, false, gic400::ALL_CPU);
    }
}

u32 gic400::distif::read_itargets_ppi(size_t idx) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(intt) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    // local cpu is always target for its own SGIs and PPIs
    return 0x01010101 << cpu;
}

void gic400::distif::write_icfgr(u32 value) {
    icfgr_ppi = value & 0xaaaaaaaa; // odd bits are reserved, zero them out

    unsigned int irq = NSGI;
    for (unsigned int i = 0; i < NPPI; i++) {
        if (value & (2 << (i * 2))) {
            m_parent->set_irq_trigger(irq + i, gic400::EDGE);
            log_debug("irq %u configured to be edge sensitive", irq + i);
        } else {
            m_parent->set_irq_trigger(irq + i, gic400::LEVEL);
            log_debug("irq %u configured to be level sensitive", irq + i);
        }
    }

    m_parent->update();
}

void gic400::distif::write_icfgr_spi(u32 value, size_t idx) {
    icfgr_spi[idx] = value & 0xaaaaaaaa; // odd bits are reserved

    unsigned int irq = NPRIV + idx * 16;
    for (unsigned int i = 0; i < 16; i++) {
        if (value & (2 << (i * 2))) {
            m_parent->set_irq_trigger(irq + i, gic400::EDGE);
            log_debug("irq %u configured to be edge sensitive", irq + i);
        } else {
            m_parent->set_irq_trigger(irq + i, gic400::LEVEL);
            log_debug("irq %u configured to be level sensitive", irq + i);
        }
    }

    m_parent->update();
}

void gic400::distif::write_sgir(u32 value) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(sctl) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    unsigned int src_cpu = 1 << cpu;
    unsigned int sgi_num = (value >> 0) & 0x0f;
    unsigned int targets = (value >> 16) & 0xff;
    unsigned int filters = (value >> 24) & 0x03;

    switch (filters) {
    case 0x0:
        // forward interrupt to CPUs specified in CPU target list
        break;
    case 0x1:
        // forward interrupt to all CPUs except writing CPU
        targets = gic400::ALL_CPU ^ src_cpu;
        break;
    case 0x2:
        // forward interrupt only to writing CPU
        targets = 1 << cpu;
        break;
    default:
        log_warn("bad SGI target filter");
        break;
    }

    m_parent->set_irq_pending(sgi_num, true, targets);
    for (unsigned int target = 0; target < NCPU; target++) {
        if (targets & (1 << target))
            set_sgi_pending(1 << cpu, sgi_num, target, true);
    }

    m_parent->set_irq_signaled(sgi_num, false, targets);
    m_parent->update();
}

void gic400::distif::write_spendsgir(u8 value, size_t idx) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(sgis) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    unsigned int mask = 1 << cpu;
    unsigned int irq = idx;

    set_sgi_pending(value, irq, cpu, true);
    m_parent->set_irq_pending(irq, true, mask);
    m_parent->set_irq_signaled(irq, false, mask);
    m_parent->update();
}

void gic400::distif::write_cpendsgir(u8 value, size_t idx) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(sgic) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    unsigned int mask = 1 << cpu;
    unsigned int irq = idx;

    set_sgi_pending(value, irq, cpu, false);
    if (cpendsgir.bank(cpu, idx) == 0) // clear SGI if no sources remain
        m_parent->set_irq_pending(irq, false, mask);
    m_parent->update();
}

gic400::distif::distif(const sc_module_name& nm):
    peripheral(nm),
    m_parent(dynamic_cast<gic400*>(get_parent_object())),
    ctlr("ctlr", 0x000, 0x00000000),
    typer("typer", 0x004, 0x00000000),
    iidr("iidr", 0x008, 0x00000000),
    igroupr("igroupr", 0x80, 0x00000000),
    isenabler_ppi("isenabler_ppi", 0x100, 0x0000ffff),
    isenabler_spi("isenabler_spi", 0x104, 0x00000000),
    icenabler_ppi("icenabler_ppi", 0x180, 0x0000ffff),
    icenabler_spi("icenabler_spi", 0x184, 0x00000000),
    ispendr_ppi("ispendr_ppi", 0x200, 0x00000000),
    ispendr_spi("ispendr_spi", 0x204, 0x00000000),
    icpendr_ppi("icpendr_ppi", 0x280, 0x00000000),
    icpendr_spi("icpendr_spi", 0x284, 0x00000000),
    isactiver_ppi("isactiver_ppi", 0x300),
    isactiver_spi("isactiver_spi", 0x304),
    icactiver_ppi("icactiver_ppi", 0x380, 0x00000000),
    icactiver_spi("icactiver_spi", 0x384, 0x00000000),
    ipriority_sgi("ipriority_sgi", 0x400, 0x00),
    ipriority_ppi("ipriority_ppi", 0x410, 0x00),
    ipriority_spi("ipriority_spi", 0x420, 0x00),
    itargets_ppi("itargets_ppi", 0x800),
    itargets_spi("itargets_spi", 0x820),
    icfgr_sgi("icfgr_sgi", 0xc00, 0xaaaaaaaa),
    icfgr_ppi("icfgr_ppi", 0xc04, 0xaaaaaaaa),
    icfgr_spi("icfgr_spi", 0xc08),
    sgir("sgir", 0xf00),
    cpendsgir("cpendsgir", 0xf10),
    spendsgir("spendsgir", 0xf20),
    cidr("cidr", 0xff0),
    in("in") {
    VCML_ERROR_ON(!m_parent, "gic400 parent module not found");

    ctlr.sync_on_write();
    ctlr.allow_read_write();
    ctlr.on_write(&distif::write_ctlr);

    typer.sync_never();
    typer.allow_read_only();
    typer.on_read(&distif::read_typer);

    iidr.sync_never();
    iidr.allow_read_only();

    igroupr.sync_always();
    igroupr.allow_read_write();

    isenabler_ppi.set_banked();
    isenabler_ppi.sync_always();
    isenabler_ppi.allow_read_write();
    isenabler_ppi.on_read(&distif::read_isenabler_ppi);
    isenabler_ppi.on_write(&distif::write_isenabler_ppi);

    isenabler_spi.sync_always();
    isenabler_spi.allow_read_write();
    isenabler_spi.on_read(&distif::read_isenabler_spi);
    isenabler_spi.on_write(&distif::write_isenabler_spi);

    icenabler_ppi.set_banked();
    icenabler_ppi.sync_always();
    icenabler_ppi.allow_read_write();
    icenabler_ppi.on_read(&distif::read_icenabler_ppi);
    icenabler_ppi.on_write(&distif::write_icenabler_ppi);

    icenabler_spi.sync_always();
    icenabler_spi.allow_read_write();
    icenabler_spi.on_read(&distif::read_icenabler_spi);
    icenabler_spi.on_write(&distif::write_icenabler_spi);

    ispendr_ppi.set_banked();
    ispendr_ppi.sync_always();
    ispendr_ppi.allow_read_write();
    ispendr_ppi.on_read(&distif::read_ispendr_ppi);
    ispendr_ppi.on_write(&distif::write_ispendr_ppi);

    ispendr_spi.sync_always();
    ispendr_spi.allow_read_write();
    ispendr_spi.on_read(&distif::read_sspr);
    ispendr_spi.on_write(&distif::write_sspr);

    icpendr_ppi.set_banked();
    icpendr_ppi.sync_always();
    icpendr_ppi.allow_read_write();
    icpendr_ppi.on_read(&distif::read_icpendr_ppi);
    icpendr_ppi.on_write(&distif::write_icpendr_ppi);

    icpendr_spi.sync_always();
    icpendr_spi.allow_read_write();
    icpendr_spi.on_read(&distif::read_icpendr_spi);
    icpendr_spi.on_write(&distif::write_icpendr_spi);

    isactiver_ppi.set_banked();
    isactiver_ppi.allow_read_only();
    isactiver_ppi.sync_on_read();
    isactiver_ppi.on_read(&distif::read_isactiver_ppi);

    isactiver_spi.allow_read_only();
    isactiver_spi.sync_on_read();
    isactiver_spi.on_read(&distif::read_isactiver_spi);

    icactiver_ppi.set_banked();
    icactiver_ppi.sync_on_write();
    icactiver_ppi.allow_read_write();
    icactiver_ppi.on_write(&distif::write_icactiver_ppi);

    icactiver_spi.sync_on_write();
    icactiver_spi.allow_read_write();
    icactiver_spi.on_write(&distif::write_icactiver_spi);

    ipriority_sgi.set_banked();
    ipriority_sgi.sync_never();
    ipriority_sgi.allow_read_write();

    ipriority_ppi.set_banked();
    ipriority_ppi.sync_never();
    ipriority_ppi.allow_read_write();

    ipriority_sgi.sync_never();
    ipriority_sgi.allow_read_write();

    itargets_ppi.set_banked();
    itargets_ppi.sync_always();
    itargets_ppi.allow_read_write();
    itargets_ppi.on_read(&distif::read_itargets_ppi);

    itargets_spi.sync_always();
    itargets_spi.allow_read_write();

    icfgr_sgi.allow_read_only();
    icfgr_sgi.sync_on_read();

    icfgr_ppi.sync_on_write();
    icfgr_ppi.allow_read_write();
    icfgr_ppi.on_write(&distif::write_icfgr);

    icfgr_spi.sync_on_write();
    icfgr_spi.allow_read_write();
    icfgr_spi.on_write(&distif::write_icfgr_spi);

    sgir.set_banked();
    sgir.allow_write_only();
    sgir.sync_on_write();
    sgir.on_write(&distif::write_sgir);

    spendsgir.set_banked();
    spendsgir.sync_always();
    spendsgir.allow_read_write();
    spendsgir.on_write(&distif::write_spendsgir);

    cpendsgir.set_banked();
    cpendsgir.sync_always();
    cpendsgir.allow_read_write();
    cpendsgir.on_write(&distif::write_cpendsgir);

    cidr.allow_read_only();
    cidr.sync_never();
}

gic400::distif::~distif() {
    // nothing to do
}

void gic400::distif::reset() {
    peripheral::reset();

    for (unsigned int i = 0; i < cidr.count(); i++)
        cidr[i] = (AMBA_PCID >> (i * 8)) & 0xff;
}

void gic400::distif::set_sgi_pending(u8 value, unsigned int sgi,
                                     unsigned int cpu, bool set) {
    if (set) {
        spendsgir.bank(cpu, sgi) |= value;
        cpendsgir.bank(cpu, sgi) |= value;
    } else {
        spendsgir.bank(cpu, sgi) &= ~value;
        cpendsgir.bank(cpu, sgi) &= ~value;
    }
}

void gic400::distif::end_of_elaboration() {
    // SGIs are enabled per default and cannot be disabled
    for (unsigned int irq = 0; irq < NSGI; irq++)
        m_parent->enable_irq(irq, gic400::ALL_CPU);
}

void gic400::cpuif::set_current_irq(unsigned int cpu, unsigned int irq) {
    m_curr_irq[cpu] = irq;

    if (irq == SPURIOUS_IRQ)
        rpr.bank(cpu) = 0x100;
    else
        rpr.bank(cpu) = m_parent->get_irq_priority(cpu, irq);

    m_parent->update();
}

void gic400::cpuif::write_ctlr(u32 val) {
    if ((val & CTLR_ENABLE()) && !(ctlr & CTLR_ENABLE()))
        log_debug("(ctlr) enabling cpu %d", current_cpu());
    if (!(val & CTLR_ENABLE()) && (ctlr & CTLR_ENABLE()))
        log_debug("(ctlr) disabling cpu %d", current_cpu());
    ctlr = val & CTLR_ENABLE();
}

void gic400::cpuif::write_pmr(u32 val) {
    pmr = val & 0x000000ff; // read only first 8 bits
}

void gic400::cpuif::write_bpr(u32 val) {
    abpr = val & 0x7; // read only first 3 bits, store copy in ABPR
    bpr = abpr;
}

void gic400::cpuif::write_eoir(u32 val) {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(eoi) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    if (m_curr_irq[cpu] == SPURIOUS_IRQ)
        return; // no active IRQ

    unsigned int irq = val & 0x3ff; // interrupt id stored in bits [9..0]
    if (irq >= m_parent->get_irq_num()) {
        log_warn("(eoi) invalid irq %d ignored", irq);
        return;
    }

    if (irq == m_curr_irq[cpu]) {
        log_debug("(eoi) cpu %d eois irq %d", cpu, irq);
        set_current_irq(cpu, m_prev_irq[irq][cpu]);
        m_parent->set_irq_active(irq, false, 1 << cpu);
        m_parent->update();
        return;
    }

    // handle IRQ that is not currently running
    int iter = m_curr_irq[cpu];
    while (m_prev_irq[iter][cpu] != SPURIOUS_IRQ) {
        if (m_prev_irq[iter][cpu] == irq) {
            m_prev_irq[iter][cpu] = m_prev_irq[irq][cpu];
            break;
        }

        iter = m_prev_irq[iter][cpu];
    }
}

u32 gic400::cpuif::read_iar() {
    int cpu = current_cpu();
    if (cpu < 0) {
        log_warn("(iar) invalid cpu %d, assuming 0", cpu);
        cpu = 0;
    }

    int irq = hppir.bank(cpu);
    int cpu_mask = (m_parent->get_irq_model(irq) == gic400::N_1)
                       ? (gic400::ALL_CPU)
                       : (1 << cpu);

    log_debug("(iar) cpu %d acknowledges irq %d", cpu, irq);

    // check if CPU is acknowledging a not pending interrupt
    if (irq == SPURIOUS_IRQ ||
        m_parent->get_irq_priority(cpu, irq) >= rpr.bank(cpu)) {
        return SPURIOUS_IRQ;
    }

    if (is_software_interrupt(irq)) {
        u32 pending = m_parent->distif.spendsgir.bank(cpu, irq);

        unsigned int src_cpu = ctz(pending);
        VCML_ERROR_ON(src_cpu > 8, "invalid src_cpu: %u", src_cpu);
        m_parent->distif.set_sgi_pending(1 << src_cpu, irq, cpu, false);

        // check if SGI is not pending from any CPUs
        if (m_parent->distif.spendsgir.bank(cpu, irq) == 0)
            m_parent->set_irq_pending(irq, false, cpu_mask);
        iar = (src_cpu & 0x7) << 10 | irq;
    } else {
        // clear pending state for interrupt 'irq'
        m_parent->set_irq_pending(irq, false, cpu_mask);
        iar = irq;
    }

    m_prev_irq[irq][cpu] = m_curr_irq[cpu];
    set_current_irq(cpu, irq); // set the acknowledged IRQ to running
    m_parent->set_irq_active(irq, true, cpu_mask);
    m_parent->set_irq_signaled(irq, true, cpu_mask);
    return iar;
}

gic400::cpuif::cpuif(const sc_module_name& nm):
    peripheral(nm),
    m_parent(dynamic_cast<gic400*>(get_parent_object())),
    ctlr("ctlr", 0x00, 0x0),
    pmr("pmr", 0x04, 0x0),
    bpr("bpr", 0x08, 0x0),
    iar("iar", 0x0c, 0x0),
    eoir("eoir", 0x10, 0x0),
    rpr("rpr", 0x14, IDLE_PRIO),
    hppir("hppir", 0x18, SPURIOUS_IRQ),
    abpr("abpr", 0x1c, 0x0),
    apr("apr", 0xd0, 0x00000000),
    iidr("iidr", 0xfc, AMBA_IFID),
    cidr("cidr", 0xff0),
    dir("dir", 0x1000),
    in("in") {
    VCML_ERROR_ON(!m_parent, "gic400 parent module not found");

    ctlr.set_banked();
    ctlr.sync_always();
    ctlr.allow_read_write();
    ctlr.on_write(&cpuif::write_ctlr);

    pmr.set_banked();
    pmr.sync_always();
    pmr.allow_read_write();
    pmr.on_write(&cpuif::write_pmr);

    bpr.set_banked();
    bpr.sync_always();
    bpr.allow_read_write();
    bpr.on_write(&cpuif::write_bpr);

    iar.set_banked();
    iar.allow_read_only();
    iar.sync_on_read();
    iar.on_read(&cpuif::read_iar);

    eoir.set_banked();
    eoir.allow_write_only();
    eoir.sync_on_write();
    eoir.on_write(&cpuif::write_eoir);

    rpr.set_banked();
    rpr.sync_never();
    rpr.allow_read_only();

    hppir.set_banked();
    hppir.sync_never();
    hppir.allow_read_only();

    abpr.set_banked();
    abpr.sync_always();
    abpr.allow_read_write();

    apr.sync_always();
    apr.allow_read_write();

    iidr.sync_never();
    iidr.allow_read_only();

    cidr.sync_never();
    cidr.allow_read_only();

    dir.set_banked();
    dir.sync_always();
    dir.allow_read_write();
}

gic400::cpuif::~cpuif() {
    // nothing to do
}

void gic400::cpuif::reset() {
    peripheral::reset();

    for (unsigned int i = 0; i < cidr.count(); i++)
        cidr[i] = (AMBA_PCID >> (i * 8)) & 0xff;

    for (unsigned int irq = 0; irq < NIRQ; irq++)
        for (unsigned int cpu = 0; cpu < NCPU; cpu++)
            m_prev_irq[irq][cpu] = SPURIOUS_IRQ;

    for (unsigned int cpu = 0; cpu < NCPU; cpu++)
        m_curr_irq[cpu] = SPURIOUS_IRQ;
}

void gic400::vifctrl::write_hcr(u32 val) {
    hcr.bank(current_cpu()) = val;
    m_parent->update(true);
}

u32 gic400::vifctrl::read_vtr() {
    return 0x90000000 | (NLR - 1);
}

void gic400::vifctrl::write_lr(u32 val, size_t idx) {
    u8 cpu = current_cpu();
    u8 state = (val >> 28) & 0b11;
    u8 hw = (val >> 31) & 0b1;

    if (hw == 0) {
        u8 eoi = (val >> 19) & 0b1;
        if (eoi == 1)
            log_error("(lr) maintenance IRQ not implemented");
        u8 cpu_id = (val >> 10) & 0b111;
        set_lr_cpuid(idx, cpu, cpu_id);
        set_lr_hw(idx, cpu, false);
        set_lr_physid(idx, cpu, 0);
    } else {
        set_lr_cpuid(idx, cpu, 0);
        set_lr_hw(idx, cpu, true);
        u16 physid = (val >> 10) & 0x1ff;
        set_lr_physid(idx, cpu, physid);
    }

    if (state & 1)
        set_lr_pending(idx, cpu, true);
    if (state & 2)
        set_lr_active(idx, cpu, true);
    if (state == 0) {
        set_lr_pending(idx, cpu, false);
        set_lr_active(idx, cpu, false);
    }

    u32 prio = (val >> 23) & 0x1f;
    u32 irq = val & 0x1ff;

    set_lr_prio(idx, cpu, prio);
    set_lr_vid(idx, cpu, irq);

    lr[idx] = val;
    m_parent->update(true);
}

u32 gic400::vifctrl::read_lr(size_t idx) {
    u8 cpu = current_cpu();

    // update pending and active bit
    if (is_lr_pending(idx, cpu))
        lr[idx] = lr[idx] | LR_PENDING_MASK;
    else
        lr[idx] = lr[idx] & ~LR_PENDING_MASK;

    if (is_lr_active(idx, cpu))
        lr[idx] = lr[idx] | LR_ACTIVE_MASK;
    else
        lr[idx] = lr[idx] & ~LR_ACTIVE_MASK;

    return lr[idx];
}

void gic400::vifctrl::write_vmcr(u32 val) {
    int cpu = current_cpu();
    u8 pmask = (val >> 27) & 0x1f;
    u8 bpr = (val >> 21) & 0x03;
    u32 ctlr = val & 0x1ff;

    m_parent->vcpuif.pmr.bank(cpu) = pmask << 3;
    m_parent->vcpuif.bpr.bank(cpu) = bpr;
    m_parent->vcpuif.ctlr.bank(cpu) = ctlr;
}

u32 gic400::vifctrl::read_vmcr() {
    int cpu = current_cpu();
    u8 pmask = (m_parent->vcpuif.pmr.bank(cpu) >> 3) & 0x1f;
    u8 bpr = m_parent->vcpuif.bpr.bank(cpu) & 0x03;
    u32 ctlr = m_parent->vcpuif.ctlr.bank(cpu) & 0x1ff;

    return (pmask << 27 | bpr << 21 | ctlr);
}

void gic400::vifctrl::write_apr(u32 val) {
    int cpu = current_cpu();
    u8 prio = IDLE_PRIO;
    if (val != 0)
        prio = fls(val) << (VIRT_MIN_BPR + 1);
    m_parent->vcpuif.rpr.bank(cpu) = prio;
    apr = val;
}

u8 gic400::vifctrl::get_irq_priority(unsigned int cpu, unsigned int irq) {
    for (unsigned int i = 0; i < NLR; i++) {
        if (m_lr_state[cpu][i].virtual_id == irq &&
            (m_lr_state[cpu][i].active || m_lr_state[cpu][i].pending)) {
            return m_lr_state[cpu][i].prio;
        }
    }

    log_error("failed getting LR prio for irq %d on cpu %d", irq, cpu);
    return 0;
}

u8 gic400::vifctrl::get_lr(unsigned int irq, u8 cpu) {
    for (unsigned int i = 0; i < NLR; i++) {
        if (m_lr_state[cpu][i].virtual_id == irq &&
            (m_lr_state[cpu][i].active || m_lr_state[cpu][i].pending)) {
            return i;
        }
    }

    log_error("failed getting LR for irq %d on cpu%d", irq, cpu);
    return 0;
}

gic400::vifctrl::vifctrl(const sc_module_name& nm):
    peripheral(nm),
    m_parent(dynamic_cast<gic400*>(get_parent_object())),
    m_lr_state(),
    hcr("hcr", 0x0),
    vtr("vtr", 0x04, 0x0),
    vmcr("vmcr", 0x08),
    apr("apr", 0xf0, 0x0),
    lr("lr", 0x100, 0x0),
    in("in") {
    hcr.set_banked();
    hcr.allow_read_write();
    hcr.on_write(&vifctrl::write_hcr);

    vtr.allow_read_only();
    vtr.on_read(&vifctrl::read_vtr);

    lr.set_banked();
    lr.allow_read_write();
    lr.on_write(&vifctrl::write_lr);
    lr.on_read(&vifctrl::read_lr);

    vmcr.allow_read_write();
    vmcr.on_read(&vifctrl::read_vmcr);
    vmcr.on_write(&vifctrl::write_vmcr);

    apr.set_banked();
    apr.allow_read_write();
    apr.on_write(&vifctrl::write_apr);
}

gic400::vifctrl::~vifctrl() {
    // nothing to do
}

void gic400::vcpuif::write_ctlr(u32 val) {
    if (val > 1)
        log_error("(vctlr) using unimplemented features");
    ctlr = val;
}

void gic400::vcpuif::write_bpr(u32 val) {
    bpr = std::max(val & 0x07, (u32)VIRT_MIN_BPR);
}

u32 gic400::vcpuif::read_iar() {
    u8 cpu = current_cpu();

    u32 irq = hppir.bank(cpu);
    if (irq == SPURIOUS_IRQ ||
        m_vifctrl->get_irq_priority(cpu, irq) >= rpr.bank(cpu))
        return SPURIOUS_IRQ;

    u32 prio = m_vifctrl->get_irq_priority(cpu, irq) << 3;
    u32 mask = ~0ul << ((bpr.bank(cpu) & 0x07) + 1);
    rpr.bank(cpu) = prio & mask;
    u32 preemptlvl = prio >> (VIRT_MIN_BPR + 1);
    u32 bitno = preemptlvl % 32;
    m_parent->vifctrl.apr.bank(cpu) |= (1 << bitno);

    u8 lr = m_vifctrl->get_lr(irq, cpu);
    m_vifctrl->set_lr_active(lr, cpu, true);

    m_vifctrl->set_lr_pending(lr, cpu, false);

    log_debug("(viack) cpu %d acknowledges virq %d", cpu, irq);
    m_parent->update(true);
    u8 cpu_id = m_vifctrl->get_lr_cpuid(lr, cpu);
    return ((cpu_id & 0x111) << 10 | irq);
}

void gic400::vcpuif::write_eoir(u32 val) {
    u8 cpu = current_cpu();
    u32 irq = val & 0x1ff;

    if (irq >= m_parent->get_irq_num()) {
        log_warn("(eoir) invalid irq %d ignored", irq);
        return;
    }

    log_debug("(veoir) cpu%d eois virq %d", cpu, irq);

    // drop priority and update APR
    m_vifctrl->apr.bank(cpu) &= m_parent->vifctrl.apr.bank(cpu) - 1;
    u32 apr = m_parent->vifctrl.apr.bank(cpu);

    if (apr)
        rpr.bank(cpu) = fls(apr) << (VIRT_MIN_BPR + 1);
    else
        rpr.bank(cpu) = IDLE_PRIO;

    // deactivate interrupt
    u8 lr = m_vifctrl->get_lr(irq, cpu);
    m_vifctrl->set_lr_active(lr, cpu, false);
    if (m_vifctrl->is_lr_hw(lr, cpu)) {
        u16 physid = m_vifctrl->get_lr_physid(lr, cpu);
        if (!(physid < NSGI || physid > NIRQ)) {
            m_parent->set_irq_active(physid, false, 1 << cpu);
        } else {
            log_error("unexpected physical id %d for cpu %d in LR %d", physid,
                      cpu, lr);
        }
    }

    m_parent->update(true);
    eoir = val;
}

gic400::vcpuif::vcpuif(const sc_module_name& nm, class vifctrl* ctrl):
    peripheral(nm),
    m_parent(dynamic_cast<gic400*>(get_parent_object())),
    m_vifctrl(ctrl),
    ctlr("ctlr", 0x00, 0x0),
    pmr("pmr", 0x04, 0x0),
    bpr("bpr", 0x08, 2),
    iar("iar", 0x0c, 0x0),
    eoir("eoir", 0x10, 0x0),
    rpr("rpr", 0x14, IDLE_PRIO),
    hppir("hppir", 0x18, SPURIOUS_IRQ),
    apr("apr", 0xd0, 0x00000000),
    iidr("iidr", 0xfc, AMBA_IFID),
    in("in") {
    ctlr.set_banked();
    ctlr.allow_read_write();
    ctlr.on_write(&vcpuif::write_ctlr);

    pmr.set_banked();
    pmr.allow_read_write();

    bpr.set_banked();
    bpr.allow_read_write();
    bpr.on_write(&vcpuif::write_bpr);

    iar.set_banked();
    iar.allow_read_only();
    iar.on_read(&vcpuif::read_iar);

    eoir.set_banked();
    eoir.allow_write_only();
    eoir.on_write(&vcpuif::write_eoir);

    rpr.set_banked();

    hppir.set_banked();
    hppir.allow_read_write();

    apr.set_banked();
    apr.allow_read_write();

    iidr.allow_read_only();
}

gic400::vcpuif::~vcpuif() {
    // nothing to do
}

void gic400::vcpuif::reset() {
    rpr = rpr.get_default();
    hppir = hppir.get_default();
}

gic400::gic400(const sc_module_name& nm):
    peripheral(nm),
    distif("distif"),
    cpuif("cpuif"),
    vifctrl("vifctrl"),
    vcpuif("vcpuif", &vifctrl),
    ppi_in("ppi_in", NPPI * NCPU, IRQ_AS_PPI),
    spi_in("spi_in", NSPI, IRQ_AS_SPI),
    fiq_out("fiq_out", NCPU),
    irq_out("irq_out", NCPU),
    vfiq_out("vfiq_out", NVCPU),
    virq_out("virq_out", NVCPU),
    m_irq_num(NPRIV),
    m_cpu_num(0),
    m_irq_state() {
    clk.bind(distif.clk);
    clk.bind(cpuif.clk);
    clk.bind(vifctrl.clk);
    clk.bind(vcpuif.clk);
    rst.bind(distif.rst);
    rst.bind(cpuif.rst);
    rst.bind(vifctrl.rst);
    rst.bind(vcpuif.rst);
}

gic400::~gic400() {
    // nothing to do
}

void gic400::update(bool virt) {
    for (unsigned int cpu = 0; cpu < m_cpu_num; cpu++) {
        unsigned int irq;
        unsigned int mask = 1 << cpu;
        unsigned int best_irq = SPURIOUS_IRQ;
        unsigned int best_prio = IDLE_PRIO;

        if (!virt)
            cpuif.hppir.bank(cpu) = SPURIOUS_IRQ;
        else
            vcpuif.hppir.bank(cpu) = SPURIOUS_IRQ;

        if (!virt && (distif.ctlr == 0u || cpuif.ctlr.bank(cpu) == 0u)) {
            log_debug("disabling cpu%u irq", cpu);
            irq_out[cpu] = false;
            continue;
        }

        if (virt && (vifctrl.hcr.bank(cpu) == 0u)) {
            log_debug("disabling cpu%u virq", cpu);
            virq_out[cpu] = false;
            continue;
        }

        if (!virt) {
            // check SGIs
            for (irq = 0; irq < NSGI; irq++) {
                if (is_irq_enabled(irq, mask) && test_pending(irq, mask) &&
                    !is_irq_active(irq, mask)) {
                    if (distif.ipriority_sgi.bank(cpu, irq) < best_prio) {
                        best_prio = distif.ipriority_sgi.bank(cpu, irq);
                        best_irq = irq;
                    }
                }
            }

            // check PPIs
            for (irq = NSGI; irq < NPRIV; irq++) {
                if (is_irq_enabled(irq, mask) && test_pending(irq, mask) &&
                    !is_irq_active(irq, mask)) {
                    int idx = irq - NSGI;
                    if (distif.ipriority_ppi.bank(cpu, idx) < best_prio) {
                        best_prio = distif.ipriority_ppi.bank(cpu, idx);
                        best_irq = irq;
                    }
                }
            }

            // check SPIs
            for (irq = NPRIV; irq < m_irq_num; irq++) {
                int idx = irq - NPRIV;
                if (is_irq_enabled(irq, mask) && test_pending(irq, mask) &&
                    (distif.itargets_spi[idx] & mask) &&
                    !is_irq_active(irq, mask)) {
                    if (distif.ipriority_spi[idx] < best_prio) {
                        best_prio = distif.ipriority_spi[idx];
                        best_irq = irq;
                    }
                }
            }

        } else {
            for (unsigned int lr_idx = 0; lr_idx < NLR; lr_idx++) {
                if (vifctrl.is_lr_pending(lr_idx, cpu)) {
                    u8 prio = (vifctrl.lr.bank(cpu, lr_idx) & (0x1f << 23)) >>
                              23;
                    if (prio < best_prio) {
                        best_prio = prio;
                        best_irq = (vifctrl.lr.bank(cpu, lr_idx) & 0x1ff);
                    }
                }
            }
        }

        // signal IRQ to processor if priority is higher
        bool level = false;
        if (!virt) {
            if (best_prio < cpuif.pmr.bank(cpu)) {
                log_debug("setting irq %u pending on cpuif %u", best_irq, cpu);
                cpuif.hppir.bank(cpu) = best_irq;
                if (best_prio < cpuif.rpr.bank(cpu))
                    level = true;
            }
        } else {
            if (best_prio < vcpuif.pmr.bank(cpu)) {
                vcpuif.hppir.bank(cpu) = best_irq;
                if (best_prio < vcpuif.rpr.bank(cpu))
                    level = true;
            }
        }

        if (!virt) {
            if (irq_out[cpu].read() != level) {
                log_debug("%sing cpu%u irq for irq%u",
                          level ? "sett" : "clear", cpu, best_irq);
            }
            irq_out[cpu].write(level);
        } else {
            if (virq_out[cpu].read() != level) {
                log_debug("%sing cpu%u virq for irq %u",
                          level ? "sett" : "clear", cpu, best_irq);
            }
            virq_out[cpu].write(level);
        }
    }
}

u8 gic400::get_irq_priority(unsigned int cpu, unsigned int irq) {
    if (irq < NSGI)
        return distif.ipriority_sgi.bank(cpu, irq);
    else if (irq < NPRIV)
        return distif.ipriority_ppi.bank(cpu, irq - NSGI);
    else if (irq < NIRQ)
        return distif.ipriority_spi[irq - NPRIV];

    log_error("tried to get IRQ priority of invalid IRQ ID (%d)", irq);
    return 0;
}

void gic400::end_of_elaboration() {
    m_cpu_num = 0;
    m_irq_num = NPRIV;

    // determine the number of processors from the connected IRQs
    for (auto& [cpu, port] : irq_out)
        m_cpu_num = max<unsigned int>(m_cpu_num, cpu + 1);

    for (auto& [spi, port] : spi_in) {
        unsigned int irq = spi + NPRIV;

        if (irq >= NIRQ)
            VCML_ERROR("too many interrupts (%u)", irq);

        if (irq >= m_irq_num)
            m_irq_num = irq + 1;
    }

    log_debug("found %u cpus with %u irqs in total", m_cpu_num, m_irq_num);
}

void gic400::gpio_notify(const gpio_target_socket& socket) {
    switch (socket.as) {
    case IRQ_AS_PPI: {
        unsigned int cpu = ppi_in.index_of(socket) / NPPI;
        unsigned int idx = ppi_in.index_of(socket) % NPPI;
        handle_ppi(cpu, idx, socket.read());
        break;
    }

    case IRQ_AS_SPI: {
        unsigned int idx = spi_in.index_of(socket);
        handle_spi(idx, socket.read());
        break;
    }

    default:
        VCML_ERROR("illegal interrupt space: %d", (int)socket.as);
    }
}

void gic400::handle_ppi(unsigned int cpu, unsigned int idx, bool state) {
    unsigned int irq = NSGI + idx;
    unsigned int mask = 1 << cpu;

    set_irq_level(irq, state, mask);
    set_irq_signaled(irq, false, gic400::ALL_CPU);
    if (get_irq_trigger(irq) == EDGE && state)
        set_irq_pending(irq, true, mask);

    update();
}

void gic400::handle_spi(unsigned int idx, bool state) {
    unsigned int irq = NPRIV + idx;
    unsigned int target_cpu = distif.itargets_spi[idx];

    set_irq_level(irq, state, gic400::ALL_CPU);
    set_irq_signaled(irq, false, gic400::ALL_CPU);
    if (get_irq_trigger(irq) == EDGE && state)
        set_irq_pending(irq, true, target_cpu);

    update();
}

VCML_EXPORT_MODEL(vcml::arm::gic400, name, args) {
    return new gic400(name);
}

} // namespace arm
} // namespace vcml
