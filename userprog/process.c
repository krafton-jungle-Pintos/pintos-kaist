#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "userprog/syscall.h"
#include "userprog/process.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);

void argument_stack(char **argv, int argc, void **rspp);
struct thread *get_child_with_pid(int pid)
{
	struct thread *cur = thread_current();
	struct list *child_list = &cur->child_list;

	for (struct list_elem *e = list_begin(child_list); e != list_end(child_list); e = list_next(e))
	{
		struct thread *t = list_entry(e, struct thread, child_elem);
		if (t->tid == pid)
		{
			return t;
		}
	}
	return NULL;
}

/* General process initializer for initd and other process. */
static void
process_init(void)
{
	struct thread *current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name)
{
	//커맨드 라인에서 받은 arguments를 통해 실행하고자 하는 파일에 대한 프로세스를 만드는 과정
	//예를 들어 pintos -- -q run alarm-clock 라는 commad가 입력되었을 때
	//arg[0] 에는 run이, arg[1]에는 실행하고자 하는 file name과 그에 붙는 arguments이 있는 string이 들어있음
	//process_create_initd에서 인자로 arg[1]을 받고 이것을 parsing하여 user stack에 쌓아야한다.
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page(0); //페이지 할당받고
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy(fn_copy, file_name, PGSIZE); //해당 페이지에 file_name을 copy로 저장함

	// project 2 : system call
	// file_name을 분리해서 넣어줘야함
	char *save_ptr;
	strtok_r(file_name, " ", &save_ptr);
	/* Create a new thread to execute FILE_NAME. */
	// PRI_DEFAULT : 기본 우선순위 31
	// file_name을 이름으로 하고 PRI_DEFAULT를 우선순위로 갖는 새로운 thread 생성, tid에 저장
	// thread는 fn_copy를 인자로 받는 initd라는 함수를 실행시킴

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create(file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page(fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd(void *f_name)
{
#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif

	process_init();

	if (process_exec(f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char *name, struct intr_frame *if_ UNUSED)
{

	/* Clone current thread to new thread.*/
	struct thread *cur = thread_current();

	// 현재 thread의 parent_if에 if_를 저장
	memcpy(&cur->parent_if, if_, sizeof(struct intr_frame));

	tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, cur);

	if (tid == TID_ERROR)
	{
		return TID_ERROR;
	}

	struct thread *child = get_child_with_pid(tid); // child_list안에서 만들어진 child thread를 찾음
	sema_down(&child->fork_sema);					// 자식이 메모리에 load 될때까지 기다림(blocked)
	if (child->exit_status == -1)
	{
		return TID_ERROR;
	}
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte(uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va))
	{
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page(parent->pml4, va);
	if (parent_page == NULL)
	{
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL)
	{
		return false;
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page(current->pml4, va, newpage, writable))
	{
		/* 6. TODO: if fail to insert page, do error handling. */
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork(void *aux)
{
	struct intr_frame if_;
	struct thread *parent = (struct thread *)aux;
	struct thread *current = thread_current();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;
	bool succ = true;

	parent_if = &parent->parent_if;

	/* 1. Read the cpu context to local stack. */
	memcpy(&if_, parent_if, sizeof(struct intr_frame));

	if_.R.rax = 0;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate(current);
#ifdef VM
	supplemental_page_table_init(&current->spt);
	if (!supplemental_page_table_copy(&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	if (parent->fd_idx == FDT_COUNT_LIMIT)
	{
		goto error;
	}

	for (int i = 0; i < FDT_COUNT_LIMIT; i++)
	{
		struct file *file = parent->fd_table[i];
		// if (file = NULL)
		// 	continue;

		struct file *new_file;
		// 표시
		if (file > 2)
		{
			new_file = file_duplicate(file);
		}
		else
		{
			new_file = file;
		}
		current->fd_table[i] = new_file;
		
	}
	// int cnt = 2;
	// struct file **table = parent->fd_table;
	// while (cnt < FDT_COUNT_LIMIT) {
	// 	if (table[cnt]) {
	// 		current->fd_table[cnt] = file_duplicate(table[cnt]);
	// 	} else {
	// 		current->fd_table[cnt] = NULL;
	// 	}
	// 	cnt++;
	// }
	
	current->fd_idx = parent->fd_idx;

	sema_up(&current->fork_sema);

	// 표시
	if (succ)
	{
		do_iret(&if_);
	}

error:
	current->exit_status = TID_ERROR;
	sema_up(&current->fork_sema);
	exit(TID_ERROR);
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name)
{
	char *file_name = f_name; //void로 넘겨받은 f_name을 문자열로 인식하기 위해서 자료형을 char *로 변환해줌
	bool success;
	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	/*
		intr_frame은 실행중인 프로세스의 context, 즉 register 정보, stack pointer, instruction counter를 저장하는 자료구조
		interrupt나 systemcall 호출시 사용
	*/
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG; //stack - user data
	_if.cs = SEL_UCSEG; // stack - user code
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup(); //새로운 실행 파일을 현재 쓰레드에 담기 전에 현재 프로세스에 담긴 컨텍스트를 지움

	/* argument parsing */
	char *argv[128]; // argument 배열
	int argc = 0;	// argument 개수
	char *token;
	char *save_ptr; // 분리된 문자열 중 남는 부분의 시작주소, 직접 처리할 일 X
	token = strtok_r(file_name, " ", &save_ptr); 
	//strtok_r : 첫 번째 매개 변수 문자열을 두 번째 매개변수 구분자를 기준으로 문자열을 분할하여 각 문자열의 포인터를 반환함
	while (token != NULL)
	{
		argv[argc] = token;
		token = strtok_r(NULL, " ", &save_ptr); 
		//strtok_r의 첫번째 매개변수 문자열이 NULL이면 save_ptr에서 이전에 호출한 위치 다음부터 분리작업을 진행함
		//여백 " "을 기준으로 문자열을 분할하는데 각 인자에 sentinel '\0'을 추가하여 저장함
		//ex) cmd_line이 rm -rf인 경우 argv에 [rm\0,-rf\0, \0]의 형태로 저장됨
		argc++;
	}
	/* And then load the binary */
	/* _if의 rsp에 유저스택, rip에 스택 포인터 할당하는 과정 */
	success = load(file_name, &_if); // 여기선 이미 filename 에 앞부분만 담겨있다.


	// hex_dump(_if.rsp, _if.rsp, USER_STACK - (uint64_t)*rspp, true);	// 잘 됐는지 check 용

	/* If load failed, quit. */
	if (!success)
	{
		palloc_free_page(file_name);
		return -1;
	}
	/* 스택에 인자 넣기 */
	void **rspp = &_if.rsp; // 유저 스택을 가르키는 주소인 rsp의 주소가 rspp
	argument_stack(argv, argc, rspp);
	_if.R.rdi = argc;							  // rdi : 목적지 ( arg 몇개 들어갔는지 )
	_if.R.rsi = (uint64_t)*rspp + sizeof(void *); // rsi : 출발지 ( 초기화한 return address의 직전 )
	/* Start switched process. Context Switching */
	do_iret(&_if); // intr_frame 정보를 가지고 새롭게 생성된 프로세스로 context switching
	NOT_REACHED();
}

/* 유저스택에 인자 저장 */
void argument_stack(char **argv, int argc, void **rspp)
{
	// 1. Save argument strings (character by character)
	// 각 인자 스트링을 스택에 한글자씩 기록
	for (int i = argc - 1; i >= 0; i--) // 인자 개수 기준 내림차순
	{
		int N = strlen(argv[i]); // 각 인자의 길이(argv[i]의 길이)
		for (int j = N; j >= 0; j--)
		{
			char individual_character = argv[i][j]; // 각 인자의 각 요소 넣기
			(*rspp)--;								// rspp가 가르키는 rsp를 1 byte씩 내리기
			**(char **)rspp = individual_character; // *별 하나로 값 찾은 뒤, rspp는 원래 값이 주소담았으니 그 값에 ind_cha 다시 담는다.
													//*(char *)(_if.rsp) = individual_character를 해주고 싶으니까
													// char * 형으로 캐스팅한 다음, * 포인터를 통해 해당 값에 접근
		}
		argv[i] = *(char **)rspp; // 찐 rsp 담기
								  // 각 인자별 첫 글자의 스택 주소 저장(나중에 쓸 "name"의 첫 주소 저장)
	}
	// 2. Word-align padding
	// 인자들을 모두 저장한 후, 현재 스택 포인터(_if.rsp)의 값이 8배수가 되도록 맞춰주기
	// 64비트 이므로, 8바이트 단위로 끊어주기
	// rsp가 8의 배수가 되도록 설정

	// 마지막 인자 주소값을 8로 나눈 나머지만큼 밑으로 이동하며 0으로 초기화
	int pad = (int)*rspp % 8;
	for (int k = 0; k < pad; k++)
	{
		(*rspp)--;						 // rspp가 가르키는 rsp를 1 byte씩 내리기
		**(uint8_t **)rspp = (uint8_t)0; // 1 byte씩 아래로 이동하면서, 각 칸의 내용을 0으로 초기화
	}

	// 3. Pointers to the argument strings
	// 인자들이 stack에 저장된 주소를 stack에 기입하는 것
	size_t PTR_SIZE = sizeof(char *); // 캐릭터형 포인터 자료구조의 size는 8byte

	(*rspp) -= PTR_SIZE;		  // 캐릭터형 포인터 사이즈 만큼 빼주기
	**(char ***)rspp = (char *)0; // 해당 위치 값 0으로 초기화

	for (int i = argc - 1; i >= 0; i--)
	{
		(*rspp) -= PTR_SIZE;
		**(char ***)rspp = argv[i]; // 해당 위치에 argv[i] 기입(i번째 arg의 주소)
	}

	// 4. Return address를 0으로 초기화(push a fake 'return address')
	(*rspp) -= PTR_SIZE;
	**(void ***)rspp = (void *)0;
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid UNUSED)
{
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */

	struct thread *child = get_child_with_pid(child_tid);
	if (child == NULL)
	{
		return -1;
	}

	/* 자식 프로세스의 process_exit() 과 연계 */
	sema_down(&child->wait_sema);
	int exit_status = child->exit_status; // 마지막 줄에서 process_clean 당하기 전에 자식의 종료 상태를 남겨둠
	list_remove(&child->child_elem);	  // 끝날 것이므로 자식리스트에서 제거
	sema_up(&child->free_sema);

	return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
	struct thread *cur = thread_current();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	for (int i = 0; i < FDT_COUNT_LIMIT; i++) // 바이트단위로 들어가서 하나하나 다 지워준다.
	{
		close(i);
	}
	// for multi-oom(메모리 누수)
	palloc_free_multiple(cur->fd_table, FDT_PAGES);
	// for rox- (실행중에 수정 못하도록)
	file_close(cur->running);
	

	sema_up(&cur->wait_sema);	// 종료되었다고 기다리고 있는 부모 thread에게 signal 보냄-> sema_up에서 val을 올려줌
	sema_down(&cur->free_sema); // 부모의 exit_Status가 정확히 전달되었는지 확인(wait)
	process_cleanup();			// pml4를 날림(이 함수를 call 한 thread의 pml4)
}

/* Free the current process's resources. */
static void
process_cleanup(void)
{
	struct thread *curr = thread_current();

#ifdef VM
	supplemental_page_table_kill(&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL)
	{
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate(NULL);
		pml4_destroy(pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
	/* Activate thread's page tables. */
	pml4_activate(next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0			/* Ignore. */
#define PT_LOAD 1			/* Loadable segment. */
#define PT_DYNAMIC 2		/* Dynamic linking info. */
#define PT_INTERP 3			/* Name of dynamic loader. */
#define PT_NOTE 4			/* Auxiliary info. */
#define PT_SHLIB 5			/* Reserved. */
#define PT_PHDR 6			/* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load(const char *file_name, struct intr_frame *if_)
{
	struct thread *t = thread_current();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create();
	if (t->pml4 == NULL)
		goto done;
	process_activate(thread_current()); //페이지 테이블 활성화

	/* Open executable file. */
	file = filesys_open(file_name);
	if (file == NULL)
	{
		printf("load: %s: open failed\n", file_name);
		goto done;
	}

	t->running = file;
	file_deny_write(file);

	/* Read and verify executable header. */
	if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
		|| ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024)
	{
		printf("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++)
	{
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length(file))
			goto done;
		file_seek(file, file_ofs);

		if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type)
		{
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* Ignore this segment. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
			if (validate_segment(&phdr, file))
			{
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0)
				{
					/* Normal segment.
					 * Read initial part from disk and zero the rest. */
					read_bytes = page_offset + phdr.p_filesz;
					zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
				}
				else
				{
					/* Entirely zero.
					 * Don't read anything from disk. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
				}
				if (!load_segment(file, file_page, (void *)mem_page,
								  read_bytes, zero_bytes, writable))
					goto done;
			}
			else
				goto done;
			break;
		}
	}

	/* Set up stack. */
	if (!setup_stack(if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	// file_close(file);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Phdr *phdr, struct file *file)
{
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t)file_length(file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr((void *)phdr->p_vaddr))
		return false;
	if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	file_seek(file, ofs);
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page(PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
		{
			palloc_free_page(kpage);
			return false;
		}
		memset(kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page(upage, kpage, writable))
		{
			printf("fail\n");
			palloc_free_page(kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack(struct intr_frame *if_)
{
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (kpage != NULL)
	{
		success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page(kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment(struct page *page, void *aux)
{
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer(VM_ANON, upage,
											writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack(struct intr_frame *if_)
{
	bool success = false;
	void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */

