global start
extern long_mode_start

section .text
bits 32
start:
    mov esp, kernel_stack_top
    mov edi, ebx       ; Move Multiboot info pointer to edi

    call check_multiboot
    call check_cpuid
    call check_long_mode

    call set_up_page_tables
    call enable_paging

    ; load the 64-bit GDT
    lgdt [gdt64.pointer]

    jmp gdt64.code:long_mode_start


; Prints `ERR: ` and the given error code to screen and hangs.
; parameter: error code (in ascii) in al
boot_error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte  [0xb800a], al
    hlt


check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "0"
    jmp boot_error


check_cpuid:
    ; Check if CPUID is supported by attempting to flip the ID bit (bit 21)
    ; in the FLAGS register. If we can flip it, CPUID is available.

    ; Copy FLAGS in to EAX via stack
    pushfd
    pop eax

    ; Copy to ECX as well for comparing later on
    mov ecx, eax

    ; Flip the ID bit
    xor eax, 1 << 21

    ; Copy EAX to FLAGS via the stack
    push eax
    popfd

    ; Copy FLAGS back to EAX (with the flipped bit if CPUID is supported)
    pushfd
    pop eax

    ; Restore FLAGS from the old version stored in ECX (i.e. flipping the
    ; ID bit back if it was ever flipped).
    push ecx
    popfd

    ; Compare EAX and ECX. If they are equal then that means the bit
    ; wasn't flipped, and CPUID isn't supported.
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, "1"
    jmp boot_error


check_long_mode:
    ; test if extended processor info in available
    mov eax, 0x80000000    ; implicit argument for cpuid
    cpuid                  ; get highest supported argument
    cmp eax, 0x80000001    ; it needs to be at least 0x80000001
    jb .no_long_mode       ; if it's less, the CPU is too old for long mode

    ; use extended info to test if long mode is available
    mov eax, 0x80000001    ; argument for extended processor info
    cpuid                  ; returns various feature bits in ecx and edx
    test edx, 1 << 29      ; test if the LM-bit is set in the D-register
    jz .no_long_mode       ; If it's not set, there is no long mode
    ret
.no_long_mode:
    mov al, "2"
    jmp boot_error


set_up_page_tables:
    ; map first and the second last P4 entry to P3 table
    ; [0, 512GiB) and [-1024GiB, -512GiB)
    mov eax, p3_table
    or eax, 0b11 ; present + writable
    or eax, 0b100      ; user-accessible
    mov [p4_table], eax  ; first entry
    mov [p4_table + 4096 - 16], eax  ; the second last entry

    ; map each P2 entry to a huge 2MiB page
    mov ecx, 0         ; counter variable

.map_p3_table:
    ; map ecx-th P3 entry to a huge page that starts at address 1GiB*ecx
    mov eax, 0x40000000  ; 1GiB
    mul ecx            ; start address of ecx-th page
    or eax, 0b10000011 ; present + writable + huge
    or eax, 0b100      ; user-accessible
    mov [p3_table + ecx * 8], eax ; map ecx-th entry

    inc ecx            ; increase counter
    cmp ecx, 512       ; if counter == 512, the whole P3 table is mapped
    jne .map_p3_table  ; else map the next entry

    ret


enable_paging:
    ; load P4 to cr3 register (cpu uses this to access the P4 table)
    mov eax, p4_table
    mov cr3, eax

    ; enable PAE-flag in cr4 (Physical Address Extension)
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; enable CR4.FSGSBASE[bit 16]
    mov eax, cr4
    or eax, 1 << 16
    mov cr4, eax

    ; set the long mode bit in the EFER MSR (model specific register)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    
    ; set the NXE (No-Execute Enable) bit in EFER
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 11
    wrmsr

    ; enable paging in the cr0 register
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ret


section .rodata
gdt64:
    dq 0 ; zero entry
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; code segment
.pointer:
    dw $ - gdt64 - 1
    dq gdt64


section .bss

align 4096
p4_table:
    resb 4096
p3_table:
    resb 4096

kernel_stack:
    resb 4096 * 10  ; 40 KiB
global kernel_stack_top
kernel_stack_top:
