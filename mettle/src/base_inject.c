#include "base_inject.h"
#include "log.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define MIGRATE_STACK_SIZE 0x100000

#if defined(__x86_64__)
#define DO_SYSCALL "\x0f\x05\xcc"
#else
#define DO_SYSCALL "\xcd\x80\xcc"
#endif

static int wait_for_stop(int pid)
{
	int status;

	if (waitpid(pid, &status, 0) != pid) {
		return 0;
	}

	return WIFSTOPPED(status);
}

static int wait_for_trap(int pid)
{
	int status;

	for (;;) {
		if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
			return 0;
		}
		if (WSTOPSIG(status) == SIGTRAP) {
			return 1;
		}
		if (ptrace(PTRACE_CONT, pid, NULL,
				(void *)(long)(WSTOPSIG(status) == SIGCHLD ? 0 : WSTOPSIG(status))) != 0) {
			return 0;
		}
	}
}

static int read_word(int pid, unsigned long address, long *word)
{
	errno = 0;
	*word = ptrace(PTRACE_PEEKDATA, pid, (void *)address, NULL);
	return errno == 0;
}

static int write_word(int pid, unsigned long address, long word)
{
	return ptrace(PTRACE_POKEDATA, pid, (void *)address, (void *)word) == 0;
}

static int remote_write(int pid, unsigned long address, const void *data,
	size_t data_length)
{
	const unsigned char *cursor = data;

	for (size_t offset = 0; offset < data_length; offset += sizeof(long)) {
		long word;
		size_t remaining = data_length - offset;
		size_t copy_length = remaining < sizeof(word) ? remaining : sizeof(word);

		if (!read_word(pid, address + offset, &word)) {
			return 0;
		}
		memcpy(&word, cursor + offset, copy_length);
		if (!write_word(pid, address + offset, word)) {
			return 0;
		}
	}

	return 1;
}

static int remote_syscall(int pid, struct user_regs_struct *saved_regs,
	struct user_regs_struct *regs, long *result)
{
	long original_word;
	long patched_word;
	unsigned long instruction_pointer;
	int ok = 0;

#if defined(__x86_64__)
	instruction_pointer = saved_regs->rip;
	regs->rip = instruction_pointer;
	regs->orig_rax = -1;
#else
	instruction_pointer = saved_regs->eip;
	regs->eip = instruction_pointer;
	regs->orig_eax = -1;
#endif

	if (!read_word(pid, instruction_pointer, &original_word)) {
		return 0;
	}
	patched_word = original_word;
	memcpy(&patched_word, DO_SYSCALL, sizeof(DO_SYSCALL) - 1);
	if (!write_word(pid, instruction_pointer, patched_word)) {
		return 0;
	}

	if (ptrace(PTRACE_SETREGS, pid, NULL, regs) != 0) {
		goto cleanup;
	}
	if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || !wait_for_trap(pid)) {
		goto cleanup;
	}
	if (ptrace(PTRACE_GETREGS, pid, NULL, regs) != 0) {
		goto cleanup;
	}

#if defined(__x86_64__)
	*result = (long)regs->rax;
#else
	*result = (int32_t)regs->eax;
#endif
	ok = 1;

cleanup:
	if (!write_word(pid, instruction_pointer, original_word)) {
		ok = 0;
	}
	if (ptrace(PTRACE_SETREGS, pid, NULL, saved_regs) != 0) {
		ok = 0;
	}
	return ok;
}

static int syscall_failed(long result)
{
	return result < 0 && result >= -4095;
}

static unsigned long remote_map(int pid, struct user_regs_struct *saved_regs,
	size_t length, int protection, int flags)
{
	struct user_regs_struct regs = *saved_regs;
	long result;

#if defined(__x86_64__)
	regs.rax = 9;
	regs.rdi = 0;
	regs.rsi = length;
	regs.rdx = protection;
	regs.r10 = flags;
	regs.r8 = -1;
	regs.r9 = 0;
#else
	regs.eax = 192;
	regs.ebx = 0;
	regs.ecx = length;
	regs.edx = protection;
	regs.esi = flags;
	regs.edi = -1;
	regs.ebp = 0;
#endif

	if (!remote_syscall(pid, saved_regs, &regs, &result) || syscall_failed(result)) {
		return 0;
	}
	return (unsigned long)result;
}

static int remote_unmap(int pid, struct user_regs_struct *saved_regs,
	unsigned long address, size_t length)
{
	struct user_regs_struct regs = *saved_regs;
	long result;

#if defined(__x86_64__)
	regs.rax = 11;
	regs.rdi = address;
	regs.rsi = length;
#else
	regs.eax = 91;
	regs.ebx = address;
	regs.ecx = length;
#endif

	return remote_syscall(pid, saved_regs, &regs, &result) && !syscall_failed(result);
}

static int remote_call(int pid, struct user_regs_struct *saved_regs,
	unsigned long payload_address, unsigned long stub_address,
	unsigned long stack_address, unsigned long ack_address, int fd)
{
	struct user_regs_struct regs = *saved_regs;
	long result;

#if defined(__x86_64__)
	regs.rip = stub_address;
	regs.orig_rax = -1;
	regs.rsp = stack_address + MIGRATE_STACK_SIZE - 0x200;
	regs.rax = payload_address;
	regs.rbx = getpid();
	regs.rcx = fd;
	regs.rdx = ack_address;
#else
	regs.eip = stub_address;
	regs.orig_eax = -1;
	regs.esp = stack_address + MIGRATE_STACK_SIZE - 0x200;
	regs.eax = payload_address;
	regs.ebx = getpid();
	regs.ecx = fd;
	regs.edx = ack_address;
#endif

	if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
		ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
		!wait_for_trap(pid) ||
		ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
		return 0;
	}

#if defined(__x86_64__)
	result = (long)regs.rax;
#else
	result = (int32_t)regs.eax;
#endif
	if (result != 1) {
		log_error("Migrated child did not become ready (status %ld)", result);
	}

	return ptrace(PTRACE_SETREGS, pid, NULL, saved_regs) == 0 && result == 1;
}

int migrate_support(void)
{
	return 1;
}

int migrate(int pid, char *migrate_stub, size_t migrate_stub_length,
	char *payload, size_t payload_length, const char *uuid, int fd)
{
	struct user_regs_struct saved_regs;
	unsigned long payload_address = 0;
	unsigned long stub_address = 0;
	unsigned long stack_address = 0;
	unsigned long ack_address = 0;
	int attached = 0;
	int success = 0;

	(void)uuid;
	if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 || !wait_for_stop(pid)) {
		return 0;
	}
	attached = 1;
	if (ptrace(PTRACE_GETREGS, pid, NULL, &saved_regs) != 0) {
		goto cleanup;
	}

	payload_address = remote_map(pid, &saved_regs, payload_length,
		PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS);
	stub_address = remote_map(pid, &saved_regs, migrate_stub_length,
		PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS);
	stack_address = remote_map(pid, &saved_regs, MIGRATE_STACK_SIZE,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
	ack_address = remote_map(pid, &saved_regs, sizeof(uint32_t),
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS);
	if (!payload_address || !stub_address || !stack_address || !ack_address) {
		goto cleanup;
	}
	if (!remote_write(pid, payload_address, payload, payload_length) ||
		!remote_write(pid, stub_address, migrate_stub, migrate_stub_length)) {
		goto cleanup;
	}

	success = remote_call(pid, &saved_regs, payload_address, stub_address,
		stack_address, ack_address, fd);

cleanup:
	if (attached) {
		if (payload_address) {
			remote_unmap(pid, &saved_regs, payload_address, payload_length);
		}
		if (stub_address) {
			remote_unmap(pid, &saved_regs, stub_address, migrate_stub_length);
		}
		if (stack_address) {
			remote_unmap(pid, &saved_regs, stack_address, MIGRATE_STACK_SIZE);
		}
		if (ack_address) {
			remote_unmap(pid, &saved_regs, ack_address, sizeof(uint32_t));
		}
		ptrace(PTRACE_DETACH, pid, NULL, NULL);
	}
	return success;
}

#else

int migrate_support(void)
{
	return 0;
}

int migrate(int pid, char *migrate_stub, size_t migrate_stub_length,
	char *payload, size_t payload_length, const char *uuid, int fd)
{
	(void)pid;
	(void)migrate_stub;
	(void)migrate_stub_length;
	(void)payload;
	(void)payload_length;
	(void)uuid;
	(void)fd;
	return 0;
}

#endif
