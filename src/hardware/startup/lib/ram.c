/*
 * $QNXLicenseC:
 * Copyright 2008, QNX Software Systems. 
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"). You 
 * may not reproduce, modify or distribute this software except in 
 * compliance with the License. You may obtain a copy of the License 
 * at: http://www.apache.org/licenses/LICENSE-2.0 
 * 
 * Unless required by applicable law or agreed to in writing, software 
 * distributed under the License is distributed on an "AS IS" basis, 
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied.
 *
 * This file may contain contributions from others, either as 
 * contributors under the License or as licensors under other terms.  
 * Please review this entire file for other proprietary rights or license 
 * notices, as well as the QNX Development Suite License Guide at 
 * http://licensing.qnx.com/license-guide/ for other information.
 * $
 */





#include "startup.h"

struct ram_entry {
	paddr_t		addr;
	paddr_t		size;
};

struct avoid_entry {
	paddr32_t	start;
	paddr32_t	end;
};

struct watch_entry {
	struct watch_entry	*next;
	void				(*watcher)(paddr_t, paddr_t);
};

static int					sysram_added;
static paddr32_t			ws_early_next;
static paddr32_t			temp_syspage = NULL_PADDR32;
static struct avoid_entry	*avoid_list;
static unsigned				avoid_list_size;
static struct ram_entry		*ram_list;
static unsigned				ram_list_size;
static struct watch_entry	*watch_list;

#define TEMP_SYSPAGE_SIZE (ROUNDPG(KILO(32)))
#define MAX_RAM_LIST_SIZE	KILO(3)
#define MAX_AVOID_LIST_SIZE	KILO(1)

static void
ws_init(void) {
	// We can be fairly sure there's some ram following :-).
	ws_early_next = ROUND(shdr->ram_paddr + shdr->ram_size, sizeof(uint64_t));

	avoid_list = MAKE_1TO1_PTR(ws_early_next);
	avoid_list[0].start = ws_early_next;
	ws_early_next += ROUND(MAX_AVOID_LIST_SIZE, sizeof(uint64_t));
	avoid_list[0].end = ws_early_next;
	avoid_list[1].end = 0;
	avoid_list_size = sizeof(avoid_list[0])*2;

	ram_list = ws_alloc(MAX_RAM_LIST_SIZE);
	ram_list->size = 0;
	ram_list_size = sizeof(struct ram_entry);

	//Avoid using memory covered by startup
	avoid_ram(shdr->ram_paddr, shdr->startup_size);

#ifdef CPU_MEM_AVOID_START
	//Avoid using memory that might have something interesting
	avoid_ram(CPU_MEM_AVOID_START, CPU_MEM_AVOID_SIZE);
#endif
}


static int
add_mem(paddr_t start, paddr_t size) {
	paddr_t 			 e, s;
	struct ram_entry	*r;
	struct ram_entry	*ram = ram_list;
	
//kprintf( "add_mem(%L,%L)\n", (paddr64_t)start, (paddr64_t)size );
	if(size > 0) {
		for(r = ram; r->size != 0; r++) {
			if(r->addr > start) break;
		}
		if(ram_list_size > (MAX_RAM_LIST_SIZE - sizeof(*r))) {
			crash("RAM entry array overflow");
		}
		memmove(&r[1], r, ram_list_size - PTR_DIFF(r, ram));
		ram_list_size += sizeof(*r);
		r->addr = start;
		r->size = size;
		for(r = ram+1; r->size != 0; r++) {
			e = r[-1].addr + r[-1].size;
			s = r->addr;
			if(s <= e) {
				if((s + r->size) > e) e = s + r->size;
				memmove(r, &r[1], ram_list_size - PTR_DIFF(&r[1], ram));
				ram_list_size -= sizeof(*r);
				--r;
				r->size = e - r->addr;
			}
		}
	}
	return 0;
}

static int
fix_size(paddr_t *sp, paddr_t *ep, paddr_t as, paddr_t ae) {
	paddr_t		s = *sp;
	paddr_t		e = *ep;

	if(s >= ae) return 1;
	if(e <= as) return 1;
	if(s >= as) {
		if(e <= ae) return 0;
		*sp = ae;
	} else if(e < ae) {
		*ep = as;
	} else if((as-s) > (e-ae)) {
		*ep = as;
	} else {
		*sp = ae;
	}
	return 1;
}

paddr_t
find_top_ram_aligned(paddr_t size, unsigned align) {
	struct ram_entry		*r;
	struct avoid_entry		*a;
	paddr_t 				s;
	paddr_t 				e;
	paddr_t					chk;
	paddr_t					top = 0;

	size = ROUND(size, lsp.system_private.p->pagesize);
	for(r = ram_list; r->size != 0; r++) {
		s = r->addr;
		e = s + r->size;
		for(a = avoid_list; a->end != 0; ++a) {
			if(!fix_size(&s, &e, a->start, a->end)) goto next_one;
		}
		// Find an address at the end of the ram block.  If align is given,
		// truncate to the alignment value. 
		if ( (e-s) >= size ) {
			chk = align ? TRUNC(e-size, align) : e-size;
			if (chk >= s) {
				if(chk > top) top = chk;
			}
		}
next_one: ;
	}
	if(top == 0) top = NULL_PADDR;
	return top;
}

paddr_t
find_top_ram(paddr_t size) {
	return find_top_ram_aligned(size, 0);
}

paddr32_t
find_ram(size_t size, unsigned align, unsigned colour, unsigned mask) {
	struct ram_entry		*ram = ram_list;
	struct ram_entry		*r;
	paddr_t 				s;
	paddr_t 				e;
	paddr_t					addr;
	struct avoid_entry		*a;

	//NYI: should make sure memory comes out of the 1-to-1 mapping region
	for(r = ram; r->size != 0; r++) {
		s = r->addr;
		e = s + r->size;
		for(a = avoid_list; a->end != 0; ++a) {
			if(!fix_size(&s, &e, a->start, a->end)) goto next_one;
		}
		addr = ROUND(s, align);
		if((addr & mask) != colour) {
			unsigned	incr;

			incr = ((mask - 1) | mask) + 1;
			addr = ((addr + incr) & ~mask) | colour;
		}
		if(addr >= e) continue;
		if(size <= (e - addr)) return addr;
next_one: ;
	}
	return NULL_PADDR32;
}
		
paddr_t
alloc_ram(paddr_t addr, paddr_t size, unsigned align) {
	struct ram_entry	*ram = ram_list;
	struct ram_entry	*r;
	paddr_t 			s, e, end;
	
//kprintf("alloc_ram(%L,%L,%l) => ", (paddr64_t)addr, (paddr64_t)size, align);
	if(addr == NULL_PADDR) {
		addr = find_ram(size, align, 0, 0);
		if(addr == NULL_PADDR32) {
//kprintf("NULL_PADDR\n");
			return NULL_PADDR;
		}
	}
//kprintf("%L\n", (paddr64_t)addr);
	end = addr + size;
	for(r = ram; r->size != 0; r++) {
		s = r->addr;
		e = s + r->size;
		if((addr <= s && end > s)
		|| (addr >= s && addr < e)
		|| (end  >= s && end  < e)) {
			if(end  > e) e=end;
			if(addr < s) s=addr;
			if(addr == s && end == e) {
				memmove(r, &r[1], ram_list_size - PTR_DIFF(&r[1], ram));
				ram_list_size -= sizeof(*r);
				--r;
			} else if(addr == s) {
				r->size = e - end;
				r->addr = end;
			} else {
				r->size = addr - s;
				if(e != end) {
					add_mem(end, e - end);
				}
			}
		}
	}

	//
	// If add_sysram() has been called, we need to adjust the start/end
	// fields
	//

	if(sysram_added) {
		unsigned			off;
		struct asinfo_entry	*as;

		--end;
		off = AS_NULL_OFF;
		for( ;; ) {
			off = as_find_containing(off, addr, end, "sysram");
			if(off == AS_NULL_OFF) break;
			as = as_off2info(off);
			s = as->start;
			e = as->end;
			if(end  > e) e=end;
			if(addr < s) s=addr;
			if(addr == s && end == e) {
				crash("Can't remove whole sysram entry.\n");
			} else if(addr == s) {
				as->start = end + 1;
			} else {
				as->end = addr - 1;
				if(e != end) {
					as_add(end+1, e, AS_ATTR_RAM, "sysram", as->owner);
				}
			}
		}
	}
	return addr;
}

paddr32_t
calloc_ram(size_t size, unsigned align) {
	paddr32_t a;
	
	a = alloc_ram(NULL_PADDR, size, align);
	if(a != NULL_PADDR32) {
		memset(MAKE_1TO1_PTR(a), 0, size);
	}
	return a;
}

void
add_ram(paddr_t start, paddr_t size) {
	struct watch_entry	*curr;

	if(lsp.asinfo.size == 0) {
		init_asinfo(as_default());
	}
	as_add_containing(start, start + size - 1, AS_ATTR_RAM, "ram", "memory");

	add_mem(start, size);

	//Avoid using memory covered by image
	alloc_ram(shdr->ram_paddr+shdr->startup_size,
				shdr->ram_size-shdr->startup_size, 1);

	//Avoid using memory reserved for the CPU (exception table)
	#if defined(CPU_MEM_RESERVE_SIZE) && (CPU_MEM_RESERVE_SIZE > 0)
	   alloc_ram(CPU_MEM_RESERVE_START, CPU_MEM_RESERVE_SIZE, 1);
	#endif

	//Get more space for the temporary system page we're building.
	if(temp_syspage == NULL_PADDR32) {
		temp_syspage = find_ram(TEMP_SYSPAGE_SIZE, __PAGESIZE, 0, 0);
		if(temp_syspage != NULL_PADDR32) {
			avoid_ram(temp_syspage, TEMP_SYSPAGE_SIZE);
			reloc_syspage_memory(MAKE_1TO1_PTR(temp_syspage), TEMP_SYSPAGE_SIZE);
		}
	}

	for(curr = watch_list; curr != NULL; curr = curr->next) {
		curr->watcher(start, size);
	}
}

void
avoid_ram(paddr32_t start, size_t size) {
	struct avoid_entry	*avoid;

	if(avoid_list == NULL) {
		ws_init();
	}
	avoid = (void *)((uint8_t *)avoid_list + avoid_list_size);

	avoid[-1].start = start;
	avoid[-1].end = start + size;
	avoid[0].end = 0;
	avoid_list_size += sizeof(*avoid);
}

//
// Add all the "sysram" entries to the asinfo section. Note that we haven't
// allocated the system page storage yet, so we're going to have adjust
// the start/end fields when that happens.
//
void
add_sysram() {
	struct ram_entry	*ram;
	struct watch_entry	*curr;

	// Tell all the watchers that there won't be any more RAM added
	for(curr = watch_list; curr != NULL; curr = curr->next) {
		curr->watcher(0, 0);
	}

	for(ram = ram_list; ram->size != 0; ++ram) {
		as_add_containing(ram->addr, ram->addr+ram->size-1,
				AS_ATTR_RAM, "sysram", "ram");
	}
	sysram_added = 1;
}


void *
ws_alloc(size_t size) {
	paddr32_t	paddr;

	if(avoid_list == NULL) {
		ws_init();
	}

	size = ROUND(size, sizeof(uint64_t));
	if(ram_list_size <= sizeof(struct ram_entry)) {
		// no add_ram()'s have been done yet.
		paddr = ws_early_next;
		ws_early_next += size;
	} else {
		// we've been told where memory is.
		paddr = find_ram(size, sizeof(uint64_t), 0, 0);
		if(paddr == NULL_PADDR32) {
			return(NULL);
		}
	}
	avoid_ram(paddr, size);
	return(MAKE_1TO1_PTR(paddr));
}


void
watch_add_ram(void (*func)(paddr_t, paddr_t)) {
	struct watch_entry	*new;

	new = ws_alloc(sizeof(*new));
	if(new == NULL) crash("No memory for watcher");
	new->watcher = func;
	new->next = watch_list;
	watch_list = new;
}

__SRCVERSION("ram.c $Rev: 262077 $");
