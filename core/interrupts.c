/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <skiboot.h>
#include <cpu.h>
#include <fsp.h>
#include <interrupts.h>
#include <opal.h>
#include <io.h>
#include <cec.h>
#include <device.h>
#include <ccan/str/str.h>
#include <timer.h>

/* ICP registers */
#define ICP_XIRR		0x4	/* 32-bit access */
#define ICP_CPPR		0x4	/* 8-bit access */
#define ICP_MFRR		0xc	/* 8-bit access */

struct irq_source {
	uint32_t			start;
	uint32_t			end;
	const struct irq_source_ops	*ops;
	void				*data;
	struct list_node		link;
};

static LIST_HEAD(irq_sources);
static struct lock irq_lock = LOCK_UNLOCKED;

void register_irq_source(const struct irq_source_ops *ops, void *data,
			 uint32_t start, uint32_t count)
{
	struct irq_source *is, *is1;

	is = zalloc(sizeof(struct irq_source));
	assert(is);
	is->start = start;
	is->end = start + count;
	is->ops = ops;
	is->data = data;

	prlog(PR_DEBUG, "IRQ: Registering %04x..%04x ops @%p (data %p) %s\n",
	      start, start + count - 1, ops, data,
	      ops->interrupt ? "[Internal]" : "[OS]");

	lock(&irq_lock);
	list_for_each(&irq_sources, is1, link) {
		if (is->end > is1->start && is->start < is1->end) {
			prerror("register IRQ source overlap !\n");
			prerror("  new: %x..%x old: %x..%x\n",
				is->start, is->end - 1,
				is1->start, is1->end - 1);
			assert(0);
		}
	}
	list_add_tail(&irq_sources, &is->link);
	unlock(&irq_lock);
}

void unregister_irq_source(uint32_t start, uint32_t count)
{
	struct irq_source *is;

	lock(&irq_lock);
	list_for_each(&irq_sources, is, link) {
		if (start >= is->start && start < is->end) {
			if (start != is->start ||
			    count != (is->end - is->start)) {
				prerror("unregister IRQ source mismatch !\n");
				prerror("start:%x, count: %x match: %x..%x\n",
					start, count, is->start, is->end);
				assert(0);
			}
			list_del(&is->link);
			unlock(&irq_lock);
			/* XXX Add synchronize / RCU */
			free(is);
			return;
		}
	}
	unlock(&irq_lock);
	prerror("unregister IRQ source not found !\n");
	prerror("start:%x, count: %x\n", start, count);
	assert(0);
}

/*
 * This takes a 6-bit chip id and returns a 20 bit value representing
 * the PSI interrupt. This includes all the fields above, ie, is a
 * global interrupt number.
 *
 * For P8, this returns the base of the 8-interrupts block for PSI
 */
uint32_t get_psi_interrupt(uint32_t chip_id)
{
	uint32_t irq;

	switch(proc_gen) {
	case proc_gen_p7:
		/* Get the chip ID into position, it already has
		 * the T bit so all we need is room for the GX
		 * bit, 9 bit BUID and 4 bit level
		 */
		irq  = chip_id << (1 + 9 + 4);

		/* Add in the BUID */
		irq |= P7_PSI_IRQ_BUID << 4;
		break;
	case proc_gen_p8:
		irq = P8_CHIP_IRQ_BLOCK_BASE(chip_id, P8_IRQ_BLOCK_MISC);
		irq += P8_IRQ_MISC_PSI_BASE;
		break;
	default:
		assert(false);
	};

	return irq;
}


struct dt_node *add_ics_node(void)
{
	struct dt_node *ics = dt_new_addr(dt_root, "interrupt-controller", 0);
	if (!ics)
		return NULL;

	dt_add_property_cells(ics, "reg", 0, 0, 0, 0);
	dt_add_property_strings(ics, "compatible", "IBM,ppc-xics",
				"IBM,opal-xics");
	dt_add_property_cells(ics, "#address-cells", 0);
	dt_add_property_cells(ics, "#interrupt-cells", 1);
	dt_add_property_string(ics, "device_type",
			       "PowerPC-Interrupt-Source-Controller");
	dt_add_property(ics, "interrupt-controller", NULL, 0);

	return ics;
}

uint32_t get_ics_phandle(void)
{
	struct dt_node *i;

	for (i = dt_first(dt_root); i; i = dt_next(dt_root, i)) {
		if (streq(i->name, "interrupt-controller@0")) {
			return i->phandle;
		}
	}
	abort();
}

void add_opal_interrupts(void)
{
	struct irq_source *is;
	unsigned int i, count = 0;
	uint32_t *irqs = NULL, isn;

	lock(&irq_lock);
	list_for_each(&irq_sources, is, link) {
		/*
		 * Add a source to opal-interrupts if it has an
		 * ->interrupt callback
		 */
		if (!is->ops->interrupt)
			continue;
		for (isn = is->start; isn < is->end; isn++) {
			i = count++;
			irqs = realloc(irqs, 4 * count);
			irqs[i] = isn;
		}
	}
	unlock(&irq_lock);

	/* The opal-interrupts property has one cell per interrupt,
	 * it is not a standard interrupt property.
	 *
	 * Note: Even if empty, create it, otherwise some bogus error
	 * handling in Linux can cause problems.
	 */
	dt_add_property(opal_node, "opal-interrupts", irqs, count * 4);
}

/*
 * This is called at init time (and one fast reboot) to sanitize the
 * ICP. We set our priority to 0 to mask all interrupts and make sure
 * no IPI is on the way.
 */
void reset_cpu_icp(void)
{
	void *icp = this_cpu()->icp_regs;

	assert(icp);

	/* Clear pending IPIs */
	out_8(icp + ICP_MFRR, 0xff);

	/* Set priority to max, ignore all incoming interrupts, EOI IPIs */
	out_be32(icp + ICP_XIRR, 2);
}

/* Used by the PSI code to send an EOI during reset. This will also
 * set the CPPR to 0 which should already be the case anyway
 */
void icp_send_eoi(uint32_t interrupt)
{
	void *icp = this_cpu()->icp_regs;

	assert(icp);

	/* Set priority to max, ignore all incoming interrupts */
	out_be32(icp + ICP_XIRR, interrupt & 0xffffff);
}

/* This is called before winkle, we clear pending IPIs and set our priority
 * to 1 to mask all but the IPI
 */
void icp_prep_for_rvwinkle(void)
{
	void *icp = this_cpu()->icp_regs;

	assert(icp);

	/* Clear pending IPIs */
	out_8(icp + ICP_MFRR, 0xff);

	/* Set priority to 1, ignore all incoming interrupts, EOI IPIs */
	out_be32(icp + ICP_XIRR, 0x01000002);
}

/* This is called to wakeup somebody from winkle */
void icp_kick_cpu(struct cpu_thread *cpu)
{
	void *icp = cpu->icp_regs;

	assert(icp);

	/* Send high priority IPI */
	out_8(icp + ICP_MFRR, 0);
}

static struct irq_source *irq_find_source(uint32_t isn)
{
	struct irq_source *is;

	lock(&irq_lock);
	list_for_each(&irq_sources, is, link) {
		if (isn >= is->start && isn < is->end) {
			unlock(&irq_lock);
			return is;
		}
	}
	unlock(&irq_lock);

	return NULL;
}

static int64_t opal_set_xive(uint32_t isn, uint16_t server, uint8_t priority)
{
	struct irq_source *is = irq_find_source(isn);

	if (!is || !is->ops->set_xive)
		return OPAL_PARAMETER;

	return is->ops->set_xive(is->data, isn, server, priority);
}
opal_call(OPAL_SET_XIVE, opal_set_xive, 3);

static int64_t opal_get_xive(uint32_t isn, uint16_t *server, uint8_t *priority)
{
	struct irq_source *is = irq_find_source(isn);

	if (!is || !is->ops->get_xive)
		return OPAL_PARAMETER;

	return is->ops->get_xive(is->data, isn, server, priority);
}
opal_call(OPAL_GET_XIVE, opal_get_xive, 3);

static int64_t opal_handle_interrupt(uint32_t isn, uint64_t *outstanding_event_mask)
{
	struct irq_source *is = irq_find_source(isn);
	int64_t rc = OPAL_SUCCESS;

	/* We run the timers first */
	check_timers(true);

	/* No source ? return */
	if (!is || !is->ops->interrupt) {
		rc = OPAL_PARAMETER;
		goto bail;
	}

	/* Run it */
	is->ops->interrupt(is->data, isn);

	/* Update output events */
 bail:
	if (outstanding_event_mask)
		*outstanding_event_mask = opal_pending_events;

	return rc;
}
opal_call(OPAL_HANDLE_INTERRUPT, opal_handle_interrupt, 2);

void init_interrupts(void)
{
	struct dt_node *icp;
	const struct dt_property *sranges;
	struct cpu_thread *cpu;
	u32 base, count, i;
	u64 addr, size;

	dt_for_each_compatible(dt_root, icp, "ibm,ppc-xicp") {
		sranges = dt_require_property(icp,
					      "ibm,interrupt-server-ranges",
					      -1);
		base = dt_get_number(sranges->prop, 1);
		count = dt_get_number(sranges->prop + 4, 1);
		for (i = 0; i < count; i++) {
			addr = dt_get_address(icp, i, &size);
			cpu = find_cpu_by_server(base + i);
			if (cpu)
				cpu->icp_regs = (void *)addr;
		}
	}
}
