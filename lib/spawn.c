#include <inc/lib.h>
#include <inc/elf.h>

#define UTEMP2USTACK(addr)	((uintptr_t) (addr) + (USTACKTOP - PGSIZE) - UTEMP)
#define UTEMP2			(UTEMP + PGSIZE)
#define UTEMP3			(UTEMP2 + PGSIZE)

// Helper functions for spawn() defined below.
static int init_stack(envid_t child, const char** argv, uintptr_t* init_esp);
static int load_segment(envid_t child, const char* prog, struct Proghdr* ph,
			struct Elf* elf, size_t elf_size);
static int copy_shared_pages(envid_t child);

// Load a page from a binary, located either in the kernel (as a kernel
// binary; see sys_kernbin_page_alloc) or in the file system.
// This function has the same signature as sys_kernbin_page_alloc,
// and should behave similarly.
// File system binaries always begin with a slash character,
// kernel binaries don't.
// Note that pg_perm will never contain PTE_W.
ssize_t
binary_page_alloc(envid_t dst_env, const char* name,
		  size_t offset, void* pg, unsigned pg_perm)
{
	static char last_binary_name[MAXPATHLEN];
	static int last_binary_fd = -1;
	static struct Stat last_binary_stat;
	int r;
	void* local_pg;

	/* hack: this means to close the file descriptor */
	if(!name)
	{
		if(last_binary_fd >= 0)
		{
			close(last_binary_fd);
			last_binary_fd = -1;
		}
		return 0;
	}
	
	// If a kernel binary was requested, call sys_kernbin_page_alloc.
	if (name[0] != '/')
		return sys_kernbin_page_alloc(dst_env, name,
					      offset, pg, pg_perm);

	// Emulate the action of sys_kernbin_page_alloc using the filesystem.
	// Read the header comment to sys_kernbin_page_alloc in kern/syscall.c
	// to see what you should do.
	// Hint:
	//   Use open, read_map, close, and stat/fstat (for the binary size).
	//   last_binary_name and last_binary_fd may be useful.
	if(last_binary_fd < 0 || strcmp(last_binary_name, name))
	{
		if(last_binary_fd >= 0)
			close(last_binary_fd);
		
		last_binary_fd = open(name, O_RDONLY);
		if(last_binary_fd < 0)
			return last_binary_fd;
		
		strcpy(last_binary_name, name);
		fstat(last_binary_fd, &last_binary_stat);
	}
	
	r = read_map(last_binary_fd, offset, &local_pg);
	if(r)
		return r;
	r = sys_page_map(0, local_pg, dst_env, pg, pg_perm);
	if(r)
		return r;

	return last_binary_stat.st_size;
}

// Now use binary_page_alloc instead of sys_kernbin_page_alloc.
#define sys_kernbin_page_alloc	binary_page_alloc

// Spawn a child process from a program image loaded from the kernel --
// or from the file system.
// prog: the name of the program to run.
// argv: pointer to null-terminated array of pointers to strings,
// 	 which will be passed to the child as its command-line arguments.
// Returns child envid on success, < 0 on failure.
int
spawn(const char* prog, const char** argv)
{
	uint8_t elf_header_buf[512];
	struct Trapframe child_tf;


	// Insert your code, following approximately this procedure:
	//
	//   - Map the first page of the kernel binary named prog at UTEMP,
	//     using sys_kernbin_page_alloc.
	//
	//   - Copy the kernel binary's 512-byte ELF header into
	//     elf_header_buf.
	//
	//   - Read the ELF header, as you have before, and sanity check its
	//     magic number.  (Check out your load_icode!)
	//
	//   - Use sys_exofork() to create a new environment.
	//
	//   - Set child_tf to an initial struct Trapframe for the child.
	//     Hint: The sys_exofork() system call has already created
	//     a good starting point, in envs[ENVX(child)].env_tf.
	//     Hint: You must do something with the program's entry point.
	//     What?  (See load_icode!)
	//
	//   - Call the init_stack() function above to set up
	//     the initial stack page for the child environment.
	//
	//   - Map all of the program's segments that are of p_type
	//     ELF_PROG_LOAD into the new environment's address space.
	//     Use the load_segment() helper function below.
	//     All the 'struct Proghdr' structures will be accessible
	//     within the first 512 bytes of the ELF.
	//
	//   - Call sys_set_trapframe(child, &child_tf) to set up the
	//     correct initial eip and esp values in the child.
	//
	//   - Start the child process running with sys_env_set_status().
	
	struct Elf * elf = (struct Elf *) &elf_header_buf;
	struct Proghdr * ph;
	envid_t child;
	int i, r;
	size_t elf_size;
	
	r = sys_kernbin_page_alloc(0, prog, 0, (void *) UTEMP, PTE_U | PTE_P);
	if(r < 0)
		return r;
	elf_size = r;
	memcpy(elf_header_buf, (void *) UTEMP, 512);
	
	if(elf->e_magic != ELF_MAGIC)
		return -1;
	
	child = sys_exofork();
	if(child < 0)
		return child;
	
	sys_env_set_name(child, prog);
	
	child_tf = envs[ENVX(child)].env_tf;
	child_tf.tf_eip = elf->e_entry;
	child_tf.tf_esp = USTACKTOP;
	/* terminate stack traces */
	child_tf.tf_ebp = 0;
	
	init_stack(child, argv, &child_tf.tf_esp);
	
	ph = (struct Proghdr *) &elf_header_buf[elf->e_phoff];
	for(i = 0; i < elf->e_phnum; i++)
	{
		if(ph[i].p_type != ELF_PROG_LOAD)
			continue;
		r = load_segment(child, prog, &ph[i], elf, elf_size);
		if(r < 0)
		{
			sys_env_destroy(child);
			return r;
		}
	}
	
	/* close the file descriptor */
	sys_kernbin_page_alloc(0, NULL, 0, NULL, 0);
	
	r = copy_shared_pages(child);
	if(r < 0)
	{
		sys_env_destroy(child);
		return r;
	}
	
	/* cannot fail */
	sys_set_trapframe(child, &child_tf);
	sys_env_set_status(child, ENV_RUNNABLE);
	
	return child;
}

// Spawn, taking command-line arguments array directly on the stack.
int
spawnl(const char *prog, const char *args, ...)
{
	return spawn(prog, &args);
}


// Set up the initial stack page for the new child process with envid 'child'
// using the arguments array pointed to by 'argv',
// which is a null-terminated array of pointers to null-terminated strings.
//
// On success, returns 0 and sets *init_esp
// to the initial stack pointer with which the child should start.
// Returns < 0 on failure.
static int
init_stack(envid_t child, const char** argv, uintptr_t* init_esp)
{
	size_t string_size;
	int argc, i, r;
	char* string_store;
	uintptr_t* argv_store;

	// Count the number of arguments (argc)
	// and the total amount of space needed for strings (string_size).
	string_size = 0;
	for (argc = 0; argv[argc] != 0; argc++)
		string_size += strlen(argv[argc]) + 1;

	// Determine where to place the strings and the argv array.
	// Set up pointers into the temporary page 'UTEMP'; we'll map a page
	// there later, then remap that page into the child environment
	// at (USTACKTOP - PGSIZE).
	// strings is the topmost thing on the stack.
	string_store = (char*) UTEMP + PGSIZE - string_size;
	// argv is below that.  There's one argument pointer per argument, plus
	// a null pointer.
	argv_store = (uintptr_t*) (ROUNDDOWN32(string_store, 4) - 4 * (argc + 1));
	
	// Make sure that argv, strings, and the 2 words that hold 'argc'
	// and 'argv' themselves will all fit in a single stack page.
	if ((void*) (argv_store - 2) < (void*) UTEMP)
		return -E_NO_MEM;

	// Allocate the single stack page at UTEMP.
	if ((r = sys_page_alloc(0, (void*) UTEMP, PTE_P|PTE_U|PTE_W)) < 0)
		return r;

	// Replace this with your code to:
	//
	//	* Initialize 'argv_store[i]' to point to argument string i,
	//	  for all 0 <= i < argc.
	//	  Also, copy the argument strings from 'argv' into the
	//	  newly-allocated stack page.
	//	  Hint: Copy the argument strings into string_store.
	//	  Hint: Make sure that argv_store uses addresses valid in the
	//	  CHILD'S environment!  The string_store variable itself
	//	  points into page UTEMP, but the child environment will have
	//	  this page mapped at USTACKTOP - PGSIZE.  Check out the
	//	  UTEMP2USTACK macro defined above.
	//
	//	* Set 'argv_store[argc]' to 0 to null-terminate the args array.
	//
	//	* Push two more words onto the child's stack below 'args',
	//	  containing the argc and argv parameters to be passed
	//	  to the child's umain() function.
	//	  argv should be below argc on the stack.
	//	  (Again, argv should use an address valid in the child's
	//	  environment.)
	//
	//	* Set *init_esp to the initial stack pointer for the child,
	//	  (Again, use an address valid in the child's environment.)
	//
	*init_esp = UTEMP2USTACK(argv_store - 2);
	argv_store[-2] = argc;
	argv_store[-1] = UTEMP2USTACK(argv_store);
	for(i = 0; i != argc; i++)
	{
		argv_store[i] = UTEMP2USTACK(string_store);
		strcpy(string_store, argv[i]);
		string_store += strlen(argv[i]) + 1;
	}
	/* the page is zero anyway... but for clarity, terminate the argv array */
	argv_store[argc] = 0;

	// After completing the stack, map it into the child's address space
	// and unmap it from ours!
	if ((r = sys_page_map(0, (void*) UTEMP, child, (void*) (USTACKTOP - PGSIZE), PTE_P | PTE_U | PTE_W)) < 0)
		goto error;
	if ((r = sys_page_unmap(0, (void*) UTEMP)) < 0)
		goto error;

	return 0;

error:
	sys_page_unmap(0, (void*) UTEMP);
	return r;
}

// Load the 'ph' program segment of kernel binary 'prog' into 'child's address
// space at the proper place.
// (You may not need the elf and elf_size arguments.)
//
// Returns 0 on success, < 0 on error.  (Or panic on error.)
// There's no need to clean up page mappings after error; the caller will call
// sys_env_destroy(), which cleans up most mappings for us.
static int
load_segment(envid_t child, const char* prog, struct Proghdr* ph,
	     struct Elf* elf, size_t elf_size)
{
	// Use the p_flags field in the Proghdr for each segment
	// to determine how to map the segment:
	//
	//    * If the ELF flags do not include ELF_PROG_FLAG_WRITE,
	//	then the segment contains text and read-only data.
	//	Use sys_kernbin_page_alloc() to map the contents of
	//	this segment directly into the child
	//      so that multiple instances of the same program
	//	will share the same copy of the program text.
	//      Be sure to map the program text read-only in the child.
	//
	//    * If the ELF segment flags DO include ELF_PROG_FLAG_WRITE,
	//	then the segment contains read/write data and bss.
	//	As with load_icode(), such an ELF segment
	//	occupies p_memsz bytes in memory, but only the first
	//	p_filesz bytes of the segment are actually loaded
	//	from the executable file -- you must clear the rest to zero.
	//      For each page to be mapped for a read/write segment,
	//	allocate a page of memory at UTEMP in the current
	//	environment.  Then use sys_kernbin_page_alloc() to map
	//	the appropriate portion of the kernel binary at UTEMP2,
	//	copy the data over to the page at UTEMP, and unmap the
	//	kernel binary page, and/or use memset() to zero the
	//	non-loaded portions.
	//	(You can avoid calling memset(), if you like, because
	//	page_alloc() returns zeroed pages already.)
	//	Then insert the correct page mapping into the child.
	//	Look at load_icode() and fork() for inspiration.
	//      Be sure you understand why you can't just use
	//	sys_kernbin_page_alloc() directly here.
	//
	// Note: All of the segment addresses or lengths above
	// might be non-page-aligned, so you must deal with
	// these non-page-aligned values appropriately.
	// The ELF linker does, however, guarantee that no two segments
	// will overlap on the same page; and it guarantees that
	// PGOFF(ph->p_offset) == PGOFF(ph->p_va).
	
	
	if(ph->p_flags & ELF_PROG_FLAG_WRITE)
	{
		uintptr_t start = ROUNDDOWN32(ph->p_va, PGSIZE);
		uintptr_t end = ROUND32(ph->p_va + ph->p_filesz, PGSIZE);
		int filepages = (end - start) >> PGSHIFT;
		int mempages, i;
		
		end = ROUND32(ph->p_va + ph->p_memsz, PGSIZE);
		mempages = (end - start) >> PGSHIFT;
		
		/* use "end" for source */
		end = ROUNDDOWN32(ph->p_offset, PGSIZE);
		for(i = 0; i < filepages; i++)
		{
			if(sys_kernbin_page_alloc(0, prog, end, (void *) UTEMP, PTE_U | PTE_P) < 0)
				return -1;
			if(sys_page_alloc(0, (void *) UTEMP2, PTE_U | PTE_W | PTE_P))
				return -1;
			memcpy((void *) UTEMP2, (void *) UTEMP, PGSIZE);
			if(sys_page_map(0, (void *) UTEMP2, child, (void *) start, PTE_U | PTE_W | PTE_P))
				return -1;
			start += PGSIZE;
			end += PGSIZE;
		}
		end = PGSIZE - PGOFF(ph->p_offset + ph->p_filesz);
		if(end != PGSIZE)
			memset((void *) (UTEMP2 + PGSIZE - end), 0, end);
		for(; i < mempages; i++)
		{
			if(sys_page_alloc(child, (void *) start, PTE_U | PTE_W | PTE_P))
				return -1;
			start += PGSIZE;
		}
		
		sys_page_unmap(0, (void *) UTEMP);
		sys_page_unmap(0, (void *) UTEMP2);
	}
	else
	{
		uintptr_t start = ROUNDDOWN32(ph->p_va, PGSIZE);
		uintptr_t end = ROUND32(ph->p_va + ph->p_filesz, PGSIZE);
		int pages = (end - start) >> PGSHIFT;
		int i;
		
		/* use "end" for source */
		end = ROUNDDOWN32(ph->p_offset, PGSIZE);
		for(i = 0; i < pages; i++)
		{
			if(sys_kernbin_page_alloc(child, prog, end, (void *) start, PTE_U | PTE_P) < 0)
				return -1;
			start += PGSIZE;
			end += PGSIZE;
		}
	}
	
	return 0;
}

// Copy the mappings for shared pages into the child address space.
static int
copy_shared_pages(envid_t child)
{
	int r;
	uint32_t pdx, ptx;

	// Loop over all pages in the current address space,
	// and copy any pages marked as PTE_SHARE into 'child'
	// at the same location and with the same permissions.
	// Hint: Use vpd, vpt, and sys_page_map.

	for(pdx = 0; pdx != (UTOP >> PTSHIFT); pdx++)
	{
		if(!vpd[pdx])
			continue;
		for(ptx = 0; ptx != NPTENTRIES; ptx++)
		{
			void * addr = (void *) PGADDR(pdx, ptx, 0);
			if(vpt[VPN(addr)] & PTE_SHARE)
			{
				r = sys_page_map(0, addr, child, addr, vpt[VPN(addr)] & PTE_USER);
				if(r)
					return r;
			}
		}
	}
	
	return 0;
}

