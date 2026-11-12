#pragma once
// Native shim: the sim has no GPIO interrupts, so installing the ISR service
// is a no-op.

static inline int gpio_install_isr_service(int intr_alloc_flags) {
    (void)intr_alloc_flags;
    return 0;
}
