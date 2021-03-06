/*
 *  libpulp - User-space Livepatching Library
 *
 *  Copyright (C) 2017-2020 SUSE Software Solutions GmbH
 *
 *  This file is part of libpulp.
 *
 *  libpulp is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  libpulp is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libpulp.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "config.h"
#include "ulp.h"

/* ulp data structures */
struct ulp_patching_state __ulp_state = {0, NULL};
char __ulp_path_buffer[256] = "";
struct ulp_metadata *__ulp_metadata_ref = NULL;
struct ulp_detour_root *__ulp_root = NULL;

// push		function_index
// jmpq		0x0(%rip)
// <data>	&__ulp_manage_addresses
//char ulp_prologue[21] = {0x68, 0x00, 0x00, 0x00, 0x00,
//                         0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
//                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
//                         0xeb, - (PRE_NOPS_LEN + 2)};

// push		%rdi
// movq		$0xindex, %rdi
// jmp1		0x0(%rip)
// <data>	&__ulp_prologue
char ulp_prologue[24] = {0x57,
                         0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x00,
                         0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                         0xeb, - (PRE_NOPS_LEN + 2)};

unsigned int __ulp_root_index_counter = 0;
unsigned long __ulp_global_universe = 0;

extern void __ulp_prologue();

__attribute__ ((constructor)) void begin(void)
{
    __ulp_state.load_state = 1;
    fprintf(stderr, "libpulp loaded...\n");
}

static unsigned long return_zero()
{
    return 0;
}

/* libpulp interfaces for livepatch trigger */
int __ulp_apply_patch()
{
    if (!load_patch()) {
	WARN("Patch not applied");
	return 0;
    }
    return 1;
}

void __ulp_print()
{
    fprintf(stderr, "ULP DEBUG PRINT MSG\n");
}

void * __ulp_get_path_buffer_addr()
{
    return &__ulp_path_buffer;
}

int __ulp_check_applied_patch()
{
    struct ulp_applied_patch *patch;

    patch = ulp_get_applied_patch(__ulp_metadata_ref->patch_id);
    if (patch) return 1;
    else return 0;
}

unsigned long __ulp_get_global_universe_value()
{
    return __ulp_global_universe;
}

/*
 * Checks that all locks in the implementation of malloc and dlopen are
 * free. In order to do so, it makes calls into __libpulp_malloc_checks
 * and __libpulp_dlopen_check, which are not part of upstream glibc.
 *
 * A hijacked process uses this to determine if it can make calls to
 * calloc and dlopen (AS-Unsafe functions) from a signal handler. When
 * none of the locks are taken, the hijacked process may make calls to
 * calloc and dlopen without the risk to go into a deadlock.
 */
int __libpulp_malloc_checks(void);
int __libpulp_dlopen_checks(void);
int __ulp_do_testlocks()
{
    int malloc_locks;
    int dlopen_locks;

    malloc_locks = 0;
    dlopen_locks = 0;

#ifdef HAVE___LIBPULP_MALLOC_CHECKS
    malloc_locks = __libpulp_malloc_checks();
    if (malloc_locks != 0 && malloc_locks != 1) {
      WARN("FATAL: Invalid return from __libpulp_malloc_checks().");
      return -1;
    }
#endif

#ifdef HAVE___LIBPULP_DLOPEN_CHECKS
    dlopen_locks = __libpulp_dlopen_checks();
    if (dlopen_locks != 0 && dlopen_locks != 1) {
      WARN("FATAL: Invalid return from __libpulp_dlopen_checks().");
      return -1;
    }
#endif

    if (malloc_locks || dlopen_locks)
      return EAGAIN;
    return 0;
}

/* libpulp functions */
void free_metadata(struct ulp_metadata *ulp)
{
    struct ulp_unit *unit, *next_unit;
    struct ulp_object *obj;

    if (ulp) {
	obj = ulp->objs;
        if (obj) {
  	    unit = ulp->objs->units;
	    while (unit) {
                next_unit = unit->next;
                free(unit->old_fname);
                free(unit->new_fname);
                free(unit);
                unit = next_unit;
	    }
            free(obj->build_id);
            if (obj->dl_handler && dlclose(obj->dl_handler)) {
	        WARN("Error closing handler for %s.", obj->name);
            }
            free(obj->name);
            free(obj);
        }
    }
    free(ulp);
}

/* TODO: unloading needs further testing */
int unload_handlers(struct ulp_metadata *ulp)
{
    struct ulp_object *obj;
    int status = 1;
    if(ulp->so_handler && dlclose(ulp->so_handler)) {
	WARN("Error unloading patch so handler: %s", ulp->so_filename);
	status = 0;
    }
    obj = ulp->objs;
    if(obj->dl_handler && dlclose(obj->dl_handler)) {
	WARN("Error unloading patch target so handler: %s", obj->name);
	status = 0;
    }
    return status;
}

void *get_fentry_from_ulp(void *func)
{
    /* The jump slots in the .ulp section have the address of the
     * destination function hardcoded as a displacement from %rip.
     *
     * The displacement is the 4-byte immediate field in the lea
     * instruction (first instruction in each jump-slot), which requires
     * a 32-bits variable (int32_t).  The immediate field begins at the
     * forth byte in the instruction, hence the addition of 3 bytes to
     * the address where memcpy begins copying from.  Finally, lea
     * itself is 7-bytes long, hence the subtraction of 7 bytes after
     * the load.  */
    void *address;
    int32_t offset = 0;
    memcpy(&offset, func + 3, 4);
    address = func + offset + 7;
    return address;
}

// trm: should be set to take real function address out of .ulp section
void *load_so_symbol(char *fname, void *handle, int trm)
{
    void *func;
    char *error;

    func = dlsym(handle, fname);
    error = dlerror();
    if (error) {
	WARN("Unable to load function %s: %s.", fname, error);
	return NULL;
    }

    if (trm) func = get_fentry_from_ulp(func);

    return func;
}

int load_so_handlers(struct ulp_metadata *ulp)
{
    struct ulp_object *obj;
    ulp->so_handler = load_so(ulp->so_filename);

    if (!ulp->so_handler) {
	WARN("Unable to load patch dl handler.");
	unload_handlers(ulp);
	return 0;
    }

    obj = ulp->objs;
    obj->dl_handler = load_so(obj->name);
    if (!obj->dl_handler) {
	WARN("Unable to load dl handler.");
	unload_handlers(ulp);
	return 0;
    }
    return 1;
}

int unload_metadata(struct ulp_metadata *ulp)
{
    free_metadata(ulp);
    __ulp_metadata_ref = NULL;
    return 0;
}

struct ulp_metadata *load_metadata()
{
    struct ulp_metadata *ulp;
    if (__ulp_metadata_ref)
    {
	return __ulp_metadata_ref;
    }

    ulp = calloc(1, sizeof(struct ulp_metadata));
    if (!ulp) {
	WARN("Unable to allocate memory for ulp metadata");
	return 0;
    }

    __ulp_metadata_ref = ulp;
    if (!parse_metadata(ulp))
    {
	WARN("Error parsing metadata.\n");
	return 0;
    };

    return ulp;
}

int read_data(int from, void *to, size_t count)
{
  size_t done;
  ssize_t ret;

  for (done = 0;;) {
    errno = 0;
    ret = read(from, to + done, count - done);
    if (ret == 0)
      break; /* EOF or read called with count set to zero. */
    else if (ret > 0) {
      done += ret;
      if (done == count)
        break; /* Done. */\
      else
        continue; /* More to read. */
    }
    else if (errno == EINTR || errno == EAGAIN) {
      continue; /* Try again. */
    }
    else {
      WARN("Error in call to read()");
      return 1;
    }
  }
  if (done != count) {
    WARN("Not enough data to read()");
    return 1;
  }

  return 0;
}

int parse_metadata(struct ulp_metadata *ulp)
{
    int fd;
    uint32_t c;
    uint32_t i, j;
    struct ulp_object *obj;
    struct ulp_unit *unit, *prev_unit = NULL;
    struct ulp_dependency *dep, *prev_dep = NULL;

    fd = open(__ulp_path_buffer, O_RDONLY);
    if (fd == -1) {
	WARN("Unable to open metadata file: %s.", __ulp_path_buffer);
	return 0;
    }

    ulp->objs = NULL;

#define READ(from, to, count) \
  if (read_data(from, to, count)) \
    return 0;

    /* read metadata header information */
    READ (fd, &ulp->type, 1 * sizeof(uint8_t));
    READ (fd, &ulp->patch_id, 32 * sizeof(char));

    /* if this is a patch revert, we don't need extra information */
    if (ulp->type == 2) return 2;

    /* read livepatch DSO filename */
    READ (fd, &c, 1 * sizeof(uint32_t));
    ulp->so_filename = calloc(c + 1, sizeof(char));
    if (!ulp->so_filename) {
	WARN("Unable to allocate so filename buffer.");
	return 0;
    }
    READ (fd, ulp->so_filename, c * sizeof(char));

    obj = calloc(1, sizeof(struct ulp_object));
    if (!obj) {
	WARN("Unable to allocate memory for the patch objects.");
	return 0;
    }
    obj->units = NULL;

    /* read the length of the target library's build-id */
    READ (fd, &c, 1 * sizeof(uint32_t));
    obj->build_id_len = c;

    /* read the build-id of the target library */
    obj->build_id = calloc(c, sizeof(char));
    if (!obj->build_id) {
	WARN("Unable to allocate build id buffer.");
	return 0;
    }
    READ (fd, obj->build_id, c * sizeof(char));
    obj->build_id_check = 0;

    /* read the length of the target library's name */
    READ (fd, &c, 1 * sizeof(uint32_t));

    ulp->objs = obj;

    /* shared object: fill data + read patching units */
    obj->name = calloc(c + 1, sizeof(char));
    if (!obj->name) {
	WARN("Unable to allocate object name buffer.");
	return 0;
    }
    READ (fd, obj->name, c * sizeof(char));

    /* read the number of patching units */
    READ (fd, &obj->nunits, 1 * sizeof(uint32_t));

    /* read all patching units for object */
    for (j = 0; j < obj->nunits; j++) {
	unit = calloc(1, sizeof(struct ulp_unit));
	if (!unit) {
	    WARN("Unable to allocate memory for the patch units.");
	    return 0;
	}

	/* read the name of the old function in this unit */
	READ (fd, &c, 1 * sizeof(uint32_t));
	unit->old_fname = calloc(c + 1, sizeof(char));
	if (!unit->old_fname) {
	    WARN("Unable to allocate unit old function name buffer.");
	    return 0;
	}
	READ (fd, unit->old_fname, c * sizeof(char));

	/* read the name of the new function in this unit */
	READ (fd, &c, 1 * sizeof(uint32_t));
	unit->new_fname = calloc(c + 1, sizeof(char));
	if (!unit->new_fname) {
	    WARN("Unable to allocate unit new function name buffer.");
	    return 0;
	}
	READ (fd, unit->new_fname, c * sizeof(char));

	/* read the address of the old function in this unit */
	READ (fd, &unit->old_faddr, 1 * sizeof(void *));

	/* update the list of units */
	if (obj->units) {
	    prev_unit->next = unit;
	} else {
	    obj->units = unit;
	}
	prev_unit = unit;
    }

    /* read number of dependencies */
    READ (fd, &c, 1 * sizeof(uint32_t));

    /* read all dependencies */
    for (i = 0; i < c; i++) {
	dep = calloc(1, sizeof(struct ulp_dependency));
	if (!dep) {
	    WARN("Unable to allocate memory for dependency state.");
	    return 0;
	}
	READ (fd, &dep->dep_id, 32 * sizeof(char));
	if (ulp->deps) {
	    prev_dep->next = dep;
	} else {
	    ulp->deps = dep;
	}
	prev_dep = dep;
    }
#undef READ

    close(fd);
    if(!check_patch_sanity(ulp)) return 0;
    if(!load_so_handlers(ulp)) return 0;

    return 1;
}

void *load_so(char *obj)
{
    void *patch_obj;

    patch_obj = dlopen(obj, RTLD_LAZY);
    if (!patch_obj) {
	WARN("Unable to load shared object %s: %s.", obj, dlerror());
	return NULL;
    }

    return patch_obj;
}

int load_patch()
{
    struct ulp_metadata *ulp = NULL;
    struct ulp_applied_patch *patch_entry;
    int patch;

    ulp = load_metadata();
    patch = ulp->type;

    switch (patch) {
	case 1:   /* apply patch */
	    patch_entry = ulp_state_update(ulp);
	    if (!patch_entry)
		break;

	    if (!ulp_apply_all_units(ulp)) {
                WARN("FATAL ERROR while applying patch units\n");
                exit(-1);
	    }

	    goto load_patch_success;

	case 2: /* revert patch */
	    if (!ulp_can_revert_patch(ulp))
		break;
	    if (!ulp_revert_patch(ulp->patch_id)) {
		WARN("Unable to revert patch.");
		break;
	    }

	    goto load_patch_success;

	default:  /* load patch metadata error */
	    if (!patch) WARN("load patch metadata error");
	    else WARN("Unknown load metadata status");
    }
    patch = 0;

load_patch_success:
    unload_metadata(ulp);
    return patch;
}

int ulp_can_revert_patch(struct ulp_metadata *ulp)
{
    unsigned char *id = ulp->patch_id;
    int i;
    struct ulp_applied_patch *patch, *applied_patch;
    struct ulp_dependency *dep;

    /* check if patch exists */
    applied_patch = ulp_get_applied_patch(id);
    if (!applied_patch) {
	WARN("Can't revert because patch was not applied");
	return 0;
    }

    /* check if someone depends on the patch */
    for (patch = __ulp_state.patches; patch != NULL; patch = patch->next) {
	for (dep = patch->deps; dep != NULL; dep = dep->next) {
	    if (memcmp(dep->dep_id, id, 32) == 0) {
		fprintf(stderr, "Can't revert. Dependency:\n   PATCH 0x");
		for (i = 0; i < 32; i++) {
		    fprintf(stderr, "%x ", id[i]);
		}
		fprintf(stderr, "\n");
		return 0;
	    }
	}
    }

    return 1;
}

struct ulp_detour_root *get_detour_root_by_index(unsigned int idx)
{
    struct ulp_detour_root *r;
    r = __ulp_root;

    if (r == NULL) return NULL;
    for (r = __ulp_root; r != NULL && r->index != idx; r = r->next) {};

    return r;
}

struct ulp_detour_root *get_detour_root_by_address(void *addr)
{
    struct ulp_detour_root *r;
    r = __ulp_root;

    if (r == NULL) return NULL;
    for (r = __ulp_root; r != NULL && r->patched_addr != addr; r = r->next) {};

    return r;
}

struct ulp_detour_root *push_new_root()
{
    struct ulp_detour_root *root, *root_aux;

    root = calloc(1, sizeof(struct ulp_detour_root));
    if (!root) {
	WARN("unable to allocate memory for ulp detour root");
	return NULL;
    }

    // since above we use calloc, the if/else below shouldn't be needed
    if (!__ulp_root) root_aux = NULL;
    else root_aux = __ulp_root;
    __ulp_root = root;
    root->next = root_aux;

    return root;
}

int ulp_apply_all_units(struct ulp_metadata *ulp)
{
    void *old_fun, *new_fun;
    void *patch_so = ulp->so_handler;
    struct ulp_object *obj = ulp->objs;
    struct ulp_unit *unit;
    struct ulp_detour_root *root;

    __ulp_global_universe++;

    /* only shared objs have units, this loop never runs for main obj */
    unit = obj->units;
    while (unit) {
	old_fun = load_so_symbol(unit->old_fname, obj->dl_handler, 1);
	if (!old_fun) return 0;

	new_fun = load_so_symbol(unit->new_fname, patch_so, 0);
	if (!new_fun) return 0;

        root = get_detour_root_by_address(old_fun);
        if (!root) {
            root = push_new_root();
            if (!root) return 0;

            root->index = get_next_function_index();
            root->patched_addr = old_fun;
            root->handler = obj->dl_handler;
            root->get_local_universe =
                dlsym(root->handler, "__ulp_ret_local_universe");
            if (!root->get_local_universe)
                root->get_local_universe = return_zero;
        }

        if (!(push_new_detour(__ulp_global_universe, ulp->patch_id,
                              root, new_fun)))
        {
            WARN("error setting ulp data structure\n");
            return 0;
        }

        if (!(ulp_patch_addr(old_fun, root->index)))
        {
            WARN("error patching address %p", old_fun);
            return 0;
        }

	unit = unit->next;
    }

    return 1;
}

struct ulp_applied_patch *ulp_state_update(struct ulp_metadata *ulp)
{
    struct ulp_applied_patch *a_patch, *prev_patch = NULL;
    struct ulp_applied_unit *a_unit, *prev_unit = NULL;
    struct ulp_object *obj;
    struct ulp_unit *unit;
    struct ulp_dependency *dep, *a_dep;

    a_patch = calloc(1, sizeof(struct ulp_applied_patch));
    if (!a_patch) {
	WARN("Unable to allocate memory to update ulp state.");
	return 0;
    }
    memcpy(a_patch->patch_id, ulp->patch_id, 32);

    for (dep = ulp->deps; dep != NULL; dep = dep->next) {
	a_dep = calloc(1, sizeof(struct ulp_dependency));
	if (!a_dep) {
	    WARN("Unable to allocate memory to ulp state dependency.");
	    return 0;
	}

	*a_dep = *dep;
	a_dep->next = a_patch->deps;
	a_patch->deps = a_dep;
    }

    obj = ulp->objs;
    unit = obj->units;

    /* only shared objs have units, this loop never runs for main obj */
    while (unit != NULL) {
	a_unit = calloc(1, sizeof(struct ulp_applied_unit));
	if (!a_unit) {
	    WARN("Unable to allocate memory to update ulp state (unit).");
	    return 0;
	}

	a_unit->patched_addr = load_so_symbol(unit->old_fname,
		obj->dl_handler, 1);
	if (!a_unit->patched_addr) return 0;

	a_unit->target_addr = load_so_symbol(unit->new_fname,
		ulp->so_handler, 0);
	if (!a_unit->target_addr) return 0;

	memcpy(a_unit->overwritten_bytes, a_unit->patched_addr, 14);

	if (a_patch->units == NULL) {
	    a_patch->units = a_unit;
	    prev_unit = a_unit;
	} else {
	    prev_unit->next = a_unit;
	    prev_unit = a_unit;
	}
	unit = unit->next;
    }

    /* leave last on top of list to optmize revert */
    prev_patch = __ulp_state.patches;
    __ulp_state.patches = a_patch;
    a_patch->next = prev_patch;

    return a_patch;
}

/*
 * Assumes that TGT_ADDR points to the start of the nop padding area of
 * live patchable functions, then sets the memory protection bits of the
 * pages containing that area to allow writes to them. The nop padding
 * area may span multiple pages.
 */
int set_write_tgt(void *tgt_addr)
{
    unsigned long page_size, page_offset;
    void *page_start;

    page_size = getpagesize();
    page_offset = (unsigned long) tgt_addr % page_size;
    page_start = tgt_addr - page_offset;

    /*
     * From the start of the page containing the starting address of the
     * nop padding area, up to the last byte of the padding area, allow
     * writes to the pages containing the range.
     *
     * NOTE: mprotect implicitly handles page crosses.
     */
    if (mprotect(page_start, page_offset+ULP_NOPS_LEN,
		 PROT_WRITE | PROT_EXEC)) {
	WARN("Memory protection set +w error");
	return 0;
    }

    return 1;
}

/*
 * Assumes that TGT_ADDR points to the start of the nop padding area of
 * live patchable functions, then sets the memory protection bits of the
 * pages containing that area to allow execution of instruction in them.
 * The nop padding area may span multiple pages.
 */
int set_exec_tgt(void *tgt_addr)
{
    unsigned long page_size, page_offset;
    void *page_start;

    page_size = getpagesize();
    page_offset = (unsigned long) tgt_addr % page_size;
    page_start = tgt_addr - page_offset;

    /*
     * From the start of the page containing the starting address of the
     * nop padding area, up to the last byte of the padding area, allow
     * execution of instructions in the pages containing the range.
     *
     * NOTE: mprotect implicitly handles page crosses.
     */
    if (mprotect(page_start, page_offset+ULP_NOPS_LEN,
		 PROT_READ | PROT_EXEC)) {
	WARN("Memory protection set +x error");
	return 0;
    }

    return 1;
}

int check_patch_sanity(struct ulp_metadata *ulp)
{
    if (!check_build_id(ulp)) return 0;
    if (!check_patch_dependencies(ulp)) return 0;
    if (ulp_get_applied_patch(ulp->patch_id)) {
      WARN("Patch was already applied\n");
      return 0;
    }

    return 1;
}

int check_patch_dependencies(struct ulp_metadata *ulp)
{
    struct ulp_applied_patch *patch;
    struct ulp_dependency *dep;

    for (dep = ulp->deps; dep != NULL; dep = dep->next) {
	for (patch = __ulp_state.patches; patch != NULL; patch = patch->next) {
	    if (memcmp(patch->patch_id, dep->dep_id, 32) == 0) {
		dep->patch_id_check = 1;
		break;
	    }
	}
    }

    for (dep = ulp->deps; dep != NULL; dep = dep->next) {
	if (dep->patch_id_check == 0) {
	    WARN("Patch does not match dependencies.");
	    return 0;
	}
    }
    return 1;
}

int compare_build_ids(struct dl_phdr_info *info,
		      size_t __attribute__ ((unused)) size, void *data)
{
    int i;
    char *note_ptr, *build_id_ptr, *note_sec;
    uint32_t note_type, build_id_len, name_len, sec_size, next = 0;
    struct ulp_metadata *ulp;
    ulp = (struct ulp_metadata *) data;

    /* algorithm goes as follows:
     * 1 - check every object inside ulp_metadata
     * 1.1 - if object is main, match loaded object whose name length is 0
     * 1.2 - else, match ulp object and loaded object names
     * 2 - check every phdr for loaded object and match all PT_NOTE
     * 3 - trespass PT_NOTE searching for NT_GNU_BUILD_ID
     * 3.1 - once found, match contents with ulp object build id
     * 3.2 - if match, mark ulp object as checked
     * 3.3 - do not mark and break dl_iterate by returning 1
     *
     * Algorithm assumes that objects will only have one NT_GNU_BUILD_ID entry
     */

    if (strcmp(ulp->objs->name, info->dlpi_name) != 0) return 0;

    for (i = 0; i < info->dlpi_phnum; i++) {
	if (info->dlpi_phdr[i].p_type != PT_NOTE) continue;

	note_sec = (char *) (info->dlpi_phdr[i].p_vaddr + info->dlpi_addr);
	sec_size = info->dlpi_phdr[i].p_memsz;

	for (note_ptr = note_sec, note_type = (uint32_t) *(note_ptr + 8);
		note_type != NT_GNU_BUILD_ID && note_ptr < note_sec +
		sec_size;
		note_ptr = note_sec + next,
		note_type = (uint32_t) *(note_ptr + 8))
	{

	    build_id_len = (uint32_t) *(note_ptr + 4);
	    name_len = (uint32_t) *note_ptr;

	    /* fix paddings */
	    build_id_len += build_id_len % 4;
	    name_len += name_len % 4;

	    next = next + build_id_len + name_len + 12;
	}

	/* could not fid the build id in the note section, go to next sec */
	if (note_type !=  NT_GNU_BUILD_ID) continue;

	build_id_len = (uint32_t) *(note_ptr + 4);
	build_id_len += build_id_len % 4;
	if (build_id_len != ulp->objs->build_id_len) return 0;

	/* we compute, but currently do not check note names */
	name_len = (uint32_t) *note_ptr;
	name_len += name_len % 4;

	build_id_ptr = note_ptr + 12 + name_len;
	if (memcmp(ulp->objs->build_id, build_id_ptr, build_id_len) == 0) {
	    ulp->objs->build_id_check = 1;
	    return 0;
	} else {
	    return 1;
	}
    }
    return 0;
}

int all_build_ids_checked(struct ulp_metadata *ulp)
{
    if (!ulp->objs->build_id_check) {
	WARN("Could not match patch target build id %s.", ulp->objs->name);
	return 0;
    }
    return 1;
}

int check_build_id(struct ulp_metadata *ulp)
{
    dl_iterate_phdr(compare_build_ids, ulp);
    if (!all_build_ids_checked(ulp)) return 0;
    return 1;
}

void ulp_patch_prologue_layout(void *old_fentry, unsigned int function_index)
{
    memcpy(old_fentry, ulp_prologue, sizeof(ulp_prologue));
    memcpy(old_fentry + 4, &function_index, 4);
}

void __ulp_manage_universes(unsigned long idx)
{
    unsigned long universe;
    struct ulp_detour_root *root;
    struct ulp_detour *d;
    void *target;

    root = get_detour_root_by_index((unsigned int) idx);
    if (!root) {
        WARN("FATAL ERROR While Live Patching.");
        exit(-1);
    }

    target = NULL;

    universe = root->get_local_universe();
    if (universe != 0) {
        // since universes are kept in order, this is a top-down search
        for (d = root->detours; d != NULL; d = d->next) {
            if (d->universe == universe ||
               (d->universe < universe && d->active)) {
                target = d->target_addr;
                break;
            }
        }
    }
    if (!target) target = root->patched_addr + 2;

    asm ("movq %0, %%r11;"
		    :
		    : "r" (target)
		    : );
}

unsigned int get_next_function_index()
{
    return __ulp_root_index_counter++;
}

unsigned int push_new_detour(unsigned long universe, unsigned char *patch_id,
                             struct ulp_detour_root *root, void *new_faddr)
{
    struct ulp_detour *detour, *detour_aux;

    detour = calloc(1, sizeof(struct ulp_detour));
    if (!detour) {
        WARN("Unable to acllocate memory for ulp detour");
        return 0;
    }

    detour_aux = root->detours;
    root->detours = detour;
    detour->next = detour_aux;
    detour->target_addr = new_faddr;
    detour->universe = universe;
    detour->active = 1;
    memcpy(detour->patch_id, patch_id, 32);

    return 1;
}

void ulp_patch_addr_absolute(void *old_fentry, void *manager)
{
    memcpy(old_fentry + 14, &manager, sizeof(void *));
}

int ulp_patch_addr(void *old_faddr, unsigned int index)
{
    void *old_fentry = old_faddr - PRE_NOPS_LEN;
    void *manage;

    manage = &__ulp_prologue;

    if (!set_write_tgt(old_fentry)) return 0;

    ulp_patch_prologue_layout(old_fentry, index);
    ulp_patch_addr_absolute(old_fentry, manage);

    if (!set_exec_tgt(old_fentry)) return 0;

    return 1;
}

struct ulp_applied_patch *ulp_get_applied_patch(unsigned char *id)
{
    struct ulp_applied_patch *patch;

    for (patch = __ulp_state.patches; patch != NULL; patch = patch->next)
	if (memcmp(patch->patch_id, id, 32) == 0) return patch;

    return NULL;
}

int ulp_revert_patch(unsigned char *id)
{
    struct ulp_applied_patch *patch;

    __ulp_global_universe++;
    patch = ulp_get_applied_patch(id);

    if (ulp_revert_all_units(id)) {
	if (!ulp_state_remove(patch)) {
	    WARN("Problem updating state. Program may be inconsistent.");
	    return 0;
	}
    }


    return 1;
}

int ulp_state_remove(struct ulp_applied_patch *rm_patch)
{
    struct ulp_applied_patch *patch;
    struct ulp_applied_unit *unit, *next_unit;
    struct ulp_dependency *dep, *next_dep;
    int found = 0;

    /* take it out from applied patches list */
    if (__ulp_state.patches == rm_patch) {
	found = 1;
	__ulp_state.patches = rm_patch->next;
    } else {
	for (patch = __ulp_state.patches; patch != NULL; patch = patch->next) {
	    if (patch == rm_patch) {
		found = 1;
		patch->next = rm_patch->next;
		break;
	    }
	}
    }

    if (!found) return 0;

    /* free all units from it */
    for (unit = rm_patch->units; unit != NULL; unit = next_unit) {
	next_unit = unit->next;
	free(unit);
    }

    /* free all deps from it */
    for (dep = rm_patch->deps; dep != NULL; dep = next_dep) {
	next_dep = dep->next;
	free(dep);
    }

    /* free it */
    free(rm_patch);

    return 1;
}

int ulp_revert_all_units(unsigned char *patch_id)
{
    struct ulp_detour_root *r;
    struct ulp_detour *d;

    for (r = __ulp_root; r != NULL; r = r->next)
        for (d = r->detours; d != NULL; d = d->next)
            if (memcmp(d->patch_id, patch_id,32)==0)
                d->active = 0;

    return 1;
}

/* these are here for debugging reasons :) */
void dump_ulp_patching_state(void)
{
    struct ulp_applied_patch *a_patch;
    struct ulp_applied_unit *a_unit;
    struct ulp_dependency *dep;
    int i;

    fprintf(stderr, "----- ULP state dump -----\n");
    for (a_patch = __ulp_state.patches; a_patch != NULL;
	    a_patch = a_patch->next)
    {
	fprintf(stderr, "* PATCH 0x");
	for (i = 0; i < 32; i++) {
	    fprintf(stderr, "%x.", a_patch->patch_id[i]);
	}
	fprintf(stderr, "\n");
	for (dep = a_patch->deps; dep != NULL; dep = dep->next) {
	    fprintf(stderr, "* DEPENDs 0x");
	    for (i = 0; i < 32; i++) {
		fprintf(stderr, "%x.", dep->dep_id[i]);
	    }
	    fprintf(stderr, "\n");
	}

	for (a_unit = a_patch->units; a_unit != NULL; a_unit = a_unit->next)
	    fprintf(stderr, "** %p %p\n", a_unit->patched_addr,
		    a_unit->target_addr);
    }
    fprintf(stderr, "----- End of dump ------\n");
}

void dump_ulp_detours(void)
{
    struct ulp_detour_root *r;
    struct ulp_detour *d;
    int i;
    fprintf(stderr, "====== ULP Roots ======\n");
    for (r = __ulp_root; r != NULL; r = r->next)
    {
        fprintf(stderr, "* ROOT:\n");
        fprintf(stderr, "* Index: %d\n", r->index);
        fprintf(stderr, "* Patched addr: %p\n", r->patched_addr);
        fprintf(stderr, "----- ULP DETOURS -----\n");
        for (d = r->detours; d != NULL; d = d->next)
        {
            fprintf(stderr, "  * DETOUR:\n");
            fprintf(stderr, "  * Universe: %ld\n", d->universe);
            fprintf(stderr, "  * Target addr: %p\n", d->target_addr);
            fprintf(stderr, "  * Active: ");
            if (d->active) fprintf(stderr, "yep\n");
            else fprintf(stderr, "nop\n");
            fprintf(stderr, "  * Patch ID: ");
            for (i = 0; i < 16; i++)  fprintf(stderr, "%x.", d->patch_id[i]);
            fprintf(stderr, "\n              ");
            for (i = 16; i < 32; i++)  fprintf(stderr, "%x.", d->patch_id[i]);
	    fprintf(stderr, "\n========================\n");
        }
    }
}
