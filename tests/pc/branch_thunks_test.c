/* The indirect-branch thunks (src/pc/guest/branch_thunks.c): for each of the
 * seven registers a compiler may call through, a target in guest memory
 * reaches what the resolver names and a host target is jumped to as it is,
 * with every register (the target register too) and the stack as the caller
 * left them; the same through the entry of the stubs the build writes for
 * module functions called by name. Then calls from C, which this file is compiled to route through
 * the thunks: arguments and results pass, a tail call works, and the
 * resolver runs on an aligned stack.
 * Nothing here maps guest memory: the guest addresses must never be jumped
 * to, or the test faults. */
#include "pc/guest/image.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__i386__)
#ifdef _WIN32
#define SYMBOL(name) "_" #name
#else
#define SYMBOL(name) #name
#endif

#define GUEST_RECORD 0x80001000u
#define GUEST_ADD 0x80002004u
#define GUEST_PHYSICAL 0x00012340u
#define GUEST_KSEG1 0xa0002004u

/* What record_registers saw: eax, ecx, edx, ebx, esp, ebp, esi, edi. */
uint32_t seen[8];
uint32_t esp_before;
static unsigned resolved, misaligned;
void record_registers(void);
__asm__(".text\n"
        ".globl " SYMBOL(record_registers) "\n"
        SYMBOL(record_registers) ":\n"
        "    movl %eax, " SYMBOL(seen) "\n"
        "    movl %ecx, " SYMBOL(seen) "+4\n"
        "    movl %edx, " SYMBOL(seen) "+8\n"
        "    movl %ebx, " SYMBOL(seen) "+12\n"
        "    movl %esp, " SYMBOL(seen) "+16\n"
        "    movl %ebp, " SYMBOL(seen) "+20\n"
        "    movl %esi, " SYMBOL(seen) "+24\n"
        "    movl %edi, " SYMBOL(seen) "+28\n"
        "    ret\n");

/* call_through_<reg>(target): every register but esp set to a known value,
 * <reg> to `target`, then a call through that register's thunk. */
#define DRIVER(reg)                                                               \
    void call_through_##reg(uint32_t target);                                     \
    __asm__(".text\n"                                                             \
            ".globl " SYMBOL(call_through_##reg) "\n"                             \
            SYMBOL(call_through_##reg) ":\n"                                      \
            "    movl 4(%esp), %eax\n"                                            \
            "    pushal\n"                                                        \
            "    pushl %eax\n"                                                    \
            "    movl $0x11111111, %eax\n"                                        \
            "    movl $0x22222222, %ecx\n"                                        \
            "    movl $0x33333333, %edx\n"                                        \
            "    movl $0x44444444, %ebx\n"                                        \
            "    movl $0x66666666, %ebp\n"                                        \
            "    movl $0x77777777, %esi\n"                                        \
            "    movl $0x88888888, %edi\n"                                        \
            "    popl %" #reg "\n"                                                \
            "    movl %esp, " SYMBOL(esp_before) "\n"                             \
            "    call " SYMBOL(__x86_indirect_thunk_##reg) "\n"                   \
            "1:  cmpl %esp, " SYMBOL(esp_before) "\n"                             \
            "    je 2f\n"                                                         \
            "    movl $0, " SYMBOL(esp_before) "\n"                               \
            "2:  popal\n"                                                         \
            "    ret\n");
DRIVER(eax)
DRIVER(ecx)
DRIVER(edx)
DRIVER(ebx)
DRIVER(esi)
DRIVER(edi)
DRIVER(ebp)

/* A module function the C calls by name, as build_game32.py writes its host
 * stub (guest_branches.c), and a driver that calls it directly with every
 * register set to its known value (the argument is not used). */
__asm__(".text\n"
        "direct_record:\n"
        "    pushl $0x80001000\n"
        "    jmp " SYMBOL(Memories_GuestBranchDirect) "\n"
        ".globl " SYMBOL(call_direct) "\n"
        SYMBOL(call_direct) ":\n"
        "    pushal\n"
        "    movl $0x11111111, %eax\n"
        "    movl $0x22222222, %ecx\n"
        "    movl $0x33333333, %edx\n"
        "    movl $0x44444444, %ebx\n"
        "    movl $0x66666666, %ebp\n"
        "    movl $0x77777777, %esi\n"
        "    movl $0x88888888, %edi\n"
        "    movl %esp, " SYMBOL(esp_before) "\n"
        "    call direct_record\n"
        "    cmpl %esp, " SYMBOL(esp_before) "\n"
        "    je 1f\n"
        "    movl $0, " SYMBOL(esp_before) "\n"
        "1:  popal\n"
        "    ret\n");
void call_direct(uint32_t unused);

static int add3(int a, int b, int c) { return a + b + c; }

static void *resolve(unsigned address)
{
    /* The thunk aligns the stack to 16 before the call, so this frame's
     * base (below the return address and the saved ebp) is 8 past it. */
    if (((uintptr_t)__builtin_frame_address(0) & 15) != 8) misaligned++;
    resolved++;
    if (address == GUEST_RECORD) return (void *)(uintptr_t)record_registers;
    if (address == GUEST_ADD || address == GUEST_KSEG1 || address == GUEST_PHYSICAL) return (void *)(uintptr_t)add3;
    return (void *)(uintptr_t)address;
}

static int check_registers(const char *name, void (*driver)(uint32_t), int slot, uint32_t target)
{
    static const uint32_t magic[8] = {0x11111111, 0x22222222, 0x33333333, 0x44444444, 0, 0x66666666, 0x77777777,
                                      0x88888888};
    int i, failures = 0;
    memset(seen, 0, sizeof(seen));
    driver(target);
    for (i = 0; i < 8; i++) {
        uint32_t expected = i == slot ? target : magic[i];
        if (i == 4) continue;
        if (seen[i] != expected) {
            printf("FAIL %s -> 0x%08x: register %d is 0x%08x, not 0x%08x\n", name, target, i, seen[i], expected);
            failures++;
        }
    }
    if (!esp_before || seen[4] != esp_before - 4) {
        printf("FAIL %s -> 0x%08x: the stack moved\n", name, target);
        failures++;
    }
    return failures;
}

typedef int (*Add)(int, int, int);

static int __attribute__((noinline)) tail_call(Add function, int x)
{
    return function(x, 1, 2);   /* a sibling call at -O2: jmp through the thunk */
}

int main(void)
{
    static const struct { const char *name; void (*driver)(uint32_t); int slot; } drivers[] = {
        {"eax", call_through_eax, 0}, {"ecx", call_through_ecx, 1}, {"edx", call_through_edx, 2},
        {"ebx", call_through_ebx, 3}, {"ebp", call_through_ebp, 5}, {"esi", call_through_esi, 6},
        {"edi", call_through_edi, 7}};
    volatile Add through;
    unsigned i, before;
    int failures = 0;
    Memories_GuestBranchResolver = resolve;
    for (i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
        before = resolved;
        failures += check_registers(drivers[i].name, drivers[i].driver, drivers[i].slot, GUEST_RECORD);
        if (resolved != before + 1) {
            printf("FAIL %s: the guest target was not resolved\n", drivers[i].name);
            failures++;
        }
        before = resolved;
        failures += check_registers(drivers[i].name, drivers[i].driver, drivers[i].slot,
                                    (uint32_t)(uintptr_t)record_registers);
        if (resolved != before) {
            printf("FAIL %s: a host target went to the resolver\n", drivers[i].name);
            failures++;
        }
    }
    before = resolved;
    failures += check_registers("direct call", call_direct, -1, GUEST_RECORD);
    if (resolved != before + 1) failures++, printf("FAIL direct call: the guest address was not resolved\n");
    through = (Add)(uintptr_t)GUEST_ADD;
    if (through(1, 2, 3) != 6) failures++, printf("FAIL call to a KSEG0 guest address\n");
    through = (Add)(uintptr_t)GUEST_KSEG1;
    if (through(10, 20, 30) != 60) failures++, printf("FAIL call to a KSEG1 guest address\n");
    through = (Add)(uintptr_t)GUEST_PHYSICAL;
    if (through(5, 5, 5) != 15) failures++, printf("FAIL call to a physical guest address\n");
    if (tail_call((Add)(uintptr_t)GUEST_ADD, 4) != 7) failures++, printf("FAIL tail call to a guest address\n");
    through = add3;
    before = resolved;
    if (through(1, 1, 1) != 3 || resolved != before) failures++, printf("FAIL call to a host function\n");
    if (misaligned) failures++, printf("FAIL the resolver ran on a misaligned stack %u times\n", misaligned);
    printf("%s\n", failures ? "branch thunks: FAILED" : "branch thunks: ok");
    return failures != 0;
}
#else
int main(void)
{
    puts("branch thunks: 32-bit x86 only");
    return 0;
}
#endif
