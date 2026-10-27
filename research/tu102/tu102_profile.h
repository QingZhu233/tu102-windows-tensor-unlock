#ifndef TU102_WINDOWS_RESEARCH_PROFILE_H
#define TU102_WINDOWS_RESEARCH_PROFILE_H

#include <stdint.h>

#define TU102_VENDOR_DEVICE 0x1e0710deU
#define TU102_SUBSYSTEM 0x12a410deU
#define TU102_SEGMENT 0U
#define TU102_BUS 3U
#define TU102_DEVICE 0U
#define TU102_FUNCTION 0U

typedef struct {
    uint32_t segment;
    uint32_t bus;
    uint32_t device;
    uint32_t function;
    uint32_t vendor_device;
    uint32_t subsystem;
} tu102_pci_identity;

/* Identifies the research target only; it does not authorize MMIO or DMA. */
static inline int tu102_identity_matches(const tu102_pci_identity *id) {
    return id && id->segment == TU102_SEGMENT && id->bus == TU102_BUS &&
           id->device == TU102_DEVICE && id->function == TU102_FUNCTION &&
           id->vendor_device == TU102_VENDOR_DEVICE &&
           id->subsystem == TU102_SUBSYSTEM;
}

/* No exact PROM, FWSEC, selector, framebuffer or DMA profile is validated. */
static inline int tu102_transaction_profile_ready(void) {
    return 0;
}

#endif
