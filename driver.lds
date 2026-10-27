SECTIONS
{
    .early_init : ALIGN(8) {
        PROVIDE(__start_early_init = .);
        KEEP(*(.early_init))
        PROVIDE(__stop_early_init = .);
    }
    .core_init : ALIGN(8) {
        PROVIDE(__start_core_init = .);
        KEEP(*(.core_init))
        PROVIDE(__stop_core_init = .);
    }
    .drivers_init : ALIGN(8) {
        PROVIDE(__start_drivers_init = .);
        KEEP(*(.drivers_init))
        PROVIDE(__stop_drivers_init = .);
    }
    .postcore_init : ALIGN(8) {
        PROVIDE(__start_postcore_init = .);
        KEEP(*(.postcore_init))
        PROVIDE(__stop_postcore_init = .);
    }
}
INSERT AFTER .rodata;