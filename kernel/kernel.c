#include "prosenjit.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"
#include "health.h"
#include "crash_record.h"
#include "panic_handoff.h"
#include "log.h"
#include "address_space.h"
#include "elf_loader.h"
#include "init.h"
#include "service_manager.h"
#include "usercopy.h"
#include "object.h"
#include "process.h"
#include "thread.h"
#include "timer.h"
#include "intent.h"
#include "ipc.h"
#include "usercopy.h"
#include "pci.h"
#include "usb_msc.h"
#include "usb_dma.h"
#include "usb_xhci.h"
#include "usb_xhci_ring.h"
#include "usb_desc.h"
#include "driver.h"
#include "network.h"
#include "device_broker.h"
#include "display.h"
#include "ata_pio.h"
#include "partition.h"
#include "storage.h"
#include "fatfs_disk.h"
#include "fatfs_provider.h"
#include "vfs.h"
#include "prosenjitfs.h"
#include "vfs_provider.h"
#include "syscall.h"
#include "security_hardening.h"
#include "../include/abi/syscall.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define PROSENJIT_NATIVE_BOOT_MAGIC 0x534F5250u
#define VGA_ADDRESS 0xB8000u
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u
#define IDT_ENTRIES 256u
#define USER_DATA_VA PROSENJIT_USER_TEST_VA

static volatile uint16_t *const VGA = (uint16_t *)VGA_ADDRESS;

static struct pros_interrupt_frame scheduler_init_frame;
static struct pros_interrupt_frame scheduler_worker_frame;
static struct pros_process init_process;
static struct pros_process boot_spawn_child;
static struct pros_thread boot_spawn_thread;
static struct pros_ipc_channel user_test_channel;
static uint64_t user_test_handle;
static uint64_t boot_spawn_child_handle;
static const char *const boot_elf_argv[] = { "/init", "elf-runtime" };
static const char *const boot_elf_envp[] = { "PROS_ELF_RUNTIME=1" };
static uint64_t boot_elf_image_size;
static struct pros_process spawn_failure_process;
static struct pros_thread spawn_failure_thread;
static const uint8_t spawn_failure_image[16] = {0};

static volatile uint64_t scheduler_test_init_progress;
static volatile uint64_t scheduler_test_worker_progress;
static volatile uint64_t health_exception_count;
static volatile uint64_t health_page_fault_count;
static volatile uint64_t health_recovered_page_fault_count;
static volatile uint64_t health_last_exception_vector;
static volatile uint64_t health_last_exception_rip;
static volatile uint64_t health_previous_crash_sequence;
static volatile uint64_t health_previous_crash_rip;
static volatile uint8_t health_previous_crash_pending;
/* Prevent a fault in exception reporting from recursively re-entering it. */
static volatile uint8_t exception_dispatch_active;

/* Test-only snapshot proving recoverable #PF changes only the saved RIP. */
static volatile uint8_t usercopy_fault_frame_test_active;
static volatile uint64_t usercopy_fault_frame_test_address;
static volatile uint64_t usercopy_fault_frame_test_fixup_rip;
static volatile uint64_t usercopy_fault_frame_test_snapshot[7];

extern int copy_from_user_asm(void *destination, const void *source,
                              uint64_t size);

/* PCI bus_data pointers retained by the driver model must outlive this scope. */
static struct pros_pci_device pci_inventory[PROS_PCI_MAX_DEVICES];

/* xHCI controller and DMA owners must outlive every published hardware address. */
#define PROS_XHCI_MAX_ATTACHED 8u
static struct pros_xhci_controller xhci_attached_controllers[PROS_XHCI_MAX_ATTACHED];
static struct pros_xhci_dma_resources xhci_attached_dma[PROS_XHCI_MAX_ATTACHED];
static uint32_t xhci_attached_count;
static struct pros_storage_volume xhci_storage_volumes[PROS_STORAGE_MAX_VOLUMES];
static struct pros_vfs_mount xhci_storage_mounts[PROS_STORAGE_MAX_VOLUMES];



#define BOOT_ELF_IMAGE_CAP 65536u
static uint8_t boot_user_elf[BOOT_ELF_IMAGE_CAP] __attribute__((aligned(16)));

struct boot_elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));
struct boot_elf_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

static void scheduler_test_init_entry(void) {
    for (;;) {
        ++scheduler_test_init_progress;
        __asm__ volatile ("pause");
    }
}

static void scheduler_test_worker_entry(void) {
    for (;;) {
        ++scheduler_test_worker_progress;
        __asm__ volatile ("pause");
    }
}
static inline void io_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t io_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
#define COM1_PORT 0x3F8u

static uint16_t vga_entry(char c, uint8_t color) {
    return ((uint16_t)color << 8) | (uint8_t)c;
}
static void vga_clear(void) {
    for (uint32_t i = 0; i < VGA_COLUMNS * VGA_ROWS; ++i) VGA[i] = vga_entry(' ', 0x07);
}
static void vga_write(const char *text, uint8_t color) {
    uint32_t i = 0;
    while (text[i] != '\0' && i < VGA_COLUMNS * VGA_ROWS) {
        VGA[i] = vga_entry(text[i], color);
        ++i;
    }
}
static void serial_init(void) {
    io_out8(COM1_PORT + 1u, 0x00); io_out8(COM1_PORT + 3u, 0x80);
    io_out8(COM1_PORT + 0u, 0x03); io_out8(COM1_PORT + 1u, 0x00);
    io_out8(COM1_PORT + 3u, 0x03); io_out8(COM1_PORT + 2u, 0xC7);
    io_out8(COM1_PORT + 4u, 0x03);
}
static int serial_ready(void) { return (io_in8(COM1_PORT + 5u) & 0x20u) != 0; }
static void serial_putc(char c) {
    while (!serial_ready()) __asm__ volatile ("pause");
    io_out8(COM1_PORT, (uint8_t)c);
}
static void serial_write(const char *text) {
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\n') serial_putc('\r');
        serial_putc(text[i]);
    }
}
static void serial_hex64(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    serial_write("0x");
    for (int shift = 60; shift >= 0; shift -= 4) serial_putc(hex[(value >> shift) & 0xFu]);
}

static void native_storage_fail(const char *stage, int rc)
{
    serial_write("ProsenjitOS: ERROR native storage test ");
    serial_write(stage);
    serial_write(" rc=");
    serial_hex64((uint64_t)(int64_t)rc);
    serial_write("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

struct idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

static struct idt_gate idt[IDT_ENTRIES];
extern void idt_load(const struct idt_pointer *ptr);
extern void isr128(void);
extern void isr32(void);
extern void thread_enter_interrupt_frame(struct pros_interrupt_frame *frame);
extern void cpu_load_tss(void);
extern int cpu_set_kernel_stack(uint64_t stack_top);
extern uint64_t cpu_get_kernel_stack(void);
extern char stack_top[];
extern void thread_start_trampoline(void);
extern void user_test_entry(void);

struct gdtr_snapshot {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void debug_tss_descriptor(void) {
    struct gdtr_snapshot gdtr;
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    serial_write("ProsenjitOS: GDTR base = ");
    serial_hex64(gdtr.base);
    serial_write("\n");
    serial_write("ProsenjitOS: GDTR limit = ");
    serial_hex64(gdtr.limit);
    serial_write("\n");
    volatile uint64_t *gdt = (volatile uint64_t *)(uintptr_t)gdtr.base;
    serial_write("ProsenjitOS: TSS descriptor low = ");
    serial_hex64(gdt[5]);
    serial_write("\n");
    serial_write("ProsenjitOS: TSS descriptor high = ");
    serial_hex64(gdt[6]);
    serial_write("\n");
}
extern void user_mode_exit_trampoline(void);
extern char __user_text_start[];
extern char __user_text_end[];
extern char __kernel_text_start[];
extern char __kernel_rodata_start[];
#define ISR(n) extern void isr##n(void);
ISR(0) ISR(1) ISR(2) ISR(3) ISR(4) ISR(5) ISR(6) ISR(7)
ISR(8) ISR(9) ISR(10) ISR(11) ISR(12) ISR(13) ISR(14) ISR(15)
ISR(16) ISR(17) ISR(18) ISR(19) ISR(20) ISR(21) ISR(22) ISR(23)
ISR(24) ISR(25) ISR(26) ISR(27) ISR(28) ISR(29) ISR(30) ISR(31)

static void idt_set_gate(uint32_t n, void (*handler)(void), uint8_t ist) {
    uint64_t addr = (uint64_t)handler;
    idt[n].offset_low = (uint16_t)addr;
    idt[n].selector = 0x08;
    idt[n].ist = ist & 0x07u;
    idt[n].type_attr = 0x8E;
    idt[n].offset_mid = (uint16_t)(addr >> 16);
    idt[n].offset_high = (uint32_t)(addr >> 32);
    idt[n].zero = 0;
}
void idt_init(void) {
    static void (*const handlers[IDT_ENTRIES])(void) = {
        isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,isr12,isr13,isr14,isr15,
        isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31
    };
    for (uint32_t i = 0; i < IDT_ENTRIES; ++i) idt[i].type_attr = 0;
    for (uint32_t i = 0; i < 32; ++i) idt_set_gate(i, handlers[i], 0);
    idt_set_gate(2, handlers[2], 1); /* dedicated NMI stack */
    idt_set_gate(8, handlers[8], 2); /* dedicated double-fault stack */
    /*
     * Keep #GP/#PF on the normal CPL0 stack while debugging the user entry
     * path. If delivery itself fails, #DF will still have its dedicated IST.
     */
    idt_set_gate(13, handlers[13], 0);
    /* Page faults use their own IST stack so a damaged task/bootstrap stack
     * cannot recursively destroy the first fault report. */
    idt_set_gate(14, handlers[14], 3);
    idt_set_gate(32, isr32, 0);
    /* CPL3 syscall entry uses the scheduler-published per-thread RSP0.
     * Keep IST=0 so each syscall frame lives on its owning thread's kernel
     * stack rather than the shared IST1 stack. */
    idt_set_gate(128, isr128, 0);
    idt[128].type_attr = 0xEE;
    struct idt_pointer ptr = { (uint16_t)(sizeof(idt) - 1u), (uint64_t)idt };
    idt_load(&ptr);
}
/* Verify that live IDT metadata matches the raw-frame normalizer contract. */
static int idt_exception_ist_contract_selftest(void)
{
    for (uint32_t vector = 0u; vector < 32u; ++vector) {
        const uint8_t expected_ist =
            vector == 2u ? 1u : vector == 8u ? 2u : vector == 14u ? 3u : 0u;
        if (idt[vector].selector != 0x08u ||
            (idt[vector].ist & 0x07u) != expected_ist ||
            (idt[vector].type_attr & 0x8Fu) != 0x8Eu)
            return -1;
    }

    if (idt[32].selector != 0x08u || idt[32].ist != 0u ||
        idt[32].type_attr != 0x8Eu)
        return -2;
    if (idt[128].selector != 0x08u || idt[128].ist != 0u ||
        idt[128].type_attr != 0xEEu)
        return -3;
    return 0;
}

uint64_t kernel_exception_count(void) { return health_exception_count; }
uint64_t kernel_page_fault_count(void) { return health_page_fault_count; }
uint64_t kernel_recovered_page_fault_count(void) { return health_recovered_page_fault_count; }
uint64_t kernel_last_exception_vector(void) { return health_last_exception_vector; }
uint64_t kernel_last_exception_rip(void) { return health_last_exception_rip; }

void exception_dispatch(uint64_t *frame) {
    /* A nested exception means this handler or its reporting path faulted. Stop
     * immediately rather than recursively corrupting the same exception frame. */
    if (exception_dispatch_active) {
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }
    exception_dispatch_active = 1u;
    ++health_exception_count;
    health_last_exception_vector = frame[0];
    health_last_exception_rip = frame[2];
    uint64_t vector = frame[0];
    if (vector == 14u) ++health_page_fault_count;

    /*
     * Recoverable user-copy faults must be fixed up before serial, filesystem,
     * health, or crash-reporting work. The active flag is published with IRQs
     * disabled by usercopy.asm, so this discriminator is stable in the
     * current single-CPU execution model.
     */
    if (vector == 14u && usercopy_fault_active()) {
        uint64_t fixup_rip = usercopy_fault_rip();
        if (usercopy_fault_frame_test_active) {
            usercopy_fault_frame_test_address = (uint64_t)(uintptr_t)frame;
            usercopy_fault_frame_test_fixup_rip = fixup_rip;
            for (uint32_t i = 0u; i < 7u; ++i)
                usercopy_fault_frame_test_snapshot[i] = frame[i];
        }
        ++health_recovered_page_fault_count;
        usercopy_fault_clear();
        frame[2] = fixup_rip;
        exception_dispatch_active = 0u;
        return;
    }

    /* CR2 is architecturally meaningful only for a page-fault (#PF).
     * Keep non-#PF crash records explicit instead of leaking a stale address. */
    uint64_t cr2 = 0u;
    if (vector == 14u)
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    /*
     * Capture the minimal fatal record before optional diagnostics. This API
     * is RAM-only and bounded; block/ATA persistence remains prohibited here.
     */
    int crash_rc = PROS_CRASH_RECORD_ENOTSUP;
    if (vector != 128u) {
        crash_rc = pros_crash_record_capture(vector, frame[1], frame[2],
                                               cr2, timer_ticks());
    }

    serial_write("ProsenjitOS: CPU exception vector ");
    serial_hex64(vector);
    serial_write("\n");
    serial_write("ProsenjitOS: exception error code = ");
    serial_hex64(frame[1]);
    serial_write("\n");
    serial_write("ProsenjitOS: exception RIP = ");
    serial_hex64(frame[2]);
    serial_write("\n");
    {
        uint32_t pfs_stage = 0u;
        int pfs_rc = 0;
        prosenjitfs_selftest_diag_get(&pfs_stage, &pfs_rc);
        serial_write("ProsenjitOS: ProsenjitFS diagnostic stage = ");
        serial_hex64(pfs_stage);
        serial_write(" rc = ");
        serial_hex64((uint64_t)(int64_t)pfs_rc);
        serial_write("\n");
    }
    if (vector != 128u) {
        if (crash_rc != PROS_CRASH_RECORD_OK)
            (void)pros_health_record_recovery(PROS_CRASH_HEALTH_SOURCE,
                                              PROS_CRASH_HEALTH_CAPTURE_FAILED,
                                              (uint64_t)(uint32_t)(-crash_rc));
        (void)pros_health_record_exception(vector, frame[1], frame[2], cr2);
    }
    serial_write("ProsenjitOS: CR2 = ");
    serial_hex64(cr2);
    serial_write("\n");
    if (vector == 128u) {
        if (((volatile uint8_t *)(uintptr_t)USER_DATA_VA)[0] != 0x5A) {
            serial_write("ProsenjitOS: ERROR user-mode write verification failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        serial_write("ProsenjitOS: CPL3 syscall returned to kernel successfully\n");
        if (paging_switch_kernel() != 0) {
            serial_write("ProsenjitOS: ERROR kernel address-space restore failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        frame[2] = (uint64_t)(uintptr_t)user_mode_exit_trampoline;
        frame[3] = 0x08;
        /* exception_dispatch's frame is: vector,error,RIP,CS,RFLAGS,RSP,SS */
        frame[5] = (uint64_t)(uintptr_t)(frame + 7);
        frame[6] = 0x10;
        exception_dispatch_active = 0u;
        return;
    }
    vga_write("ProsenjitOS: CPU exception - system halted", 0x4F);
    for (;;) __asm__ volatile ("cli; hlt");
}

void user_mode_test_complete(void) {
    int32_t child_exit = 0;
    if (process_current() != 0 || thread_current() != 0 ||
        process_state(&boot_spawn_child) != PROS_PROCESS_ZOMBIE ||
        boot_spawn_child.exit_code != 42 ||
        boot_spawn_thread.state != PROS_THREAD_DEAD) {
        serial_write("ProsenjitOS: ERROR native spawned process exit failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_wait(&init_process, boot_spawn_child.pid, &child_exit) != 0 ||
        child_exit != 42 || process_lookup(boot_spawn_child.pid) != 0 ||
        boot_spawn_child.address_space.root != 0 ||
        boot_spawn_thread.kernel_stack_base != 0) {
        serial_write("ProsenjitOS: ERROR native spawned process wait/reap failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: native user SYS_EXIT process transition passed\n");
    serial_write("ProsenjitOS: scheduler-created native user process launch passed\n");
    serial_write("ProsenjitOS: native spawn exit/wait/reap transaction passed\n");
    serial_write("ProsenjitOS: CPL3 user-mode and IPC syscall test passed\n");
    vga_write("ProsenjitOS: CPL3 user mode online", 0x0F);

    /*
     * Last boot-test gate: establish a first owner through the reversible
     * isolated entry, then invoke it again. The second entry must take the
     * same nested-owner cli/hlt branch used by the real handoff. QEMU captures
     * a test-only debugcon marker from inside that branch before it halts.
     */
    {
        struct pros_panic_frame nested_frame = {0};
        nested_frame.version = PROS_PANIC_FRAME_VERSION;
        nested_frame.size = sizeof(nested_frame);
        nested_frame.vector = 13u;
        nested_frame.rip = 0xFFFFFFFF80003000ULL;
        nested_frame.cs = 0x08u;
        nested_frame.rflags = 0x202u;
        pros_panic_handoff_reset_test();
        pros_panic_handoff_test_return_mode = 1u;
        if (pros_panic_handoff_enter_test(&nested_frame) != 0) {
            pros_panic_handoff_test_return_mode = 0u;
            serial_write("ProsenjitOS: ERROR nested panic test first-owner setup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        pros_panic_handoff_test_return_mode = 0u;
        serial_write("ProsenjitOS: entering duplicate panic-owner fail-stop regression\\n");
        pros_panic_handoff_test_nested_entry(&nested_frame);
        serial_write("ProsenjitOS: ERROR duplicate panic-owner path returned\n");
    }
    for (;;) __asm__ volatile ("cli; hlt");
}


static int usercopy_fault_frame_selftest(void)
{
    uint8_t destination = 0u;
    const uint64_t invalid_user_source = 0x00007FFFFFFFF000ULL;
    uint64_t page_faults_before = health_page_fault_count;
    uint64_t recovered_before = health_recovered_page_fault_count;

    usercopy_fault_frame_test_address = 0u;
    usercopy_fault_frame_test_fixup_rip = 0u;
    usercopy_fault_frame_test_active = 1u;
    int rc = copy_from_user_asm(&destination,
        (const void *)(uintptr_t)invalid_user_source, 1u);
    usercopy_fault_frame_test_active = 0u;

    uint64_t *raw = (uint64_t *)(uintptr_t)usercopy_fault_frame_test_address;
    if (rc != -1 || usercopy_fault_frame_test_address == 0u ||
        usercopy_fault_frame_test_fixup_rip == 0u ||
        health_page_fault_count != page_faults_before + 1u ||
        health_recovered_page_fault_count != recovered_before + 1u ||
        usercopy_fault_active() != 0u || usercopy_fault_rip() != 0u ||
        raw[0] != 14u || raw[3] != 0x08u || raw[4] !=
            usercopy_fault_frame_test_snapshot[4] ||
        raw[5] == 0u || raw[6] != 0x10u ||
        raw[2] != usercopy_fault_frame_test_fixup_rip)
        return -1;

    /* The legacy IST3 frame is intact except for its documented RIP fixup. */
    for (uint32_t i = 0u; i < 7u; ++i) {
        if (i == 2u)
            continue;
        if (raw[i] != usercopy_fault_frame_test_snapshot[i])
            return -2;
    }
    if (raw[2] == usercopy_fault_frame_test_snapshot[2])
        return -3;
    return 0;
}

static int xhci_usb_fatfs_persistence_test(struct pros_usb_msc *msc)
{
    static const char payload[] = "ProsenjitOS USB FAT32/exFAT persistence test";
    static const char path[] = "/xhci-storage-test.txt";
    struct pros_vfs_file file;
    uint32_t count = 0u, tested = 0u;
    uint8_t readback[sizeof(payload)];
    int rc;
    if (!msc || !msc->initialized)
        return PROS_STORAGE_EINVAL;

    rc = pros_storage_discover(&msc->block, xhci_storage_volumes,
                               PROS_STORAGE_MAX_VOLUMES, &count);
    if (rc != PROS_STORAGE_OK || count == 0u)
        return rc == PROS_STORAGE_OK ? PROS_STORAGE_ENOTSUP : rc;

    /*
     * Exercise every partition accepted by the registered FAT32/exFAT
     * provider. A USB disk may contain both formats; stopping at the first
     * provider volume silently skipped the second filesystem.
     */
    for (uint32_t i = 0u; i < count; ++i) {
        struct pros_storage_volume *volume = &xhci_storage_volumes[i];
        struct pros_vfs_mount *mount = &xhci_storage_mounts[i];
        uint32_t written = 0u, read_bytes = 0u;
        uint8_t mount_active = 0u, file_open = 0u;

        if (volume->filesystem != PROS_FS_PROVIDER)
            continue;

        serial_write("ProsenjitOS: USB FAT32/exFAT provider partition; start LBA = ");
        serial_hex64(volume->partition.first_lba);
        serial_write("; sectors = ");
        serial_hex64(volume->partition.sector_count);
        serial_write("\n");

        rc = pros_storage_mount_volume(volume, mount, 0u);
        if (rc != PROS_STORAGE_OK)
            return rc;
        mount_active = 1u;

        rc = vfs_kernel_file_open(mount, path,
            PROS_VFS_O_READ | PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file);
        if (rc != PROS_VFS_OK)
            goto failed;
        file_open = 1u;
        rc = vfs_file_write(&file, payload,
                            (uint32_t)(sizeof(payload) - 1u), &written);
        if (rc != PROS_VFS_OK || written != sizeof(payload) - 1u)
            goto failed;
        rc = vfs_file_close(&file);
        if (rc != PROS_VFS_OK)
            goto failed;
        file_open = 0u;

        rc = pros_storage_unmount_volume(mount);
        if (rc != PROS_STORAGE_OK)
            goto failed;
        mount_active = 0u;

        rc = pros_storage_mount_volume(volume, mount, 0u);
        if (rc != PROS_STORAGE_OK)
            goto failed;
        mount_active = 1u;
        rc = vfs_kernel_file_open(mount, path, PROS_VFS_O_READ, &file);
        if (rc != PROS_VFS_OK)
            goto failed;
        file_open = 1u;
        rc = vfs_file_read(&file, readback,
                           (uint32_t)(sizeof(payload) - 1u), &read_bytes);
        if (rc != PROS_VFS_OK || read_bytes != sizeof(payload) - 1u)
            goto failed;
        rc = vfs_file_close(&file);
        if (rc != PROS_VFS_OK)
            goto failed;
        file_open = 0u;
        for (uint32_t j = 0u; j < sizeof(payload) - 1u; ++j)
            if (readback[j] != (uint8_t)payload[j])
                goto failed;

        rc = pros_storage_unmount_volume(mount);
        if (rc != PROS_STORAGE_OK)
            goto failed;
        mount_active = 0u;
        ++tested;
        serial_write("ProsenjitOS: USB filesystem write/flush/unmount/remount/readback passed; partition = ");
        serial_hex64(i);
        serial_write("\n");
        continue;

failed:
        if (file_open)
            (void)vfs_file_close(&file);
        if (mount_active)
            (void)pros_storage_unmount_volume(mount);
        return rc == PROS_VFS_OK ? PROS_STORAGE_EIO : rc;
    }

    serial_write("ProsenjitOS: USB FAT32/exFAT partitions tested = ");
    serial_hex64(tested);
    serial_write("\n");
    return tested != 0u ? PROS_STORAGE_OK : PROS_STORAGE_ENOTSUP;
}

void kernel_main(uint32_t boot_magic, uint32_t boot_info) {
    serial_init();
    serial_write("ProsenjitOS: early kernel entry\n");
    serial_write("ProsenjitOS: boot magic = "); serial_hex64(boot_magic); serial_write("\n");
    vga_clear();
    if (boot_magic != MULTIBOOT2_BOOT_MAGIC && boot_magic != PROSENJIT_NATIVE_BOOT_MAGIC) {
        vga_write("ProsenjitOS: invalid boot handoff", 0x4F);
        serial_write("ProsenjitOS: ERROR invalid boot handoff\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (boot_magic == PROSENJIT_NATIVE_BOOT_MAGIC) {
        serial_write("ProsenjitOS: native bootloader handoff accepted\n");
    } else {
        serial_write("ProsenjitOS: Multiboot2/GRUB handoff accepted\n");
    }
    int panic_selftest_rc = pros_panic_handoff_selftest();
    if (panic_selftest_rc != 0) {
        serial_write("ProsenjitOS: ERROR isolated panic frame/owner selftest failed rc=");
        serial_hex64((uint64_t)(int64_t)panic_selftest_rc);
        serial_write("\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: isolated panic frame validation/owner selftest passed\n");
    idt_init();
    int idt_contract_rc = idt_exception_ist_contract_selftest();
    if (idt_contract_rc != 0) {
        serial_write("ProsenjitOS: ERROR IDT exception/IST contract failed rc=");
        serial_hex64((uint64_t)(int64_t)idt_contract_rc);
        serial_write("\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: IDT exception/IST contract passed\n");
    __asm__ volatile ("cli");
    serial_write("ProsenjitOS: IDT loaded; attempting LTR\n");
    debug_tss_descriptor();
    cpu_load_tss();
    serial_write("ProsenjitOS: TSS loaded successfully\n");
    {
        uint64_t bootstrap_rsp0 = (uint64_t)(uintptr_t)stack_top;
        if (!bootstrap_rsp0 ||
            cpu_get_kernel_stack() != bootstrap_rsp0 ||
            cpu_set_kernel_stack(0) == 0 ||
            cpu_get_kernel_stack() != bootstrap_rsp0 ||
            cpu_set_kernel_stack(bootstrap_rsp0 + 1u) == 0 ||
            cpu_get_kernel_stack() != bootstrap_rsp0) {
            serial_write("ProsenjitOS: ERROR TSS RSP0 setter validation failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: TSS RSP0 setter validation passed\n");
    if (timer_init(100) != 0) {
        serial_write("ProsenjitOS: ERROR PIT timer initialization failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: PIT timer initialized at 100 Hz\n");
    serial_write("ProsenjitOS: IDT loaded; exceptions armed; hardware IRQs masked\n");
    serial_write("ProsenjitOS: Multiboot2 info = "); serial_hex64(boot_info); serial_write("\n");
    serial_write("ProsenjitOS: x86-64 long mode active\n");
    if (physical_memory_init((uint64_t)boot_info) != 0) {
        serial_write("ProsenjitOS: ERROR physical memory initialization failed\n");
        vga_write("ProsenjitOS: memory initialization failed", 0x4F);
        for (;;) __asm__ volatile ("cli; hlt");
    }

    serial_write("ProsenjitOS: physical memory manager online\n");
    serial_write("ProsenjitOS: free pages = ");
    serial_hex64(physical_memory_free_pages());
    serial_write("\n");

    uint64_t test_page = physical_page_alloc();
    if (test_page == 0) {
        serial_write("ProsenjitOS: ERROR page allocation test failed\n");
        vga_write("ProsenjitOS: page allocation failed", 0x4F);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: allocated test page = ");
    serial_hex64(test_page);
    serial_write("\n");
    physical_page_free(test_page);
    serial_write("ProsenjitOS: page allocation/free test passed\n");
    if (physical_memory_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR physical memory deep self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: physical memory deep invariant self-test passed\n");

    if (paging_init() != 0) {
        serial_write("ProsenjitOS: ERROR virtual memory initialization failed\n");
        vga_write("ProsenjitOS: paging initialization failed", 0x4F);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (percpu_init() != 0) {
        serial_write("ProsenjitOS: ERROR per-CPU usercopy fault state initialization failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: per-CPU usercopy fault state online\n");

    if (usercopy_fault_frame_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR recoverable usercopy #PF frame-preservation test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: recoverable usercopy #PF preserved original IST3 frame except RIP fixup\n");

    /*
     * paging_init() uses PROSENJIT_KERNEL_TEST_VA as an internal smoke-test
     * mapping and deliberately unmaps/frees it before returning. Do not
     * translate that VA here: a zero result is the expected post-cleanup
     * state. The persistent paging contract is verified by paging_selftest().
     */
    serial_write("ProsenjitOS: paging active; init mapping cleanup verified\n");
    if (paging_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR paging deep self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: paging deep invariant self-test passed\n");

    if (heap_init() != 0) {
        serial_write("ProsenjitOS: ERROR kernel heap initialization failed\n");
        vga_write("ProsenjitOS: heap initialization failed", 0x4F);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    void *heap_test = kmalloc(128);
    if (!heap_test) {
        serial_write("ProsenjitOS: ERROR kernel heap allocation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    ((volatile uint8_t *)heap_test)[0] = 0xA5;
    if (((volatile uint8_t *)heap_test)[0] != 0xA5) {
        serial_write("ProsenjitOS: ERROR kernel heap memory test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    kfree(heap_test);
    serial_write("ProsenjitOS: kernel heap allocation/free test passed\n");
    if (heap_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR kernel heap deep self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: kernel heap deep invariant self-test passed\n");
    if (address_space_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR address-space deep self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: address-space deep isolation self-test passed\n");
    if (pros_log_selftest() != PROS_LOG_OK) {
        serial_write("ProsenjitOS: ERROR central logging self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (pros_health_sample() != PROS_HEALTH_OK) {
        serial_write("ProsenjitOS: ERROR initial health sample failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (pros_crash_record_selftest() != PROS_CRASH_RECORD_OK) {
        serial_write("ProsenjitOS: ERROR persistent crash record self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: persistent crash record self-test passed\n");
    int elf_selftest_rc = pros_elf_selftest();
    if (elf_selftest_rc != PROS_ELF_OK) {
        serial_write("ProsenjitOS: ERROR ELF validation negative self-test rc = ");
        serial_hex64((uint64_t)(int64_t)elf_selftest_rc);
        serial_write("\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: ELF validation negative self-test passed\n");
    if (pros_init_selftest() != PROS_INIT_OK) {
        serial_write("ProsenjitOS: ERROR /init filesystem-boundary self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: /init filesystem-boundary negative self-test passed\n");

    if (block_device_selftest() != PROS_BLOCK_OK) {
        serial_write("ProsenjitOS: ERROR block storage health self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (block_device_registry_selftest() != PROS_BLOCK_OK) {
        serial_write("ProsenjitOS: ERROR block-device registry self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: block storage and registry self-tests passed\n");
    if (pros_fatfs_disk_selftest() != PROS_BLOCK_OK) {
        serial_write("ProsenjitOS: ERROR FatFs block adapter self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: FatFs block adapter self-test passed\n");
    serial_write("ProsenjitOS: system health monitor initialized\n");
    (void)pros_log_emit(PROS_HEALTH_INFO, PROS_HEALTH_RECOVERY,
                        0x52454356u, PROS_HEALTH_RECOVERY_BOOT_BEGIN, timer_ticks());
    if (pros_health_watchdog_register(1u, 3000u) != PROS_HEALTH_OK) {
        serial_write("ProsenjitOS: ERROR kernel health watchdog registration failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    (void)pros_health_watchdog_kick(1u);

    if (process_manager_init() != 0 ||
        process_init(&init_process, 1, PROS_SECURITY_LIMITED) != 0 ||
        process_register(&init_process) != 0 ||
        process_lookup(1) != &init_process) {
        serial_write("ProsenjitOS: ERROR process manager initialization failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (ipc_channel_integrity_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR IPC waiter-count integrity self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: IPC waiter-count corruption guard test passed\n");
    if (ipc_channel_init(&user_test_channel) != PROSENJIT_IPC_OK ||
        process_grant_handle(&init_process, &user_test_channel.object,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_WRITE |
                             PROSENJIT_CAP_DUPLICATE | PROSENJIT_CAP_TRANSFER,
                             &user_test_handle) != 0) {
        serial_write("ProsenjitOS: ERROR initial IPC test handle grant failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&init_process);
    if (syscall_selftest(&init_process) != 0) {
        serial_write("ProsenjitOS: ERROR syscall dispatcher runtime self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: syscall dispatcher ABI/policy self-test passed\n");
    if (process_state(&init_process) != PROS_PROCESS_NEW ||
        process_set_state(&init_process, PROS_PROCESS_RUNNING) != 0 ||
        process_state(&init_process) != PROS_PROCESS_RUNNING) {
        serial_write("ProsenjitOS: ERROR initial process lifecycle test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    int spawn_rollback_rc = process_spawn(&init_process,
                                         &spawn_failure_process,
                                         &spawn_failure_thread,
                                         0xB003u, 0xB003u,
                                         PROS_SECURITY_LIMITED,
                                         spawn_failure_image,
                                         sizeof(spawn_failure_image),
                                         0, 0, 0, 0);
    if (spawn_rollback_rc == 0 || process_lookup(0xB003u) != 0 ||
        spawn_failure_process.address_space.root != 0 ||
        spawn_failure_process.object.refcount != 0 ||
        spawn_failure_thread.kernel_stack_base != 0) {
        serial_write("ProsenjitOS: ERROR native process spawn rollback test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: native process spawn invalid-image rollback passed\n");

    struct pros_process child_process;
    if (process_create(&init_process, &child_process, 2,
                       PROS_SECURITY_LIMITED) != 0 ||
        child_process.parent_pid != init_process.pid ||
        process_state(&child_process) != PROS_PROCESS_READY ||
        process_set_state(&child_process, PROS_PROCESS_RUNNING) != 0 ||
        process_state(&child_process) != PROS_PROCESS_RUNNING) {
        serial_write("ProsenjitOS: ERROR child process lifecycle test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_exit(&child_process, 42) != 0 ||
        process_state(&child_process) != PROS_PROCESS_ZOMBIE) {
        serial_write("ProsenjitOS: ERROR process exit test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    struct pros_process duplicate_pid_process;
    if (process_create(&init_process, &duplicate_pid_process, child_process.pid,
                       PROS_SECURITY_LIMITED) == 0) {
        serial_write("ProsenjitOS: ERROR duplicate live/zombie PID was accepted\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    int32_t waited_exit = 0;
    if (process_wait(&init_process, child_process.pid, &waited_exit) != 0 ||
        waited_exit != 42 ||
        process_lookup(child_process.pid) != 0) {
        serial_write("ProsenjitOS: ERROR process wait/reap test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process lifecycle/parent/exit-wait test passed\n");
    /* Lifecycle regressions: zombie parent and current-process clearing. */
    struct pros_process lifecycle_parent;
    if (process_create(&init_process, &lifecycle_parent, 3,
                       PROS_SECURITY_LIMITED) != 0 ||
        process_unregister(&lifecycle_parent) != -1 ||
        process_exit(&lifecycle_parent, 7) != 0 ||
        process_wait(&lifecycle_parent, child_process.pid, &waited_exit) != -1) {
        serial_write("ProsenjitOS: ERROR process state validation regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&lifecycle_parent);
    if (process_current() != 0) {
        serial_write("ProsenjitOS: ERROR zombie process became current\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_wait(&init_process, lifecycle_parent.pid, &waited_exit) != 0 ||
        waited_exit != 7 ||
        lifecycle_parent.address_space.root != 0 ||
        process_lookup(lifecycle_parent.pid) != 0) {
        serial_write("ProsenjitOS: ERROR zombie process reap validation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_unregister(&lifecycle_parent) != -1) {
        serial_write("ProsenjitOS: ERROR unreaped process unregister was accepted\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&init_process);
    serial_write("ProsenjitOS: process state validation regression passed\n");

    struct pros_object lifecycle_object;
    uint64_t lifecycle_handle = PROSENJIT_HANDLE_INVALID;
    uint64_t lifecycle_free_before = physical_memory_free_pages();
    uint64_t lifecycle_user_page = physical_page_alloc_user();
    object_init(&lifecycle_object, PROS_OBJECT_FILE);
    if (!lifecycle_user_page ||
        process_create(&init_process, &lifecycle_parent, 6,
                       PROS_SECURITY_LIMITED) != 0 ||
        address_space_map_user_page_owned(&lifecycle_parent.address_space,
                                          PROSENJIT_USER_TEST_VA,
                                          lifecycle_user_page,
                                          PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        address_space_map_user_page_owned(&lifecycle_parent.address_space,
                                          PROSENJIT_USER_TEST_VA + PROSENJIT_PAGE_SIZE,
                                          lifecycle_user_page,
                                          PROSENJIT_PAGE_WRITABLE) == 0 ||
        process_grant_handle(&lifecycle_parent, &lifecycle_object,
                             PROSENJIT_CAP_READ, &lifecycle_handle) != 0 ||
        lifecycle_object.refcount != 2 ||
        process_exit(&lifecycle_parent, 13) != 0 ||
        process_wait(&init_process, lifecycle_parent.pid, &waited_exit) != 0 ||
        waited_exit != 13 ||
        lifecycle_object.refcount != 1 ||
        lifecycle_parent.address_space.root != 0) {
        serial_write("ProsenjitOS: ERROR process resource cleanup regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (physical_memory_free_pages() != lifecycle_free_before) {
        serial_write("ProsenjitOS: ERROR address-space page-table cleanup regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process resource cleanup regression passed\n");

    /* Address-space switch/reap regression. */
    struct pros_process as_switch_child;
    if (process_create(&init_process, &as_switch_child, 7, PROS_SECURITY_LIMITED) != 0) {
        serial_write("ProsenjitOS: ERROR address-space switch child creation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&as_switch_child);
    if (process_current() != &as_switch_child) {
        serial_write("ProsenjitOS: ERROR child address-space switch failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_exit(&as_switch_child, 17) != 0 || process_current() != 0) {
        serial_write("ProsenjitOS: ERROR child exit did not clear current address space\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&init_process);
    if (process_current() != &init_process) {
        serial_write("ProsenjitOS: ERROR parent address-space restore failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_wait(&init_process, as_switch_child.pid, &waited_exit) != 0 ||
        waited_exit != 17 || as_switch_child.address_space.root != 0) {
        serial_write("ProsenjitOS: ERROR address-space switch/reap regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: address-space switch/reap regression passed\n");

    struct pros_process child_lifetime_parent;
    struct pros_process child_lifetime_child;
    if (process_create(&init_process, &child_lifetime_parent, 8, PROS_SECURITY_LIMITED) != 0 ||
        process_create(&child_lifetime_parent, &child_lifetime_child, 9, PROS_SECURITY_LIMITED) != 0 ||
        process_exit(&child_lifetime_parent, 1) == 0) {
        serial_write("ProsenjitOS: ERROR parent-with-live-child exit was accepted\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_exit(&child_lifetime_child, 2) != 0 ||
        process_wait(&child_lifetime_parent, child_lifetime_child.pid, &waited_exit) != 0 ||
        waited_exit != 2 || process_exit(&child_lifetime_parent, 1) != 0 ||
        process_wait(&init_process, child_lifetime_parent.pid, &waited_exit) != 0 ||
        waited_exit != 1) {
        serial_write("ProsenjitOS: ERROR parent/child lifetime ordering regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: parent/child lifetime ordering regression passed\n");

    struct pros_thread init_thread;
    struct pros_thread worker_thread;
    if (thread_manager_init() != 0 ||
        thread_kernel_selftest() != 0 ||
        thread_init(&init_thread, &init_process, 1,
                    (uint64_t)(uintptr_t)scheduler_test_init_entry) != 0 ||
        thread_init(&worker_thread, &init_process, 2,
                    (uint64_t)(uintptr_t)scheduler_test_worker_entry) != 0 ||
        thread_set_state(&init_thread, PROS_THREAD_READY) != 0 ||
        thread_set_state(&worker_thread, PROS_THREAD_READY) != 0 ||
        thread_enqueue(&init_thread) != 0 ||
        thread_enqueue(&worker_thread) != 0 ||
        /* Regression: changing a queued thread to BLOCKED must not leave a
         * blocked entry linked into the runnable queue. */
        thread_set_state(&worker_thread, PROS_THREAD_BLOCKED) == 0 ||
        thread_state(&worker_thread) != PROS_THREAD_READY ||
        thread_schedule() != 0 ||
        thread_current() != &init_thread ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING ||
        cpu_get_kernel_stack() !=
            (uint64_t)(uintptr_t)init_thread.kernel_stack_base +
            init_thread.kernel_stack_size ||
        thread_schedule() != 0 ||
        thread_current() != &worker_thread ||
        thread_state(&worker_thread) != PROS_THREAD_RUNNING ||
        cpu_get_kernel_stack() !=
            (uint64_t)(uintptr_t)worker_thread.kernel_stack_base +
            worker_thread.kernel_stack_size ||
        thread_schedule() != 0 ||
        thread_current() != &init_thread ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING ||
        cpu_get_kernel_stack() !=
            (uint64_t)(uintptr_t)init_thread.kernel_stack_base +
            init_thread.kernel_stack_size) {
        serial_write("ProsenjitOS: ERROR thread scheduler foundation test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: kernel-owned thread ownership/queue/CR3 self-test passed\n");
    for (struct pros_thread *guard_thread = &init_thread;
         guard_thread; guard_thread = (guard_thread == &init_thread ? &worker_thread : 0)) {
        uint64_t stack_base = (uint64_t)(uintptr_t)guard_thread->kernel_stack_base;
        uint64_t slot_base = stack_base - PROSENJIT_PAGE_SIZE;
        uint64_t flags = 0u;
        int stack_pages_valid = 1;
        for (uint32_t page = 0; page < 4u; ++page) {
            if (paging_translate(stack_base + (uint64_t)page * PROSENJIT_PAGE_SIZE) !=
                guard_thread->kernel_stack_pages[page]) stack_pages_valid = 0;
        }
        if (!stack_pages_valid || paging_translate(slot_base) != 0u ||
            paging_translate(stack_base + guard_thread->kernel_stack_size) != 0u ||
            paging_query_page(stack_base, &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_PRESENT) ||
            !(flags & PROSENJIT_PAGE_WRITABLE) ||
            !(flags & PROSENJIT_PAGE_NX) ||
            (flags & PROSENJIT_PAGE_USER) != 0u ||
            paging_translate(stack_base + guard_thread->kernel_stack_size -
                             PROSENJIT_PAGE_SIZE) != guard_thread->kernel_stack_pages[3]) {
            serial_write("ProsenjitOS: ERROR kernel stack guard-page mapping failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: kernel thread stack guard-page mapping passed\n");
    serial_write("ProsenjitOS: thread lifecycle/run-queue scheduler test passed\n");
    serial_write("ProsenjitOS: TSS RSP0 follows cooperative scheduler switches passed\n");

    /* Reinitializing an object already linked into the registry must fail
     * without changing its scheduler state or registry linkage. */
    if (thread_init(&init_thread, &init_process, 99,
                    (uint64_t)(uintptr_t)scheduler_test_init_entry) == 0 ||
        thread_current() != &init_thread ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING ||
        thread_state(&worker_thread) != PROS_THREAD_READY) {
        serial_write("ProsenjitOS: ERROR registered-thread reinitialization regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: registered-thread reinitialization guard passed\n");

    /*
     * Regression: a process may become blocked or quarantined after one of
     * its threads has already entered the run queue. Stale queue entries
     * must become BLOCKED rather than DEAD. With no idle thread yet, the
     * cooperative scheduler must leave the current thread authoritative.
     */
    struct pros_process scheduler_blocked_owner;
    if (process_init(&scheduler_blocked_owner, 0xB101u,
                     PROS_SECURITY_LIMITED) != 0 ||
        thread_dequeue() != &worker_thread) {
        serial_write("ProsenjitOS: ERROR scheduler blocked-owner setup failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    worker_thread.process = &scheduler_blocked_owner;
    worker_thread.state = PROS_THREAD_READY;
    scheduler_blocked_owner.state = PROS_PROCESS_READY;
    if (thread_enqueue(&worker_thread) != 0) {
        serial_write("ProsenjitOS: ERROR scheduler blocked-process enqueue setup failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    scheduler_blocked_owner.state = PROS_PROCESS_BLOCKED;
    if (thread_schedule() == 0 ||
        thread_current() != &init_thread ||
        thread_state(&worker_thread) != PROS_THREAD_BLOCKED) {
        serial_write("ProsenjitOS: ERROR scheduler blocked-process regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* A quarantined READY owner must be treated as blocked, not dead. */
    worker_thread.state = PROS_THREAD_READY;
    scheduler_blocked_owner.state = PROS_PROCESS_READY;
    scheduler_blocked_owner.quarantined = 0u;
    if (thread_enqueue(&worker_thread) != 0) {
        serial_write("ProsenjitOS: ERROR scheduler quarantine enqueue setup failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    scheduler_blocked_owner.quarantined = 1u;
    if (thread_schedule() == 0 ||
        thread_current() != &init_thread ||
        thread_state(&worker_thread) != PROS_THREAD_BLOCKED) {
        serial_write("ProsenjitOS: ERROR scheduler quarantined-process regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    worker_thread.process = &init_process;
    worker_thread.state = PROS_THREAD_READY;
    if (thread_enqueue(&worker_thread) != 0 ||
        thread_schedule() != 0 ||
        thread_current() != &worker_thread ||
        thread_state(&worker_thread) != PROS_THREAD_RUNNING ||
        address_space_destroy(&scheduler_blocked_owner.address_space) != 0 ||
        /* Restore the scheduler's expected init-thread context for the IPC
         * wait/wakeup regressions that follow this eligibility test. */
        thread_schedule() != 0 ||
        thread_current() != &init_thread ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING) {
        serial_write("ProsenjitOS: ERROR scheduler eligibility recovery regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: blocked/quarantined process scheduler regression passed\n");

    /* Process exit must terminate its threads and remove them from the run queue. */
    struct pros_process thread_lifecycle_process;
    struct pros_thread thread_lifecycle;
    struct pros_thread thread_lifecycle_second;
    struct pros_object reap_probe;
    uint64_t reap_probe_handle = PROSENJIT_HANDLE_INVALID;
    if (process_create(&init_process, &thread_lifecycle_process, 10, PROS_SECURITY_LIMITED) != 0 ||
        thread_init(&thread_lifecycle, &thread_lifecycle_process, 10,
                    (uint64_t)(uintptr_t)scheduler_test_worker_entry) != 0 ||
        thread_init(&thread_lifecycle_second, &thread_lifecycle_process, 13,
                    (uint64_t)(uintptr_t)scheduler_test_worker_entry) != 0 ||
        thread_set_state(&thread_lifecycle, PROS_THREAD_READY) != 0 ||
        thread_set_state(&thread_lifecycle_second, PROS_THREAD_READY) != 0 ||
        thread_enqueue(&thread_lifecycle) != 0 ||
        thread_enqueue(&thread_lifecycle_second) != 0 ||
        process_exit(&thread_lifecycle_process, 21) != 0 ||
        thread_state(&thread_lifecycle) != PROS_THREAD_DEAD ||
        thread_state(&thread_lifecycle_second) != PROS_THREAD_DEAD ||
        thread_has_runnable() != 1) {
        serial_write("ProsenjitOS: ERROR process-thread teardown regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* Give the lifecycle child a real owned user page so the wait-path
     * ownership-corruption regression exercises a populated ownership list. */
    uint64_t lifecycle_owned_page = physical_page_alloc_user();
    if (!lifecycle_owned_page ||
        address_space_map_user_page_owned(&thread_lifecycle_process.address_space,
                                          PROSENJIT_USER_TEST_VA,
                                          lifecycle_owned_page,
                                          PROSENJIT_PAGE_WRITABLE |
                                          PROSENJIT_PAGE_NX) != 0) {
        if (lifecycle_owned_page)
            physical_page_free(lifecycle_owned_page);
        serial_write("ProsenjitOS: ERROR process-reap owned-page fixture setup failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    object_init(&reap_probe, PROS_OBJECT_FILE);
    reap_probe_handle = handle_alloc(&thread_lifecycle_process.handles,
                                     &reap_probe, PROSENJIT_CAP_READ);
    if (reap_probe_handle == PROSENJIT_HANDLE_INVALID ||
        reap_probe.refcount != 2u) {
        serial_write("ProsenjitOS: ERROR process-reap handle fixture setup failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* A reference held by the parent must prevent destructive child teardown. */
    {
        uint64_t external_process_handle =
            handle_alloc(&init_process.handles, &thread_lifecycle_process.object,
                         PROSENJIT_CAP_READ);
        waited_exit = 0x51515151;
        if (external_process_handle == PROSENJIT_HANDLE_INVALID ||
            process_wait(&init_process, thread_lifecycle_process.pid,
                         &waited_exit) == 0 ||
            thread_lifecycle_process.address_space.root == 0 ||
            thread_lifecycle_process.object.refcount != 2u ||
            waited_exit != 0x51515151 ||
            handle_check(&thread_lifecycle_process.handles, reap_probe_handle,
                         PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
            handle_close(&init_process.handles, external_process_handle) != 0) {
            serial_write("ProsenjitOS: ERROR external process-reference reap preflight failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    /* Exercise bounded registry-cycle and stack-ownership rejection. */
    if (thread_registry_integrity_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR thread registry integrity selftest failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* Corrupt queue endpoints must reject the real wait path before teardown. */
    waited_exit = 0x61616161;
    if (thread_runq_corruption_wait_selftest(&init_process,
                                              &thread_lifecycle_process) != 0 ||
        thread_lifecycle_process.state != PROS_PROCESS_ZOMBIE ||
        process_lookup(thread_lifecycle_process.pid) != &thread_lifecycle_process ||
        thread_lifecycle_process.address_space.root == 0 ||
        thread_lifecycle.kernel_stack_base == 0 ||
        thread_lifecycle_second.kernel_stack_base == 0 ||
        handle_check(&thread_lifecycle_process.handles, reap_probe_handle,
                     PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
        reap_probe.refcount != 2u) {
        serial_write("ProsenjitOS: ERROR run-queue corruption wait preflight was destructive\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* A process may close its own process-object handles as part of reap. */
    {
        uint64_t self_handle =
            handle_alloc(&thread_lifecycle_process.handles,
                         &thread_lifecycle_process.object, PROSENJIT_CAP_READ);
        if (self_handle == PROSENJIT_HANDLE_INVALID ||
            thread_lifecycle_process.object.refcount != 2u) {
            serial_write("ProsenjitOS: ERROR self process-handle fixture setup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    /* TSS/current-RSP rejection must happen before handles or mappings are freed. */
    {
        uint64_t saved_rsp0 = cpu_get_kernel_stack();
        uint64_t dead_stack_top =
            (uint64_t)(uintptr_t)thread_lifecycle.kernel_stack_base +
            thread_lifecycle.kernel_stack_size;
        waited_exit = 0x52525252;
        int set_rc = cpu_set_kernel_stack(dead_stack_top);
        int wait_rc = process_wait(&init_process, thread_lifecycle_process.pid,
                                   &waited_exit);
        int reap_rc = thread_reap_process(&thread_lifecycle_process);
        int restore_rc = cpu_set_kernel_stack(saved_rsp0);
        if (!thread_lifecycle.kernel_stack_base || set_rc != 0 ||
            wait_rc == 0 || reap_rc == 0 || restore_rc != 0 ||
            cpu_get_kernel_stack() != saved_rsp0 ||
            thread_lifecycle_process.state != PROS_PROCESS_ZOMBIE ||
            process_lookup(thread_lifecycle_process.pid) != &thread_lifecycle_process ||
            thread_lifecycle_process.address_space.root == 0 ||
            thread_lifecycle.kernel_stack_base == 0 ||
            thread_lifecycle_second.kernel_stack_base == 0 ||
            thread_lifecycle.registry_next == 0 ||
            thread_lifecycle_second.registry_next == 0 ||
            waited_exit != 0x52525252 ||
            handle_check(&thread_lifecycle_process.handles, reap_probe_handle,
                         PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
            reap_probe.refcount != 2u) {
            serial_write("ProsenjitOS: ERROR TSS reap preflight was destructive or incomplete\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    /* A malformed later stack must not partially release an earlier stack. */
    {
        uint64_t saved_rsp0 = cpu_get_kernel_stack();
        uint64_t saved_page = thread_lifecycle_second.kernel_stack_pages[0];
        uint64_t slots_before = 0u, quarantine_before = 0u;
        uint64_t pages_before[8];
        uint32_t allocated_before[8];
        uint64_t free_pages_before = physical_memory_free_pages();

        if (thread_stack_allocator_snapshot(&slots_before,
                                            &quarantine_before) != 0) {
            serial_write("ProsenjitOS: ERROR late-stack allocator snapshot failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        for (uint32_t i = 0u; i < 8u; ++i) {
            struct pros_thread *owner = i < 4u
                ? &thread_lifecycle : &thread_lifecycle_second;
            uint32_t page_index = i & 3u;
            pages_before[i] = owner->kernel_stack_pages[page_index];
            allocated_before[i] =
                physical_page_is_allocated(pages_before[i]) ? 1u : 0u;
            if (!pages_before[i] || !allocated_before[i]) {
                serial_write("ProsenjitOS: ERROR late-stack allocator baseline invalid\n");
                for (;;) __asm__ volatile ("cli; hlt");
            }
        }

        thread_lifecycle_second.kernel_stack_pages[0] = 0u;
        waited_exit = 0x53535353;
        int wait_rc = process_wait(&init_process, thread_lifecycle_process.pid,
                                   &waited_exit);
        int reap_rc = thread_reap_process(&thread_lifecycle_process);
        thread_lifecycle_second.kernel_stack_pages[0] = saved_page;

        uint64_t slots_after = 0u, quarantine_after = 0u;
        int ownership_unchanged =
            thread_stack_allocator_snapshot(&slots_after,
                                            &quarantine_after) == 0 &&
            slots_after == slots_before &&
            quarantine_after == quarantine_before &&
            physical_memory_free_pages() == free_pages_before;
        for (uint32_t i = 0u; i < 8u; ++i) {
            struct pros_thread *owner = i < 4u
                ? &thread_lifecycle : &thread_lifecycle_second;
            uint32_t page_index = i & 3u;
            uint64_t va = (uint64_t)(uintptr_t)owner->kernel_stack_base +
                          (uint64_t)page_index * PROSENJIT_PAGE_SIZE;
            if (owner->kernel_stack_pages[page_index] != pages_before[i] ||
                paging_translate(va) != pages_before[i] ||
                (physical_page_is_allocated(pages_before[i]) ? 1u : 0u) !=
                    allocated_before[i])
                ownership_unchanged = 0;
        }

        if (wait_rc == 0 || reap_rc == 0 || !ownership_unchanged ||
            cpu_get_kernel_stack() != saved_rsp0 ||
            thread_lifecycle_process.state != PROS_PROCESS_ZOMBIE ||
            process_lookup(thread_lifecycle_process.pid) != &thread_lifecycle_process ||
            thread_lifecycle.kernel_stack_base == 0 ||
            thread_lifecycle_second.kernel_stack_base == 0 ||
            thread_lifecycle.registry_next == 0 ||
            thread_lifecycle_second.registry_next == 0 ||
            thread_lifecycle_process.address_space.root == 0 ||
            waited_exit != 0x53535353 ||
            handle_check(&thread_lifecycle_process.handles, reap_probe_handle,
                         PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
            reap_probe.refcount != 2u) {
            serial_write("ProsenjitOS: ERROR multi-thread stack reap preflight was partial\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    /*
     * A cross-thread physical stack-page alias must be rejected by the real
     * process wait/reap path, not only by the registry-level self-test.
     * Keep each per-thread stack record and mapping internally consistent so
     * only the global ownership-uniqueness gate detects the corruption.
     */
    {
        uint32_t alias_page_index = 0u;
        uint64_t alias_va =
            (uint64_t)(uintptr_t)thread_lifecycle_second.kernel_stack_base +
            (uint64_t)alias_page_index * PROSENJIT_PAGE_SIZE;
        uint64_t original_page =
            thread_lifecycle_second.kernel_stack_pages[alias_page_index];
        uint64_t shared_page =
            thread_lifecycle.kernel_stack_pages[alias_page_index];
        uint64_t alias_vas[1] = {alias_va};
        uint64_t original_pages[1] = {original_page};
        uint64_t shared_pages[1] = {shared_page};

        if (paging_unmap_pages_expected(alias_vas, original_pages, 1u) != 0) {
            serial_write("ProsenjitOS: ERROR cross-thread process alias fixture unmap failed\\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        physical_page_free(original_page);
        if (paging_map_page(alias_va, shared_page,
                            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
            uint64_t replacement = physical_page_alloc();
            if (!replacement ||
                paging_map_page(alias_va, replacement,
                                PROSENJIT_PAGE_WRITABLE |
                                PROSENJIT_PAGE_NX) != 0) {
                serial_write("ProsenjitOS: ERROR cross-thread process alias fixture recovery failed\\n");
                for (;;) __asm__ volatile ("cli; hlt");
            }
            thread_lifecycle_second.kernel_stack_pages[alias_page_index] =
                replacement;
            serial_write("ProsenjitOS: ERROR cross-thread process alias fixture map failed\\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        thread_lifecycle_second.kernel_stack_pages[alias_page_index] =
            shared_page;

        /*
         * Snapshot every relevant allocator/mapping owner after constructing
         * the deliberate alias. Rejected wait/reap must be completely
         * read-only, including allocator bitmaps and physical-page accounting.
         */
        uint64_t stack_slots_before = 0u;
        uint64_t stack_quarantine_before = 0u;
        uint64_t stack_pages_before[8];
        uint32_t stack_page_allocated_before[8];
        uint64_t free_pages_before = physical_memory_free_pages();
        if (thread_stack_allocator_snapshot(&stack_slots_before,
                                            &stack_quarantine_before) != 0) {
            serial_write("ProsenjitOS: ERROR stack allocator snapshot failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        for (uint32_t i = 0u; i < 8u; ++i) {
            struct pros_thread *owner = i < 4u
                ? &thread_lifecycle : &thread_lifecycle_second;
            uint32_t page_index = i & 3u;
            stack_pages_before[i] = owner->kernel_stack_pages[page_index];
            stack_page_allocated_before[i] =
                physical_page_is_allocated(stack_pages_before[i]) ? 1u : 0u;
        }
        if (physical_page_is_allocated(original_page) ||
            !physical_page_is_allocated(shared_page)) {
            serial_write("ProsenjitOS: ERROR cross-thread alias allocator fixture invalid\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        waited_exit = 0x56565656;
        int wait_rc = process_wait(&init_process, thread_lifecycle_process.pid,
                                   &waited_exit);
        int reap_rc = thread_reap_process(&thread_lifecycle_process);
        uint64_t stack_slots_after = 0u;
        uint64_t stack_quarantine_after = 0u;
        int ownership_unchanged =
            thread_stack_allocator_snapshot(&stack_slots_after,
                                            &stack_quarantine_after) == 0 &&
            stack_slots_after == stack_slots_before &&
            stack_quarantine_after == stack_quarantine_before &&
            physical_memory_free_pages() == free_pages_before &&
            !physical_page_is_allocated(original_page);
        for (uint32_t i = 0u; i < 8u; ++i) {
            struct pros_thread *owner = i < 4u
                ? &thread_lifecycle : &thread_lifecycle_second;
            uint32_t page_index = i & 3u;
            uint64_t va = (uint64_t)(uintptr_t)owner->kernel_stack_base +
                          (uint64_t)page_index * PROSENJIT_PAGE_SIZE;
            if (owner->kernel_stack_pages[page_index] != stack_pages_before[i] ||
                paging_translate(va) != stack_pages_before[i] ||
                (physical_page_is_allocated(stack_pages_before[i]) ? 1u : 0u) !=
                    stack_page_allocated_before[i])
                ownership_unchanged = 0;
        }

        if (wait_rc == 0 || reap_rc == 0 || !ownership_unchanged ||
            thread_lifecycle_process.state != PROS_PROCESS_ZOMBIE ||
            process_lookup(thread_lifecycle_process.pid) !=
                &thread_lifecycle_process ||
            thread_lifecycle_process.address_space.root == 0 ||
            thread_lifecycle.kernel_stack_base == 0 ||
            thread_lifecycle_second.kernel_stack_base == 0 ||
            thread_lifecycle.registry_next == 0 ||
            thread_lifecycle_second.registry_next == 0 ||
            waited_exit != 0x56565656 ||
            handle_check(&thread_lifecycle_process.handles, reap_probe_handle,
                         PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
            reap_probe.refcount != 2u) {
            serial_write("ProsenjitOS: ERROR cross-thread process alias was not rejected transactionally\\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        /* Restore a unique, independently allocated page before later tests. */
        if (paging_unmap_pages_expected(alias_vas, shared_pages, 1u) != 0) {
            serial_write("ProsenjitOS: ERROR cross-thread process alias unmap recovery failed\\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        uint64_t restored_page = physical_page_alloc();
        if (!restored_page ||
            paging_map_page(alias_va, restored_page,
                            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
            serial_write("ProsenjitOS: ERROR cross-thread process alias page restore failed\\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        thread_lifecycle_second.kernel_stack_pages[alias_page_index] =
            restored_page;
        if (paging_translate((uint64_t)(uintptr_t)
                thread_lifecycle.kernel_stack_base) !=
                thread_lifecycle.kernel_stack_pages[0] ||
            paging_translate((uint64_t)(uintptr_t)
                thread_lifecycle_second.kernel_stack_base) !=
                thread_lifecycle_second.kernel_stack_pages[0] ||
            !thread_process_reapable(&thread_lifecycle_process)) {
            serial_write("ProsenjitOS: ERROR cross-thread process alias restoration failed\\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    /* The target CR3 must also be rejected before any handle release. */
    {
        uint64_t irq_flags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
        uint64_t saved_rsp0 = cpu_get_kernel_stack();
        waited_exit = 0x54545454;
        int switch_rc = address_space_switch(&thread_lifecycle_process.address_space);
        int wait_rc = switch_rc == 0
            ? process_wait(&init_process, thread_lifecycle_process.pid, &waited_exit)
            : -99;
        int restore_rc = address_space_switch(&init_process.address_space);
        if (restore_rc == 0)
            __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        if (switch_rc != 0 || wait_rc == 0 || restore_rc != 0 ||
            cpu_get_kernel_stack() != saved_rsp0 ||
            thread_lifecycle_process.state != PROS_PROCESS_ZOMBIE ||
            process_lookup(thread_lifecycle_process.pid) != &thread_lifecycle_process ||
            thread_lifecycle_process.address_space.root == 0 ||
            thread_lifecycle.kernel_stack_base == 0 ||
            thread_lifecycle_second.kernel_stack_base == 0 ||
            thread_lifecycle.registry_next == 0 ||
            thread_lifecycle_second.registry_next == 0 ||
            waited_exit != 0x54545454 ||
            handle_check(&thread_lifecycle_process.handles, reap_probe_handle,
                         PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
            reap_probe.refcount != 2u) {
            serial_write("ProsenjitOS: ERROR active CR3 reap preflight was destructive\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    /* A corrupt object reference count must be rejected before close-all mutates. */
    {
        uint64_t saved_rsp0 = cpu_get_kernel_stack();
        uint32_t saved_refcount = reap_probe.refcount;
        uint32_t slot = (uint32_t)(reap_probe_handle & 0xFFULL);
        reap_probe.refcount = 0u;
        waited_exit = 0x56565656;
        int wait_rc = process_wait(&init_process, thread_lifecycle_process.pid,
                                   &waited_exit);
        int entry_preserved =
            thread_lifecycle_process.handles.entries[slot].object == &reap_probe;
        reap_probe.refcount = saved_refcount;
        if (wait_rc == 0 || !entry_preserved ||
            cpu_get_kernel_stack() != saved_rsp0 ||
            thread_lifecycle_process.state != PROS_PROCESS_ZOMBIE ||
            process_lookup(thread_lifecycle_process.pid) != &thread_lifecycle_process ||
            thread_lifecycle_process.address_space.root == 0 ||
            thread_lifecycle.kernel_stack_base == 0 ||
            thread_lifecycle_second.kernel_stack_base == 0 ||
            thread_lifecycle.registry_next == 0 ||
            thread_lifecycle_second.registry_next == 0 ||
            waited_exit != 0x56565656 || reap_probe.refcount != 2u) {
            serial_write("ProsenjitOS: ERROR handle-table reap preflight was destructive\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    /* Corrupt owned-page metadata must fail wait before handles or mappings change. */
    {
        struct pros_owned_page *owned = thread_lifecycle_process.address_space.owned_pages;
        if (!owned) {
            serial_write("ProsenjitOS: ERROR address-space corruption fixture missing\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        uint64_t saved_physical = owned->physical_address;
        owned->physical_address = saved_physical + PROSENJIT_PAGE_SIZE;
        waited_exit = 0x57575757;
        int wait_rc = process_wait(&init_process, thread_lifecycle_process.pid,
                                   &waited_exit);
        int intact = thread_lifecycle_process.address_space.root != 0 &&
            thread_lifecycle_process.address_space.owned_pages == owned &&
            thread_lifecycle_process.state == PROS_PROCESS_ZOMBIE &&
            process_lookup(thread_lifecycle_process.pid) == &thread_lifecycle_process &&
            thread_lifecycle.kernel_stack_base != 0 &&
            thread_lifecycle_second.kernel_stack_base != 0 &&
            handle_check(&thread_lifecycle_process.handles, reap_probe_handle,
                         PROSENJIT_CAP_READ, PROS_OBJECT_FILE) == 0 &&
            reap_probe.refcount == 2u && waited_exit == 0x57575757;
        owned->physical_address = saved_physical;
        if (wait_rc == 0 || !intact) {
            serial_write("ProsenjitOS: ERROR address-space corruption wait preflight was destructive\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    if (process_wait(&init_process, thread_lifecycle_process.pid, &waited_exit) != 0 ||
        waited_exit != 21 || thread_lifecycle_process.address_space.root != 0 ||
        thread_lifecycle.kernel_stack_base != 0 ||
        thread_lifecycle.kernel_stack_size != 0 ||
        thread_lifecycle_second.kernel_stack_base != 0 ||
        thread_lifecycle_second.kernel_stack_size != 0 ||
        thread_lifecycle_second.registry_next != 0 ||
        thread_lifecycle_process.object.refcount != 0u ||
        reap_probe.refcount != 1u ||
        thread_lifecycle.registry_next != 0) {
        serial_write("ProsenjitOS: ERROR process-thread teardown regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process-thread stack reap regression passed\n");
    serial_write("ProsenjitOS: TSS active-stack reap guard passed\n");
    serial_write("ProsenjitOS: process wait/reap full preflight retry regression passed\n");

    /*
     * Process teardown must unregister a blocked thread from its IPC channel
     * before the thread object and its kernel stack can be reaped.
     */
    {
        struct pros_process ipc_exit_process;
        struct pros_thread ipc_exit_thread;
        struct pros_ipc_channel ipc_exit_channel;
        if (process_create(&init_process, &ipc_exit_process, 12,
                           PROS_SECURITY_LIMITED) != 0 ||
            thread_init(&ipc_exit_thread, &ipc_exit_process, 12,
                        (uint64_t)(uintptr_t)scheduler_test_worker_entry) != 0 ||
            ipc_channel_init(&ipc_exit_channel) != PROSENJIT_IPC_OK ||
            thread_set_state(&ipc_exit_thread, PROS_THREAD_BLOCKED) != 0) {
            serial_write("ProsenjitOS: ERROR IPC waiter process-exit setup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        ipc_exit_thread.wait_channel = &ipc_exit_channel;
        ipc_exit_thread.blocked_syscall = PROSENJIT_SYS_IPC_RECEIVE;
        ipc_exit_channel.waiters[0] = &ipc_exit_thread;
        ipc_exit_channel.waiter_count = 1u;

        if (process_exit(&ipc_exit_process, 22) != 0 ||
            thread_state(&ipc_exit_thread) != PROS_THREAD_DEAD ||
            ipc_exit_channel.waiter_count != 0u ||
            ipc_exit_channel.waiters[0] != 0 ||
            ipc_exit_thread.wait_channel != 0 ||
            ipc_exit_thread.blocked_syscall != 0) {
            serial_write("ProsenjitOS: ERROR process-exit IPC waiter cleanup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        if (process_wait(&init_process, ipc_exit_process.pid, &waited_exit) != 0 ||
            waited_exit != 22 ||
            ipc_exit_thread.kernel_stack_base != 0 ||
            ipc_exit_thread.registry_next != 0) {
            serial_write("ProsenjitOS: ERROR process-exit IPC waiter reap failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: process-exit IPC waiter cleanup regression passed\n");

    /*
     * IPC wait-queue regression: invalidate a stale frame, force an early
     * message arrival, then verify the interrupt scheduler retries the wake
     * only after publishing the fresh CPL3 syscall frame.
     */
    struct pros_ipc_channel ipc_wait_test;
    scheduler_init_frame.rax = PROSENJIT_SYSCALL_INTERNAL_BLOCK;
    scheduler_init_frame.rip = 0x1002;
    scheduler_init_frame.cs = 0x1B;
    scheduler_init_frame.rflags = 0x202;
    scheduler_init_frame.user_rsp = PROSENJIT_USER_STACK_TOP;
    scheduler_init_frame.user_ss = 0x23;
    init_thread.interrupt_frame = 0;
    init_thread.interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    init_thread.blocked_syscall = PROSENJIT_SYS_IPC_RECEIVE;
    if (ipc_channel_init(&ipc_wait_test) != PROSENJIT_IPC_OK ||
        thread_current() != &init_thread ||
        ipc_channel_wait(&ipc_wait_test, &init_thread) != PROSENJIT_IPC_OK ||
        thread_state(&init_thread) != PROS_THREAD_BLOCKED ||
        init_thread.wait_channel != &ipc_wait_test) {
        serial_write("ProsenjitOS: ERROR IPC wait-block regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* Neither a KERNEL tag nor a malformed USER tail may resume IPC. */
    init_thread.interrupt_frame = &scheduler_init_frame;
    init_thread.interrupt_frame_kind = PROS_INTERRUPT_FRAME_KERNEL;
    if (thread_wake(&init_thread) == 0 ||
        thread_state(&init_thread) != PROS_THREAD_BLOCKED) {
        serial_write("ProsenjitOS: ERROR kernel-frame IPC wakeup was accepted\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    init_thread.interrupt_frame_kind = PROS_INTERRUPT_FRAME_USER;
    scheduler_init_frame.user_ss = 0x08;
    if (thread_wake(&init_thread) == 0 ||
        thread_state(&init_thread) != PROS_THREAD_BLOCKED) {
        serial_write("ProsenjitOS: ERROR malformed user-frame wakeup was accepted\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    scheduler_init_frame.user_ss = 0x23;
    scheduler_init_frame.rflags = 0x3202;
    if (thread_wake(&init_thread) == 0 ||
        thread_state(&init_thread) != PROS_THREAD_BLOCKED) {
        serial_write("ProsenjitOS: ERROR privileged user RFLAGS wakeup was accepted\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    scheduler_init_frame.rflags = 0x202;

    /*
     * Model a sender winning after waiter registration but before the
     * scheduler has saved this syscall's frame. The stale frame is invalidated,
     * so the first wake must leave the waiter queued for the scheduler retry.
     */
    init_thread.interrupt_frame = 0;
    init_thread.interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    const uint8_t wait_message[] = { 'W', 'A', 'K', 'E' };
    if (ipc_channel_send(&ipc_wait_test, wait_message, sizeof(wait_message)) != PROSENJIT_IPC_OK ||
        thread_state(&init_thread) != PROS_THREAD_BLOCKED ||
        init_thread.wait_channel != &ipc_wait_test ||
        init_thread.blocked_syscall != PROSENJIT_SYS_IPC_RECEIVE ||
        ipc_wait_test.waiter_count != 1 ||
        ipc_channel_count(&ipc_wait_test) != 1) {
        serial_write("ProsenjitOS: ERROR early IPC wake was not deferred safely\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    struct pros_interrupt_frame *early_wake_switch =
        thread_schedule_interrupt(&scheduler_init_frame, PROS_INTERRUPT_FRAME_USER);
    if (!early_wake_switch ||
        thread_current() != &worker_thread ||
        thread_state(&init_thread) != PROS_THREAD_READY ||
        init_thread.wait_channel != 0 ||
        init_thread.blocked_syscall != 0 ||
        init_thread.interrupt_frame != &init_thread.user_interrupt_frame_storage ||
        init_thread.interrupt_frame_kind != PROS_INTERRUPT_FRAME_USER ||
        init_thread.interrupt_frame == &scheduler_init_frame ||
        scheduler_init_frame.rax != PROSENJIT_SYSCALL_INTERNAL_BLOCK ||
        scheduler_init_frame.rip != 0x1002 ||
        init_thread.interrupt_frame->rax != PROSENJIT_SYS_IPC_RECEIVE ||
        init_thread.interrupt_frame->rip != 0x1000 ||
        init_thread.interrupt_frame->user_rsp != PROSENJIT_USER_STACK_TOP ||
        init_thread.interrupt_frame->user_ss != 0x23 ||
        ipc_wait_test.waiter_count != 0 ||
        ipc_channel_count(&ipc_wait_test) != 1 ||
        thread_schedule() != 0 ||
        thread_current() != &init_thread ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING) {
        serial_write("ProsenjitOS: ERROR IPC early-wakeup scheduler retry regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: IPC wait-queue block/wakeup test passed\n");
    serial_write("ProsenjitOS: IPC early-wakeup stale-frame retry regression passed\\n");

    /*
     * Regression: if a message is already queued when a receiver tries to
     * register as a waiter, the wait operation must report EALREADY without
     * blocking the thread or adding a stale waiter. syscall_dispatch() must
     * retry receive for this result rather than return a spurious error.
     */
    if (thread_current() != &init_thread ||
        ipc_channel_wait(&ipc_wait_test, &init_thread) != PROSENJIT_IPC_EALREADY ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING ||
        init_thread.wait_channel != 0 ||
        init_thread.blocked_syscall != 0 ||
        ipc_wait_test.waiter_count != 0 ||
        ipc_channel_count(&ipc_wait_test) != 1) {
        serial_write("ProsenjitOS: ERROR IPC queued-message waiter race regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: IPC queued-message waiter race regression passed\n");

    /*
     * Cancellation removes only the channel registration. The scheduler still
     * owns the blocked thread transition, so an explicit wake must remain
     * possible and must clear the wait metadata exactly once.
     */
    uint8_t drained_message[sizeof(wait_message)];
    uint32_t drained_length = 0;
    if (ipc_channel_receive(&ipc_wait_test, drained_message,
                            sizeof(drained_message), &drained_length) != PROSENJIT_IPC_OK ||
        drained_length != sizeof(wait_message) ||
        ipc_channel_count(&ipc_wait_test) != 0) {
        serial_write("ProsenjitOS: ERROR IPC cancellation test message drain failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    init_thread.user_interrupt_frame_storage.rax = PROSENJIT_SYSCALL_INTERNAL_BLOCK;
    init_thread.user_interrupt_frame_storage.rip = 0x2002;
    init_thread.user_interrupt_frame_storage.cs = 0x1B;
    init_thread.user_interrupt_frame_storage.rflags = 0x202;
    init_thread.user_interrupt_frame_storage.user_rsp = PROSENJIT_USER_STACK_TOP;
    init_thread.user_interrupt_frame_storage.user_ss = 0x23;
    init_thread.interrupt_frame = &init_thread.user_interrupt_frame_storage;
    init_thread.interrupt_frame_kind = PROS_INTERRUPT_FRAME_USER;
    init_thread.blocked_syscall = PROSENJIT_SYS_IPC_RECEIVE;
    if (ipc_channel_wait(&ipc_wait_test, &init_thread) != PROSENJIT_IPC_OK ||
        thread_state(&init_thread) != PROS_THREAD_BLOCKED ||
        ipc_wait_test.waiter_count != 1 ||
        ipc_channel_cancel_waiter(&ipc_wait_test, &init_thread) != PROSENJIT_IPC_OK ||
        ipc_wait_test.waiter_count != 0 ||
        thread_state(&init_thread) != PROS_THREAD_BLOCKED ||
        init_thread.wait_channel != &ipc_wait_test ||
        ipc_channel_cancel_waiter(&ipc_wait_test, &init_thread) != PROSENJIT_IPC_EEMPTY) {
        serial_write("ProsenjitOS: ERROR IPC waiter cancellation registration regression failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (thread_wake(&init_thread) != 0 ||
        thread_state(&init_thread) != PROS_THREAD_READY ||
        init_thread.wait_channel != 0 ||
        init_thread.blocked_syscall != 0 ||
        init_thread.user_interrupt_frame_storage.rax != PROSENJIT_SYS_IPC_RECEIVE ||
        init_thread.user_interrupt_frame_storage.rip != 0x2000 ||
        thread_schedule() != 0 ||
        thread_current() != &worker_thread ||
        thread_schedule() != 0 ||
        thread_current() != &init_thread ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING) {
        serial_write("ProsenjitOS: ERROR IPC waiter cancellation wake recovery regression failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: IPC waiter cancellation and wake recovery regression passed\\n");

    /* Regression guard for interrupt-driven scheduler frame save/restore. */
    scheduler_init_frame.rax = 0x1111111111111111ULL;
    scheduler_init_frame.rip = 0xAAAAAAAAAAAAAAAAULL;
    scheduler_init_frame.cs = 0x08;
    scheduler_init_frame.rflags = 0x202;
    struct pros_interrupt_frame *first_switch =
        thread_schedule_interrupt(&scheduler_init_frame, PROS_INTERRUPT_FRAME_KERNEL);
    if (thread_current() != &worker_thread ||
        first_switch != worker_thread.interrupt_frame ||
        worker_thread.interrupt_frame_kind != PROS_INTERRUPT_FRAME_KERNEL ||
        first_switch->rip != (uint64_t)(uintptr_t)thread_start_trampoline ||
        worker_thread.interrupt_frame == &scheduler_init_frame ||
        cpu_get_kernel_stack() !=
            (uint64_t)(uintptr_t)worker_thread.kernel_stack_base +
            worker_thread.kernel_stack_size) {
        serial_write("ProsenjitOS: ERROR interrupt-frame first-switch regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    scheduler_worker_frame.rax = 0x2222222222222222ULL;
    scheduler_worker_frame.rip = 0xBBBBBBBBBBBBBBBBULL;
    scheduler_worker_frame.cs = 0x08;
    scheduler_worker_frame.rflags = 0x202;
    struct pros_interrupt_frame *second_switch =
        thread_schedule_interrupt(&scheduler_worker_frame, PROS_INTERRUPT_FRAME_KERNEL);
    if (thread_current() != &init_thread ||
        second_switch != &scheduler_init_frame ||
        init_thread.interrupt_frame != &scheduler_init_frame ||
        init_thread.interrupt_frame_kind != PROS_INTERRUPT_FRAME_KERNEL ||
        worker_thread.interrupt_frame_kind != PROS_INTERRUPT_FRAME_KERNEL ||
        scheduler_init_frame.rax != 0x1111111111111111ULL ||
        scheduler_init_frame.rip != 0xAAAAAAAAAAAAAAAAULL ||
        worker_thread.interrupt_frame != &scheduler_worker_frame ||
        cpu_get_kernel_stack() !=
            (uint64_t)(uintptr_t)init_thread.kernel_stack_base +
            init_thread.kernel_stack_size) {
        serial_write("ProsenjitOS: ERROR interrupt-frame save/restore regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: interrupt-frame scheduler save/restore test passed\n");
    serial_write("ProsenjitOS: TSS RSP0 follows interrupt-frame scheduler switches passed\n");

    /* A failed CR3/address-space switch must not leave the still-executing
     * thread marked READY and linked in the run queue. Keep each call
     * separately bracketed so a boot hang identifies the exact operation. */
    {
        struct pros_process switch_failure_process;
        struct pros_thread switch_failure_thread;
        uint64_t *switch_failure_root = 0;
        int stage_rc;

        serial_write("ProsenjitOS: scheduler rollback diag 1 process_create begin\n");
        stage_rc = process_create(&init_process, &switch_failure_process, 11,
                                  PROS_SECURITY_LIMITED);
        serial_write("ProsenjitOS: scheduler rollback diag 1 process_create rc=");
        serial_hex64((uint64_t)(int64_t)stage_rc);
        serial_write("\n");
        if (stage_rc != 0) {
            serial_write("ProsenjitOS: ERROR scheduler switch-failure setup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        serial_write("ProsenjitOS: scheduler rollback diag 2 thread_init begin\n");
        stage_rc = thread_init(&switch_failure_thread, &switch_failure_process, 11,
                               (uint64_t)(uintptr_t)scheduler_test_worker_entry);
        serial_write("ProsenjitOS: scheduler rollback diag 2 thread_init rc=");
        serial_hex64((uint64_t)(int64_t)stage_rc);
        serial_write("\n");
        if (stage_rc != 0) {
            serial_write("ProsenjitOS: ERROR scheduler switch-failure setup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        serial_write("ProsenjitOS: scheduler rollback diag 3 thread_set_state begin\n");
        stage_rc = thread_set_state(&switch_failure_thread, PROS_THREAD_READY);
        serial_write("ProsenjitOS: scheduler rollback diag 3 thread_set_state rc=");
        serial_hex64((uint64_t)(int64_t)stage_rc);
        serial_write("\n");
        if (stage_rc != 0) {
            serial_write("ProsenjitOS: ERROR scheduler switch-failure setup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        serial_write("ProsenjitOS: scheduler rollback diag 4 thread_enqueue begin\n");
        stage_rc = thread_enqueue(&switch_failure_thread);
        serial_write("ProsenjitOS: scheduler rollback diag 4 thread_enqueue rc=");
        serial_hex64((uint64_t)(int64_t)stage_rc);
        serial_write("\n");
        if (stage_rc != 0) {
            serial_write("ProsenjitOS: ERROR scheduler switch-failure setup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        serial_write("ProsenjitOS: scheduler rollback diag 5 first thread_schedule begin\n");
        stage_rc = thread_schedule();
        serial_write("ProsenjitOS: scheduler rollback diag 5 first thread_schedule rc=");
        serial_hex64((uint64_t)(int64_t)stage_rc);
        serial_write("\n");
        if (stage_rc != 0 || thread_current() != &worker_thread ||
            thread_state(&worker_thread) != PROS_THREAD_RUNNING) {
            serial_write("ProsenjitOS: ERROR scheduler switch-failure setup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        switch_failure_root = switch_failure_process.address_space.root;
        switch_failure_process.address_space.root = 0;
        serial_write("ProsenjitOS: scheduler rollback diag 6 failed-root second schedule begin\n");
        stage_rc = thread_schedule();
        serial_write("ProsenjitOS: scheduler rollback diag 6 second thread_schedule rc=");
        serial_hex64((uint64_t)(int64_t)stage_rc);
        serial_write("\n");
        if (stage_rc == 0 || thread_current() != &worker_thread ||
            thread_state(&worker_thread) != PROS_THREAD_RUNNING ||
            process_current() != &init_process ||
            thread_state(&switch_failure_thread) != PROS_THREAD_READY ||
            cpu_get_kernel_stack() !=
                (uint64_t)(uintptr_t)worker_thread.kernel_stack_base +
                worker_thread.kernel_stack_size) {
            serial_write("ProsenjitOS: ERROR scheduler switch-failure rollback regression failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        switch_failure_process.address_space.root = switch_failure_root;
        serial_write("ProsenjitOS: scheduler rollback diag 7 process_exit begin\n");
        stage_rc = process_exit(&switch_failure_process, 22);
        serial_write("ProsenjitOS: scheduler rollback diag 7 process_exit rc=");
        serial_hex64((uint64_t)(int64_t)stage_rc);
        serial_write("\n");
        if (stage_rc != 0) {
            serial_write("ProsenjitOS: ERROR scheduler switch-failure cleanup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        serial_write("ProsenjitOS: scheduler rollback diag 8 process_wait begin\n");
        stage_rc = process_wait(&init_process, switch_failure_process.pid, &waited_exit);
        serial_write("ProsenjitOS: scheduler rollback diag 8 process_wait rc=");
        serial_hex64((uint64_t)(int64_t)stage_rc);
        serial_write("\n");
        if (stage_rc != 0 || waited_exit != 22) {
            serial_write("ProsenjitOS: ERROR scheduler switch-failure cleanup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        serial_write("ProsenjitOS: scheduler rollback diag 9 final thread_schedule begin\n");
        stage_rc = thread_schedule();
        serial_write("ProsenjitOS: scheduler rollback diag 9 final thread_schedule rc=");
        serial_hex64((uint64_t)(int64_t)stage_rc);
        serial_write("\n");
        if (stage_rc != 0 || thread_current() != &init_thread ||
            thread_state(&init_thread) != PROS_THREAD_RUNNING ||
            cpu_get_kernel_stack() !=
                (uint64_t)(uintptr_t)init_thread.kernel_stack_base +
                init_thread.kernel_stack_size) {
            serial_write("ProsenjitOS: ERROR scheduler switch-failure cleanup failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: scheduler address-space failure rollback test passed\n");
    serial_write("ProsenjitOS: TSS RSP0 rollback after failed CR3 switch passed\n");
    serial_write("ProsenjitOS: initial limited process and capability context online\n");
    {
        int security_init_status = security_manager_init();
        if (security_init_status != 0) {
            serial_write("ProsenjitOS: ERROR security manager initialization failed, stage = ");
            serial_hex64((uint64_t)(int64_t)security_init_status);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        /*
         * Runtime security verification: the manager self-tests above validate
         * individual policy components; this boundary verifies that the live
         * boot process is still admissible and that kernel hardening remains
         * active after initialization.
         */
        if (!pros_security_hardening_ready() ||
            pros_security_hardening_check_process(&init_process) != 0 ||
            pros_security_hardening_violations() != 0) {
            serial_write("ProsenjitOS: ERROR live security runtime verification failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        {
            uint64_t text_flags = 0, rodata_flags = 0;
            uint64_t text_start = (uint64_t)(uintptr_t)__kernel_text_start;
            uint64_t rodata_start = (uint64_t)(uintptr_t)__kernel_rodata_start;
            if (paging_query_page(text_start, &text_flags) != 0 ||
                paging_query_page(rodata_start, &rodata_flags) != 0 ||
                (text_flags & PROSENJIT_PAGE_PRESENT) == 0 ||
                (text_flags & PROSENJIT_PAGE_WRITABLE) != 0 ||
                (text_flags & PROSENJIT_PAGE_USER) != 0 ||
                (text_flags & PROSENJIT_PAGE_NX) != 0 ||
                (rodata_flags & PROSENJIT_PAGE_PRESENT) == 0 ||
                (rodata_flags & PROSENJIT_PAGE_WRITABLE) != 0 ||
                (rodata_flags & PROSENJIT_PAGE_USER) != 0 ||
                (rodata_flags & PROSENJIT_PAGE_NX) == 0) {
                serial_write("ProsenjitOS: ERROR live kernel page-protection verification failed\n");
                for (;;) __asm__ volatile ("cli; hlt");
            }
        }
        serial_write("ProsenjitOS: security runtime verification passed (process policy + kernel W^X/NX)\n");
    }
    /* Storage-stack regression suite: partition discovery and filesystem
     * geometry/mount contracts must remain boot-testable before drivers depend
     * on them. */
    {
        int partition_test_rc = partition_selftest();
        if (partition_test_rc != PROS_PARTITION_OK) {
            serial_write("ProsenjitOS: ERROR partition discovery self-test failed rc=");
            serial_hex64((uint64_t)(int64_t)partition_test_rc);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: partition discovery self-test passed\n");
    if (pros_vfs_provider_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR VFS provider registry self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: VFS provider registry boundary self-test passed\n");
    {
        int fatfs_test_rc = pros_fatfs_provider_selftest();
        if (fatfs_test_rc != PROS_VFS_OK) {
            serial_write("ProsenjitOS: ERROR FAT32/exFAT provider mount/I/O self-test failed rc=");
            serial_hex64((uint64_t)(int64_t)fatfs_test_rc);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: FAT32/exFAT provider mount/I/O/remount self-test passed\n");
    if (process_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR process lifecycle self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process lifecycle self-test passed\n");
    if (vfs_security_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR VFS permission/security self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: VFS permission/security self-tests passed\n");
    if (vfs_file_64bit_selftest() != PROS_VFS_OK) {
        serial_write("ProsenjitOS: ERROR VFS 64-bit file I/O self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: VFS 64-bit file I/O self-test passed\n");
    if (pros_storage_selftest() != PROS_STORAGE_OK) {
        serial_write("ProsenjitOS: ERROR storage volume discovery self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: storage volume discovery self-test passed\n");
    {
        int prosenjitfs_rc = prosenjitfs_selftest();
        if (prosenjitfs_rc != PROSENJITFS_OK) {
            uint32_t mkdir_stage = 0u, tx_stage = 0u, tx_count = 0u;
            uint32_t selftest_stage = 0u;
            uint32_t tx_count_before_clear = 0u, tx_count_after_clear = 0u;
            int mkdir_rc = PROSENJITFS_OK;
            int selftest_diag_rc = PROSENJITFS_OK;
            int tx_clear_rc = PROSENJITFS_OK;
            (void)prosenjitfs_mkdir_diag_get(&mkdir_stage, &mkdir_rc,
                                             &tx_stage, &tx_count);
            (void)prosenjitfs_tx_diag_get(&tx_stage, &tx_count,
                                          &tx_count_before_clear,
                                          &tx_count_after_clear,
                                          &tx_clear_rc);
            prosenjitfs_selftest_diag_get(&selftest_stage, &selftest_diag_rc);
            serial_write("ProsenjitOS: ERROR ProsenjitFS self-test failed rc=");
            serial_hex64((uint64_t)(int64_t)prosenjitfs_rc);
            serial_write(" selftest_stage=");
            serial_hex64(selftest_stage);
            serial_write(" selftest_rc=");
            serial_hex64((uint64_t)(int64_t)selftest_diag_rc);
            serial_write(" mkdir_stage=");
            serial_hex64(mkdir_stage);
            serial_write(" mkdir_rc=");
            serial_hex64((uint64_t)(int64_t)mkdir_rc);
            serial_write(" tx_stage=");
            serial_hex64(tx_stage);
            serial_write(" tx_count=");
            serial_hex64(tx_count);
            serial_write(" tx_clear_rc=");
            serial_hex64((uint64_t)(int64_t)tx_clear_rc);
            serial_write(" tx_count_before_clear=");
            serial_hex64(tx_count_before_clear);
            serial_write(" tx_count_after_clear=");
            serial_hex64(tx_count_after_clear);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: ProsenjitFS format/mount/allocation/journal/recovery self-test passed\n");
    if (boot_magic == PROSENJIT_NATIVE_BOOT_MAGIC) {
        struct pros_ata_pio native_ata;
        struct pros_partition partitions[4];
        struct pros_partition_device native_partition_view;
        struct pros_prosenjitfs native_fs;
        struct pros_vfs_file native_file;
        const struct pros_partition *native_partition = 0;
        uint32_t partition_count = 0u;
        uint32_t done = 0u;
        uint64_t translated_lba = 0u;
        uint32_t trace_count = 0u;
        uint8_t sector[512];
        static const uint8_t native_payload[] = {
            'N', 'A', 'T', 'I', 'V', 'E', '-', 'P', 'F', 'S', 0xA5u
        };
        uint8_t native_readback[sizeof(native_payload)];

        int native_rc = pros_ata_pio_selftest();
        if (native_rc != PROS_ATA_OK)
            native_storage_fail("ATA PIO probe/read", native_rc);
        serial_write("ProsenjitOS: ATA PIO real-disk block path self-test passed\n");

        native_rc = pros_ata_pio_probe(&native_ata);
        if (native_rc != PROS_ATA_OK)
            native_storage_fail("ATA PIO device probe", native_rc);
        native_rc = partition_read_mbr(&native_ata.block, partitions, 4u,
                                       &partition_count);
        if (native_rc != PROS_PARTITION_OK)
            native_storage_fail("MBR partition parse", native_rc);
        for (uint32_t i = 0u; i < partition_count; ++i) {
            if (partitions[i].mbr_type == 0xDAu &&
                partitions[i].first_lba == 2048u &&
                partitions[i].sector_count == 512u) {
                native_partition = &partitions[i];
                break;
            }
        }
        if (!native_partition)
            native_storage_fail("ProsenjitFS MBR partition missing", -1);
        native_rc = partition_device_init(&native_partition_view,
                                          &native_ata.block,
                                          native_partition);
        if (native_rc != PROS_PARTITION_OK)
            native_storage_fail("partition block-device setup", native_rc);

        if (block_device_read(&native_partition_view.device, 0u, 1u,
                              sector) != PROS_BLOCK_OK ||
            partition_trace_find(&native_partition_view, 0u, 0u,
                                 &translated_lba, &trace_count) !=
                PROS_PARTITION_OK ||
            translated_lba != native_partition->first_lba ||
            block_device_read(&native_partition_view.device,
                              native_partition_view.device.sector_count,
                              1u, sector) != PROS_BLOCK_EINVAL) {
            native_storage_fail("partition LBA translation/bounds", -2);
        }
        serial_write("ProsenjitOS: native filesystem LBA boundary diagnostic passed\n");

        if (prosenjitfs_probe(&native_partition_view.device) != PROSENJITFS_OK)
            native_storage_fail("native ProsenjitFS superblock probe", -3);
        native_rc = prosenjitfs_mount(&native_fs,
                                      &native_partition_view.device, 0u);
        if (native_rc != PROSENJITFS_OK)
            native_storage_fail("native ProsenjitFS writable mount", native_rc);
        if (vfs_kernel_file_open(&native_fs.mount, "/native-persist.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &native_file) != PROS_VFS_OK) {
            (void)prosenjitfs_unmount(&native_fs);
            native_storage_fail("native persistence file create", -4);
        }
        native_rc = vfs_file_write(&native_file, native_payload,
                                   sizeof(native_payload), &done);
        if (native_rc != PROS_VFS_OK || done != sizeof(native_payload)) {
            (void)vfs_file_close(&native_file);
            (void)prosenjitfs_unmount(&native_fs);
            native_storage_fail("native persistence file write", native_rc);
        }
        if (vfs_file_close(&native_file) != PROS_VFS_OK ||
            prosenjitfs_unmount(&native_fs) != PROSENJITFS_OK) {
            native_storage_fail("native filesystem flush/unmount", -5);
        }

        native_rc = prosenjitfs_mount(&native_fs,
                                      &native_partition_view.device, 1u);
        if (native_rc != PROSENJITFS_OK)
            native_storage_fail("native ProsenjitFS read-only remount", native_rc);
        if (vfs_kernel_file_open(&native_fs.mount, "/native-persist.bin",
                          PROS_VFS_O_READ, &native_file) != PROS_VFS_OK) {
            (void)prosenjitfs_unmount(&native_fs);
            native_storage_fail("native persistence file reopen", -6);
        }
        done = 0u;
        native_rc = vfs_file_read(&native_file, native_readback,
                                  sizeof(native_readback), &done);
        if (native_rc != PROS_VFS_OK || done != sizeof(native_payload)) {
            (void)vfs_file_close(&native_file);
            (void)prosenjitfs_unmount(&native_fs);
            native_storage_fail("native persistence file read", native_rc);
        }
        for (uint32_t i = 0u; i < sizeof(native_payload); ++i) {
            if (native_readback[i] != native_payload[i]) {
                (void)vfs_file_close(&native_file);
                (void)prosenjitfs_unmount(&native_fs);
                native_storage_fail("native persistence data mismatch", -7);
            }
        }
        if (vfs_file_close(&native_file) != PROS_VFS_OK ||
            prosenjitfs_unmount(&native_fs) != PROSENJITFS_OK) {
            native_storage_fail("native filesystem final unmount", -8);
        }
        serial_write("ProsenjitOS: ATA PIO -> MBR -> ProsenjitFS persistent mount test passed\n");
    }

    /* Driver-model foundation: discovery, matching, controlled binding and
     * lifecycle are verified independently before real bus drivers consume it. */
    if (pros_driver_manager_init() != PROS_DRIVER_OK) {
        serial_write("ProsenjitOS: ERROR driver manager initialization failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (pros_driver_selftest() != PROS_DRIVER_OK) {
        serial_write("ProsenjitOS: ERROR driver model self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (service_manager_selftest() != PROS_SERVICE_OK) {
        serial_write("ProsenjitOS: ERROR service manager self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: service manager dependency/lifecycle self-test passed\n");

    serial_write("ProsenjitOS: driver model registry/lifecycle self-test passed\n");
    (void)pros_log_emit(PROS_HEALTH_INFO, PROS_HEALTH_DRIVER,
                        0x44525648u, PROS_HEALTH_RECOVERY_DRIVER_READY, timer_ticks());
    (void)pros_health_watchdog_kick(1u);
    (void)pros_health_watchdog_poll();

    /* Device permission broker: grants are issued only after a device is bound. */
    if (pros_device_broker_selftest() != PROS_DRIVER_OK) {
        serial_write("ProsenjitOS: ERROR device permission broker self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: device permission broker self-test passed\n");

    if (pros_display_selftest() != PROS_DRIVER_OK) {
        serial_write("ProsenjitOS: ERROR display-device binding self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: display-device binding self-test passed\n");

    {
        int network_selftest_rc = pros_net_selftest();
        if (network_selftest_rc != PROS_NET_OK) {
            serial_write("ProsenjitOS: ERROR network protocol/socket self-test failed rc=");
            serial_hex64((uint64_t)(int64_t)network_selftest_rc);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: network protocol/socket self-test passed\n");

    /*
     * PCI enumeration foundation: probe the standard PCI configuration
     * mechanism before entering CPL3. The scanner is deliberately read-only;
     * later driver work can build on this inventory without claiming device
     * ownership yet.
     */
    if (pros_usb_dma_selftest() != PROS_USB_DMA_OK) {
        serial_write("ProsenjitOS: ERROR USB DMA buffer self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: USB DMA buffer self-test passed\n");

    {
        int usb_msc_selftest_rc = pros_usb_msc_selftest();
        if (usb_msc_selftest_rc != PROS_USB_MSC_OK) {
            serial_write("ProsenjitOS: ERROR USB Mass Storage BOT/SCSI self-test failed rc=");
            serial_hex64((uint64_t)(int64_t)usb_msc_selftest_rc);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: USB Mass Storage BOT/SCSI protocol self-test passed\n");

    if (pci_selftest() != PROS_PCI_OK) {
        serial_write("ProsenjitOS: ERROR PCI configuration self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (pros_xhci_selftest() != PROS_USB_XHCI_OK) {
        serial_write("ProsenjitOS: ERROR xHCI capability self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: xHCI capability/register bounds self-test passed\n");
    {
        int xhci_resource_rc = pros_xhci_dma_resources_selftest();
        if (xhci_resource_rc != PROS_USB_XHCI_OK) {
            serial_write("ProsenjitOS: ERROR xHCI persistent DMA resource self-test rc=");
            serial_hex64((uint64_t)(int64_t)xhci_resource_rc);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: xHCI persistent DMA resource self-test passed\n");
    if (pros_xhci_program_dma_resources_selftest() != PROS_USB_XHCI_OK) {
        serial_write("ProsenjitOS: ERROR xHCI disabled DMA programming self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: xHCI disabled DMA programming self-test passed\n");
    {
        int ring_rc = pros_xhci_ring_selftest();
        if (ring_rc != PROS_XHCI_RING_OK) {
            serial_write("ProsenjitOS: ERROR xHCI TRB ring self-test rc = ");
            serial_hex64((uint64_t)(int64_t)ring_rc);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: xHCI command/event ring self-test passed\n");
    if (pros_usb_desc_selftest() != PROS_USB_DESC_OK) {
        serial_write("ProsenjitOS: ERROR USB descriptor self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: USB MSC descriptor self-test passed\n");
    {
        uint32_t pci_count = 0u;
        if (pci_scan(pci_inventory, PROS_PCI_MAX_DEVICES, &pci_count) != PROS_PCI_OK) {
            serial_write("ProsenjitOS: ERROR PCI enumeration failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        serial_write("ProsenjitOS: PCI enumeration passed; devices = ");
        serial_hex64(pci_count);
        serial_write("\n");
        if (pci_count != 0u) {
            serial_write("ProsenjitOS: first PCI device BDF = ");
            serial_hex64(((uint64_t)pci_inventory[0].bus << 16) |
                         ((uint64_t)pci_inventory[0].slot << 8) |
                         pci_inventory[0].function);
            serial_write("\n");
        }

        /* Register the PCI inventory in the generic driver model.
         * Registration does not claim ownership; binding remains separate. */
        uint32_t registered_count = 0u;
        for (uint32_t i = 0u; i < pci_count; ++i) {
            uint32_t registered_id = 0u;
            int rc = pros_driver_register_pci_device(
                pci_inventory[i].bus, pci_inventory[i].slot,
                pci_inventory[i].function, pci_inventory[i].vendor_id,
                pci_inventory[i].device_id, pci_inventory[i].class_code,
                pci_inventory[i].subclass, pci_inventory[i].prog_if,
                &pci_inventory[i], "pci-device", &registered_id);
            if (rc == PROS_DRIVER_ENOSPC) {
                serial_write("ProsenjitOS: PCI driver-model registry full; remaining devices stay unregistered\n");
                break;
            }
            if (rc != PROS_DRIVER_OK || registered_id == 0u) {
                serial_write("ProsenjitOS: PCI device registration skipped; rc = ");
                serial_hex64((uint64_t)(int64_t)rc);
                serial_write("\n");
                continue;
            }
            ++registered_count;
        }
        serial_write("ProsenjitOS: PCI devices registered with driver model = ");
        serial_hex64(registered_count);
        serial_write("\n");

        /*
         * Exercise the real PCI -> xHCI BAR -> capability-register probe on
         * discovered xHCI controllers. This stage is deliberately read-only
         * with respect to controller ownership and DMA: pros_xhci_probe()
         * does not set PCI bus mastering, reset the controller, or start it.
         */
        uint32_t xhci_probe_count = 0u;
        uint32_t xhci_probe_failures = 0u;
        uint32_t xhci_reset_count = 0u;
        uint32_t xhci_lifecycle_failures = 0u;
        uint32_t xhci_dma_programmed = 0u;
        uint32_t xhci_dma_failures = 0u;
        for (uint32_t i = 0u; i < pci_count; ++i) {
            uint8_t controller_type = 0u;
            struct pros_xhci_controller controller = {0};
            int rc = pci_usb_controller_type(&pci_inventory[i],
                                             &controller_type);
            if (rc != PROS_PCI_OK || controller_type != PROS_PCI_USB_XHCI)
                continue;

            rc = pros_xhci_probe(&pci_inventory[i], &controller);
            if (rc != PROS_USB_XHCI_OK) {
                ++xhci_probe_failures;
                serial_write("ProsenjitOS: xHCI PCI controller probe failed; rc = ");
                serial_hex64((uint64_t)(int64_t)rc);
                serial_write("\n");
                continue;
            }

            ++xhci_probe_count;
            serial_write("ProsenjitOS: xHCI PCI controller probe passed; BDF = ");
            serial_hex64(((uint64_t)pci_inventory[i].bus << 16) |
                         ((uint64_t)pci_inventory[i].slot << 8) |
                         pci_inventory[i].function);
            serial_write("; version = ");
            serial_hex64(controller.capabilities.version);
            serial_write("; ports = ");
            serial_hex64(controller.capabilities.max_ports);
            serial_write("\n");

            /*
             * Advance through ownership -> SMI shutdown -> halt/reset, then
             * allocate and publish the complete DMA set while halted. The
             * persistent arrays above own both the BAR mapping and every DMA
             * allocation for the remainder of kernel lifetime.
             *
             * Bus mastering and USBCMD.RUN intentionally remain disabled:
             * this stage establishes a real controller-bound DMA lifetime,
             * but does not yet permit command execution or port enumeration.
             */
            rc = pros_xhci_claim_ownership(&controller, 1000000u);
            if (rc == PROS_USB_XHCI_OK)
                rc = pros_xhci_disable_legacy_smi(&controller);
            if (rc == PROS_USB_XHCI_OK)
                rc = pros_xhci_halt_reset(&controller, 1000000u);
            if (rc == PROS_USB_XHCI_OK) {
                ++xhci_reset_count;
                serial_write("ProsenjitOS: xHCI ownership/SMI/halt-reset passed; BDF = ");
                serial_hex64(((uint64_t)pci_inventory[i].bus << 16) |
                             ((uint64_t)pci_inventory[i].slot << 8) |
                             pci_inventory[i].function);
                serial_write("\n");
            } else {
                ++xhci_lifecycle_failures;
                serial_write("ProsenjitOS: xHCI ownership/SMI/halt-reset failed; rc = ");
                serial_hex64((uint64_t)(int64_t)rc);
                serial_write("\n");
                pros_xhci_detach(&controller);
                continue;
            }

            if (xhci_attached_count >= PROS_XHCI_MAX_ATTACHED) {
                ++xhci_dma_failures;
                serial_write("ProsenjitOS: xHCI persistent controller capacity reached\n");
                pros_xhci_detach(&controller);
                continue;
            }

            {
                uint32_t owner_index = xhci_attached_count;
                struct pros_xhci_dma_resources *resources =
                    &xhci_attached_dma[owner_index];
                rc = pros_xhci_dma_resources_alloc(&controller, resources);
                if (rc == PROS_USB_XHCI_OK)
                    rc = pros_xhci_program_dma_resources_disabled(
                        &controller, resources, 0u);
                if (rc == PROS_USB_XHCI_OK) {
                    rc = pros_xhci_noop_command(&controller, resources, 0u,
                                                1000000u, 10000000u);
                    if (rc == PROS_USB_XHCI_OK) {
                        serial_write("ProsenjitOS: xHCI polling No-op command completion passed\n");
                    uint8_t reset_connected_port = 0u;
                    uint8_t reset_port_id = 0u;
                    uint8_t reset_port_speed = 0u;
                    for (uint32_t port = 1u;
                         port <= controller.capabilities.max_ports; ++port) {
                        uint32_t port_status = 0u;
                        int port_rc = pros_xhci_port_reset(&controller,
                            (uint8_t)port, 1000000u, &port_status);
                        if (port_rc == PROS_USB_XHCI_OK) {
                            reset_connected_port = 1u;
                            reset_port_id = (uint8_t)port;
                            reset_port_speed = (uint8_t)((port_status >> 10) & 0x0Fu);
                            serial_write("ProsenjitOS: xHCI port reset passed; port = ");
                            serial_hex64(port);
                            serial_write("; PORTSC = ");
                            serial_hex64(port_status);
                            serial_write("\n");
                        } else if (port_rc != PROS_USB_XHCI_ENODEV &&
                                   port_rc != PROS_USB_XHCI_EUNSUPPORTED) {
                            serial_write("ProsenjitOS: xHCI port reset failed; port = ");
                            serial_hex64(port);
                            serial_write("; rc = ");
                            serial_hex64((uint64_t)(int64_t)port_rc);
                            serial_write("\n");
                        }
                    }
                    if (reset_connected_port) {
                        uint8_t slot_id = 0u;
                        int slot_rc = pros_xhci_enable_slot(&controller,
                            resources, 0u, 1000000u, &slot_id);
                        if (slot_rc == PROS_USB_XHCI_OK) {
                            serial_write("ProsenjitOS: xHCI Enable Slot command passed; slot = ");
                            serial_hex64(slot_id);
                            serial_write("\n");
                            int address_rc = pros_xhci_address_device(
                                &controller, resources, 0u, slot_id,
                                reset_port_id, reset_port_speed, 1000000u);
                            if (address_rc == PROS_USB_XHCI_OK) {
                                serial_write("ProsenjitOS: xHCI Address Device command passed; slot = ");
                                serial_hex64(slot_id);
                                serial_write("\n");
                                int descriptor_rc =
                                    pros_xhci_get_device_descriptor(
                                        &controller, resources, 0u, 1000000u);
                                if (descriptor_rc == PROS_USB_XHCI_OK) {
                                    serial_write("ProsenjitOS: xHCI full device descriptor passed; VID = ");
                                    serial_hex64(resources->device_slot.vendor_id);
                                    serial_write("; PID = ");
                                    serial_hex64(resources->device_slot.product_id);
                                    serial_write("; configurations = ");
                                    serial_hex64(resources->device_slot.num_configurations);
                                    serial_write("\n");
                                    int msc_config_rc =
                                        pros_xhci_configure_msc_interface(
                                            &controller, resources, 0u, 1000000u);
                                    if (msc_config_rc == PROS_USB_XHCI_OK) {
                                        serial_write("ProsenjitOS: xHCI MSC interface configured; interface = ");
                                        serial_hex64(resources->device_slot.msc_interface_number);
                                        serial_write("; bulk IN = ");
                                        serial_hex64(resources->device_slot.msc_endpoint_in);
                                        serial_write("; bulk OUT = ");
                                        serial_hex64(resources->device_slot.msc_endpoint_out);
                                        serial_write("\n");
                                        int bulk_config_rc =
                                            pros_xhci_configure_bulk_endpoints(
                                                &controller, resources, 0u, 1000000u);
                                        if (bulk_config_rc == PROS_USB_XHCI_OK) {
                                            serial_write("ProsenjitOS: xHCI bulk endpoint contexts configured; IN DCI = ");
                                            serial_hex64(resources->device_slot.bulk_in_dci);
                                            serial_write("; OUT DCI = ");
                                            serial_hex64(resources->device_slot.bulk_out_dci);
                                            serial_write("\n");
                                            int msc_init_rc = pros_xhci_init_mass_storage(
                                                &controller, resources, 0u, 1000000u);
                                            if (msc_init_rc == PROS_USB_XHCI_OK) {
                                                struct pros_block_device *usb_block =
                                                    pros_usb_msc_block_device(&resources->device_slot.msc);
                                                int registry_rc = block_device_register(usb_block);
                                                uint32_t registry_found = 0u;
                                                for (uint32_t registry_index = 0u;
                                                     registry_index < block_device_registry_count();
                                                     ++registry_index) {
                                                    if (block_device_registry_get(registry_index) == usb_block) {
                                                        registry_found = 1u; break;
                                                    }
                                                }
                                                if (registry_rc != PROS_BLOCK_OK || !registry_found) {
                                                    serial_write("ProsenjitOS: ERROR USB MSC block-device registration failed; rc = ");
                                                    serial_hex64((uint64_t)(int64_t)registry_rc);
                                                    serial_write("\n");
                                                } else {
                                                    serial_write("ProsenjitOS: USB MSC block-device registered in kernel registry; count = ");
                                                    serial_hex64(block_device_registry_count());
                                                    serial_write("\n");
                                                }
                                                serial_write("ProsenjitOS: xHCI USB MSC BOT/SCSI INQUIRY and READ CAPACITY passed; sectors = ");
                                                serial_hex64(resources->device_slot.msc.sector_count);
                                                serial_write("; sector size = ");
                                                serial_hex64(resources->device_slot.msc.sector_size);
                                                serial_write("\n");
                                                int persistence_rc =
                                                    xhci_usb_fatfs_persistence_test(
                                                        &resources->device_slot.msc);
                                                if (persistence_rc != PROS_STORAGE_OK) {
                                                    serial_write("ProsenjitOS: USB FAT32 persistence test failed; rc = ");
                                                    serial_hex64((uint64_t)(int64_t)persistence_rc);
                                                    serial_write("\n");
                                                }
                                            } else {
                                                serial_write("ProsenjitOS: xHCI USB MSC BOT/SCSI initialization failed; rc = ");
                                                serial_hex64((uint64_t)(int64_t)msc_init_rc);
                                                serial_write("\n");
                                            }
                                        } else {
                                            serial_write("ProsenjitOS: xHCI bulk endpoint configuration failed; rc = ");
                                            serial_hex64((uint64_t)(int64_t)bulk_config_rc);
                                            serial_write("\n");
                                        }
                                    } else {
                                        serial_write("ProsenjitOS: xHCI MSC interface configuration failed; rc = ");
                                        serial_hex64((uint64_t)(int64_t)msc_config_rc);
                                        serial_write("\n");
                                    }
                                } else {
                                    serial_write("ProsenjitOS: xHCI EP0 descriptor request failed; rc = ");
                                    serial_hex64((uint64_t)(int64_t)descriptor_rc);
                                    serial_write("\n");
                                }
                            } else {
                                serial_write("ProsenjitOS: xHCI Address Device command failed; rc = ");
                                serial_hex64((uint64_t)(int64_t)address_rc);
                                serial_write("\n");
                            }
                        } else {
                            serial_write("ProsenjitOS: xHCI Enable Slot command failed; rc = ");
                            serial_hex64((uint64_t)(int64_t)slot_rc);
                            serial_write("\n");
                        }
                    }
                    }
                    else {
                        serial_write("ProsenjitOS: xHCI polling No-op command failed; rc = ");
                        serial_hex64((uint64_t)(int64_t)rc);
                        serial_write("\n");
                        {
                            serial_write("ProsenjitOS: xHCI pre-recovery snapshot; USBCMD = ");
                            serial_hex64(resources->diagnostic_usbcmd);
                            serial_write("; USBSTS = ");
                            serial_hex64(resources->diagnostic_usbsts);
                            serial_write("; IMAN = ");
                            serial_hex64(resources->diagnostic_iman);
                            serial_write("; Doorbell = ");
                            serial_hex64(resources->diagnostic_doorbell);
                            serial_write("; CRCR = ");
                            serial_hex64(resources->diagnostic_crcr);
                            serial_write("; ERDP = ");
                            serial_hex64(resources->diagnostic_erdp);
                            serial_write("; command TRB control = ");
                            serial_hex64(resources->diagnostic_command_control);
                            serial_write("; event TRB control = ");
                            serial_hex64(resources->diagnostic_event_control);
                            serial_write("; DMA DCBAA/CRCR/Event/ERST = ");
                            serial_hex64(resources->dcbaa.physical_address);
                            serial_write("/");
                            serial_hex64(resources->command_ring_dma.physical_address);
                            serial_write("/");
                            serial_hex64(resources->event_ring_dma.physical_address);
                            serial_write("/");
                            serial_hex64(resources->erst_dma.physical_address);
                            serial_write("; ERST segment base/size = ");
                            serial_hex64(resources->erst.entries[0].segment_base);
                            serial_write("/");
                            serial_hex64(resources->erst.entries[0].segment_size);
                            serial_write("; scratchpad array/pool/count = ");
                            serial_hex64(resources->scratchpads.pointer_array.physical_address);
                            serial_write("/");
                            serial_hex64(resources->scratchpads.buffer_pool.physical_address);
                            serial_write("/");
                            serial_hex64(resources->scratchpads.count);
                            serial_write("; event dequeue/cycle = ");
                            serial_hex64(((uint64_t)resources->diagnostic_event_dequeue_index << 8) |
                                         resources->diagnostic_event_cycle);
                            serial_write("; command enqueue/outstanding/cycle = ");
                            serial_hex64(((uint64_t)resources->diagnostic_command_enqueue_index << 16) |
                                         ((uint64_t)resources->diagnostic_command_outstanding << 8) |
                                         resources->diagnostic_command_cycle);
                            serial_write("\n");
                        }
                    }
                }

                if (rc == PROS_USB_XHCI_OK || resources->hardware_referenced) {
                    /* A published DMA address permanently pins this owner. */
                    xhci_attached_controllers[owner_index] = controller;
                    /*
                     * The controller discovered in this loop is stack-local.
                     * Any published MSC transport must instead retain the
                     * persistent controller copy for subsequent block I/O.
                     */
                    if (pros_xhci_pin_controller_owner(resources,
                            &controller,
                            &xhci_attached_controllers[owner_index]) !=
                            PROS_USB_XHCI_OK) {
                        int detach_rc = pros_usb_msc_detach(
                            &resources->device_slot.msc);
                        serial_write("ProsenjitOS: ERROR xHCI MSC controller owner rebinding failed; block publication revoked; detach rc = ");
                        serial_hex64((uint64_t)(int64_t)detach_rc);
                        serial_write("; DMA owner retained\\n");
                    }
                    ++xhci_attached_count;
                    if (rc == PROS_USB_XHCI_OK) {
                        ++xhci_dma_programmed;
                        serial_write("ProsenjitOS: xHCI persistent DMA resources programmed; BDF = ");
                    } else {
                        ++xhci_dma_failures;
                        serial_write("ProsenjitOS: xHCI DMA programming failed; resources retained safely; rc = ");
                        serial_hex64((uint64_t)(int64_t)rc);
                        serial_write("; BDF = ");
                    }
                    serial_hex64(((uint64_t)pci_inventory[i].bus << 16) |
                                 ((uint64_t)pci_inventory[i].slot << 8) |
                                 pci_inventory[i].function);
                    serial_write("\n");
                } else {
                    /* No address was published: rollback is safe to attempt. */
                    int release_rc = pros_xhci_dma_resources_release(resources);
                    if (release_rc == PROS_USB_XHCI_OK) {
                        pros_xhci_detach(&controller);
                    } else {
                        /* Preserve partial allocator ownership for retry/lifetime safety. */
                        xhci_attached_controllers[owner_index] = controller;
                        ++xhci_attached_count;
                    }
                    ++xhci_dma_failures;
                    serial_write("ProsenjitOS: xHCI DMA resource setup failed; rc = ");
                    serial_hex64((uint64_t)(int64_t)rc);
                    serial_write("; release rc = ");
                    serial_hex64((uint64_t)(int64_t)release_rc);
                    serial_write("\n");
                }
            }
        }
        serial_write("ProsenjitOS: xHCI PCI probe summary; detected = ");
        serial_hex64(xhci_probe_count);
        serial_write("; failures = ");
        serial_hex64(xhci_probe_failures);
        serial_write("\n");
        serial_write("ProsenjitOS: xHCI lifecycle summary; reset = ");
        serial_hex64(xhci_reset_count);
        serial_write("; failures = ");
        serial_hex64(xhci_lifecycle_failures);
        serial_write("\n");
        serial_write("ProsenjitOS: xHCI persistent DMA summary; programmed = ");
        serial_hex64(xhci_dma_programmed);
        serial_write("; failures = ");
        serial_hex64(xhci_dma_failures);
        serial_write("; retained controllers = ");
        serial_hex64(xhci_attached_count);
        serial_write("\n");
    }

    /*
     * Build a two-segment static ELF64 image. Code is RX, test data is RW/NX,
     * and the ELF loader constructs a private argv/envp stack.
     */
    uint64_t user_text_start = (uint64_t)(uintptr_t)__user_text_start;
    uint64_t user_text_end = (uint64_t)(uintptr_t)__user_text_end;
    if (user_text_end <= user_text_start) {
        serial_write("ProsenjitOS: ERROR CPL3 user text layout invalid\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    uint64_t user_text_size = user_text_end - user_text_start;
    const uint64_t boot_elf_offset = 0x1000ULL;
    const uint64_t boot_elf_header_size =
        sizeof(struct boot_elf_header) + 2u * sizeof(struct boot_elf_phdr);
    if (user_text_size > PROSENJIT_PAGE_SIZE ||
        user_text_size > BOOT_ELF_IMAGE_CAP - boot_elf_offset ||
        boot_elf_header_size > boot_elf_offset) {
        serial_write("ProsenjitOS: ERROR boot ELF image capacity/layout exceeded\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    for (uint32_t i = 0; i < BOOT_ELF_IMAGE_CAP; ++i)
        boot_user_elf[i] = 0;
    struct boot_elf_header *eh =
        (struct boot_elf_header *)(uintptr_t)boot_user_elf;
    struct boot_elf_phdr *ph =
        (struct boot_elf_phdr *)(uintptr_t)(boot_user_elf + sizeof(*eh));
    struct boot_elf_phdr *data_ph = ph + 1;
    eh->ident[0] = 0x7F; eh->ident[1] = 'E'; eh->ident[2] = 'L'; eh->ident[3] = 'F';
    eh->ident[4] = 2; eh->ident[5] = 1; eh->ident[6] = 1; eh->ident[7] = 0;
    eh->type = 2; eh->machine = 62; eh->version = 1;
    eh->entry = PROSENJIT_USER_CODE_VA;
    eh->phoff = sizeof(*eh);
    eh->ehsize = sizeof(*eh);
    eh->phentsize = sizeof(*ph);
    eh->phnum = 2;

    ph->type = 1;
    ph->flags = 1 | 4;
    ph->offset = boot_elf_offset;
    ph->vaddr = PROSENJIT_USER_CODE_VA;
    ph->paddr = 0;
    ph->filesz = user_text_size;
    ph->memsz = user_text_size;
    ph->align = PROSENJIT_PAGE_SIZE;

    uint64_t data_segment_offset =
        (boot_elf_offset + user_text_size + PROSENJIT_PAGE_SIZE - 1ULL) &
        ~(PROSENJIT_PAGE_SIZE - 1ULL);
    if (data_segment_offset > BOOT_ELF_IMAGE_CAP) {
        serial_write("ProsenjitOS: ERROR boot ELF data segment exceeds image capacity\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    data_ph->type = 1;
    data_ph->flags = 4 | 2;
    data_ph->offset = data_segment_offset;
    data_ph->vaddr = PROSENJIT_USER_TEST_VA;
    data_ph->paddr = 0;
    data_ph->filesz = 0;
    data_ph->memsz = PROSENJIT_PAGE_SIZE;
    data_ph->align = PROSENJIT_PAGE_SIZE;
    boot_elf_image_size = data_segment_offset;

    for (uint64_t i = 0; i < user_text_size; ++i)
        boot_user_elf[boot_elf_offset + i] =
            ((const uint8_t *)(uintptr_t)user_text_start)[i];

    __asm__ volatile ("cli" ::: "memory");

    if (process_spawn_rollback_selftest(&init_process, boot_user_elf,
                                        boot_elf_image_size, boot_elf_argv, 2u,
                                        boot_elf_envp, 1u) != 0) {
        serial_write("ProsenjitOS: ERROR native spawn staged rollback failpoints failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: native spawn staged rollback failpoints 1-5 passed\n");

    /* Restore the parent process/CR3 after the scheduler and lifecycle self-tests. */
    process_set_current(&init_process);
    if (process_current() != &init_process) {
        serial_write("ProsenjitOS: ERROR parent process context restore before native spawn failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* Retire synthetic scheduler-test threads before the real first-user launch. */
    int cleanup_terminate_rc = thread_terminate_process(&init_process);
    int cleanup_reap_rc = cleanup_terminate_rc == 0 ?
                          thread_reap_process(&init_process) : -1;
    if (cleanup_terminate_rc != 0 || cleanup_reap_rc != 0 ||
        thread_current() != 0 || thread_runnable_count() != 0 ||
        process_current() != &init_process) {
        serial_write("ProsenjitOS: ERROR scheduler test-thread cleanup before native spawn failed terminate=");
        serial_hex64((uint64_t)(int64_t)cleanup_terminate_rc);
        serial_write(" reap=");
        serial_hex64((uint64_t)(int64_t)cleanup_reap_rc);
        serial_write(" reapable=");
        serial_hex64((uint64_t)thread_process_reapable(&init_process));
        serial_write(" current_thread=");
        serial_hex64((uint64_t)(uintptr_t)thread_current());
        serial_write(" runnable=");
        serial_hex64(thread_runnable_count());
        serial_write(" current_process=");
        serial_hex64((uint64_t)(uintptr_t)process_current());
        serial_write(" tss_rsp0=");
        serial_hex64(cpu_get_kernel_stack());
        serial_write("\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    int spawn_rc = process_spawn(&init_process, &boot_spawn_child,
                                 &boot_spawn_thread, 0xB002u, 0xB002u,
                                 PROS_SECURITY_LIMITED, boot_user_elf,
                                 boot_elf_image_size, boot_elf_argv, 2u,
                                 boot_elf_envp, 1u);
    if (spawn_rc != 0) {
        serial_write("ProsenjitOS: ERROR transactional native process spawn failed rc=");
        serial_hex64((uint64_t)(int64_t)spawn_rc);
        serial_write(" elf_stage=");
        serial_hex64((uint64_t)pros_elf_last_load_stage());
        serial_write(" wrapper_stage=");
        serial_hex64((uint64_t)pros_elf_last_error_stage());
        serial_write(" address_map_stage=");
        serial_hex64((uint64_t)address_space_last_map_failure());
        serial_write("\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    uint64_t entry_physical = 0, entry_flags = 0;
    uint64_t data_physical = 0, data_flags = 0;
    uint64_t stack_physical = 0, stack_flags = 0;
    if (address_space_translate_user(&boot_spawn_child.address_space,
                                     boot_spawn_child.user_entry,
                                     &entry_physical, &entry_flags) != 0 ||
        !(entry_flags & PROSENJIT_PAGE_USER) ||
        (entry_flags & PROSENJIT_PAGE_NX) != 0 ||
        (entry_flags & PROSENJIT_PAGE_WRITABLE) != 0 ||
        ((uint8_t *)(uintptr_t)entry_physical)[0] !=
            ((uint8_t *)(uintptr_t)user_text_start)[0] ||
        address_space_translate_user(&boot_spawn_child.address_space,
                                     PROSENJIT_USER_TEST_VA,
                                     &data_physical, &data_flags) != 0 ||
        !(data_flags & PROSENJIT_PAGE_USER) ||
        !(data_flags & PROSENJIT_PAGE_WRITABLE) ||
        !(data_flags & PROSENJIT_PAGE_NX) ||
        address_space_translate_user(&boot_spawn_child.address_space,
                                     boot_spawn_child.user_stack_top,
                                     &stack_physical, &stack_flags) != 0 ||
        !(stack_flags & PROSENJIT_PAGE_USER) ||
        !(stack_flags & PROSENJIT_PAGE_WRITABLE) ||
        !(stack_flags & PROSENJIT_PAGE_NX) ||
        *(uint64_t *)(uintptr_t)stack_physical != 2u) {
        serial_write("ProsenjitOS: ERROR native spawn ELF mapping/argv stack verification failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /*
     * Verify the actual scheduler-created child's complete initial argv/envp
     * vector and strings. The ELF-loader unit fixture alone is not enough:
     * this checks the stack attached to the process that will enter CPL3.
     */
    {
        uint64_t *initial = (uint64_t *)(uintptr_t)stack_physical;
        uint64_t argv0 = initial[1];
        uint64_t argv1 = initial[2];
        uint64_t env0 = initial[4];
        const char expected_argv0[] = "/init";
        const char expected_argv1[] = "elf-runtime";
        const char expected_env0[] = "PROS_ELF_RUNTIME=1";
        const uint8_t *stack_page =
            (const uint8_t *)(uintptr_t)(stack_physical &
                                         ~(PROSENJIT_PAGE_SIZE - 1ULL));
        int strings_valid = 1;

        if ((boot_spawn_child.user_stack_top & 0xFULL) != 0u ||
            boot_spawn_child.user_stack_top < PROSENJIT_USER_STACK_VA ||
            boot_spawn_child.user_stack_top >
                PROSENJIT_USER_STACK_TOP - 6u * sizeof(uint64_t) ||
            initial[0] != 2u || initial[3] != 0u || initial[5] != 0u ||
            argv0 < boot_spawn_child.user_stack_top ||
            argv0 > PROSENJIT_USER_STACK_TOP - sizeof(expected_argv0) ||
            argv1 < boot_spawn_child.user_stack_top ||
            argv1 > PROSENJIT_USER_STACK_TOP - sizeof(expected_argv1) ||
            env0 < boot_spawn_child.user_stack_top ||
            env0 > PROSENJIT_USER_STACK_TOP - sizeof(expected_env0)) {
            strings_valid = 0;
        } else {
            const uint8_t *actual_argv0 = stack_page +
                (argv0 - PROSENJIT_USER_STACK_VA);
            const uint8_t *actual_argv1 = stack_page +
                (argv1 - PROSENJIT_USER_STACK_VA);
            const uint8_t *actual_env0 = stack_page +
                (env0 - PROSENJIT_USER_STACK_VA);
            for (uint32_t i = 0; i < sizeof(expected_argv0); ++i)
                if (actual_argv0[i] != (uint8_t)expected_argv0[i])
                    strings_valid = 0;
            for (uint32_t i = 0; i < sizeof(expected_argv1); ++i)
                if (actual_argv1[i] != (uint8_t)expected_argv1[i])
                    strings_valid = 0;
            for (uint32_t i = 0; i < sizeof(expected_env0); ++i)
                if (actual_env0[i] != (uint8_t)expected_env0[i])
                    strings_valid = 0;
        }

        if (!strings_valid) {
            serial_write("ProsenjitOS: ERROR native spawn argv/envp contents or terminators invalid\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: native spawn ELF mapping and argv/envp verification passed\n");
    serial_write("ProsenjitOS: native spawn argv/envp contents verified\n");

    if (process_grant_handle(&boot_spawn_child, &user_test_channel.object,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_WRITE |
                             PROSENJIT_CAP_DUPLICATE | PROSENJIT_CAP_TRANSFER,
                             &boot_spawn_child_handle) != 0) {
        serial_write("ProsenjitOS: ERROR native child IPC capability grant failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    boot_spawn_thread.user_interrupt_frame_storage.rdi = boot_spawn_child_handle;

    scheduler_init_frame.rax = scheduler_init_frame.rbx = 0;
    scheduler_init_frame.rcx = scheduler_init_frame.rdx = 0;
    scheduler_init_frame.rsi = scheduler_init_frame.rdi = 0;
    scheduler_init_frame.rbp = scheduler_init_frame.r8 = 0;
    scheduler_init_frame.r9 = scheduler_init_frame.r10 = 0;
    scheduler_init_frame.r11 = scheduler_init_frame.r12 = 0;
    scheduler_init_frame.r13 = scheduler_init_frame.r14 = 0;
    scheduler_init_frame.r15 = 0;
    scheduler_init_frame.rip = (uint64_t)(uintptr_t)kernel_main;
    scheduler_init_frame.cs = 0x08u;
    scheduler_init_frame.rflags = 0x202u;

    uint64_t *saved_child_root = boot_spawn_child.address_space.root;
    boot_spawn_child.address_space.root = 0;
    struct pros_interrupt_frame *failed_handoff_frame =
        thread_schedule_interrupt(&scheduler_init_frame,
                                  PROS_INTERRUPT_FRAME_KERNEL);
    boot_spawn_child.address_space.root = saved_child_root;
    if (failed_handoff_frame != &scheduler_init_frame ||
        thread_current() != 0 || process_current() != &init_process ||
        thread_runnable_count() != 1u ||
        cpu_get_kernel_stack() != (uint64_t)(uintptr_t)stack_top) {
        serial_write("ProsenjitOS: ERROR native spawn scheduler handoff rollback failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: native spawn scheduler handoff rollback passed\n");

    struct pros_interrupt_frame *first_user_frame =
        thread_schedule_interrupt(&scheduler_init_frame,
                                  PROS_INTERRUPT_FRAME_KERNEL);
    if (thread_current() != &boot_spawn_thread ||
        process_current() != &boot_spawn_child ||
        first_user_frame != &boot_spawn_thread.user_interrupt_frame_storage ||
        boot_spawn_thread.interrupt_frame_kind != PROS_INTERRUPT_FRAME_USER ||
        boot_spawn_thread.user_interrupt_frame_storage.cs != 0x1Bu ||
        boot_spawn_thread.user_interrupt_frame_storage.user_ss != 0x23u ||
        boot_spawn_thread.user_interrupt_frame_storage.rflags != 0x2u ||
        process_set_state(&boot_spawn_child, PROS_PROCESS_RUNNING) != 0 ||
        process_state(&boot_spawn_child) != PROS_PROCESS_RUNNING ||
        cpu_get_kernel_stack() !=
            (uint64_t)(uintptr_t)boot_spawn_thread.kernel_stack_base +
            boot_spawn_thread.kernel_stack_size) {
        serial_write("ProsenjitOS: ERROR scheduler-created CPL3 first-entry handoff failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: entering scheduler-created CPL3 native process\n");
    thread_enter_interrupt_frame(first_user_frame);
}
