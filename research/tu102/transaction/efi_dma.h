/* Local TU102 experimental UEFI adapter; copyleft-next-0.3.1.
 * Normal EFI_PCI_IO AllocateBuffer/Map only, no physical-address bypass.
 * Caller verifies full PROM/firmware hashes, display ownership, PCI identity
 * and assets BEFORE setting profile_verified. Host tests use injected services.
 */
#ifndef LOCAL_TU102_EFI_DMA_H
#define LOCAL_TU102_EFI_DMA_H
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include "../tu102_profile.h"
#include "tu102_core.h"

typedef struct {
    EFI_BOOT_SERVICES *bs;
    EFI_PCI_IO_PROTOCOL *pci;
    EFI_HANDLE image, handle;
    EFI_GUID pci_guid;
    UINT64 original_attrs;
    UINT16 original_command;
    UINT16 release_command;
    const char *dma_step;
    UINT64 dma_supported,dma_active;
    EFI_STATUS dma_allocate_status,dma_map_status;
    UINTN dma_mapped_bytes;
    VOID *mapping;
    VOID *pool;
    EFI_PHYSICAL_ADDRESS device_address;
    BOOLEAN owned,attrs_saved,dac_changed,mapped,master_pending,master_enabled;
    BOOLEAN profile_verified,retained,released,fault;
    BOOLEAN read_only_probe;
    BOOLEAN allocation_probe; /* host common-buffer API checks only, never arm */
    BOOLEAN direct_physical,pages_owned;
    VOID *direct_root;
    EFI_STATUS last_status;
    UINT32 last_offset;
    int (*log_stage)(void *,unsigned,uint32_t,uint32_t);
    void *log_context;
    Tx tx;
} TuEfi;
#ifdef TU102_AMD_DIRECT_PHYSICAL
static EFI_STATUS da_guard(TuEfi *p);
static EFI_STATUS da_allocate(TuEfi *p);
#endif

static EFI_STATUS te_config(TuEfi *p,UINT32 off,UINT32 *v) {
    if(!p || !p->pci || !p->pci->Pci.Read || !v)return EFI_INVALID_PARAMETER;
    *v=0;
    return p->pci->Pci.Read(p->pci,EfiPciIoWidthUint32,off,1,v);
}
static EFI_STATUS te_identity(TuEfi *p,UINT16 expected_command) {
    UINTN seg=0,bus=0,dev=0,fn=0;
    UINT32 id=0,sub=0,bar=0,cmd=0;
    if(!p || !p->pci || !p->pci->GetLocation)return EFI_INVALID_PARAMETER;
    EFI_STATUS st=p->pci->GetLocation(p->pci,&seg,&bus,&dev,&fn);
    if(!EFI_ERROR(st))st=te_config(p,0,&id);
    if(!EFI_ERROR(st))st=te_config(p,0x2c,&sub);
    if(!EFI_ERROR(st))st=te_config(p,0x10,&bar);
    if(!EFI_ERROR(st))st=te_config(p,4,&cmd);
    if(EFI_ERROR(st))return st;
    if(seg!=0 || bus!=3 || dev || fn || id!=TU102_VENDOR_DEVICE || sub!=TU102_SUBSYSTEM ||
       bar!=0xdd000000U || (UINT16)cmd!=expected_command)return EFI_SECURITY_VIOLATION;
    return EFI_SUCCESS;
}
static EFI_STATUS te_claim_at(TuEfi *p,UINT16 expected_command) {
    if(!p || !p->bs || !p->bs->OpenProtocol || !p->bs->CloseProtocol ||
       !p->image || !p->handle || !p->pci || p->owned || p->released || p->fault ||
       !p->profile_verified || !p->bs->ProtocolsPerHandle ||
       !p->bs->OpenProtocolInformation || !p->bs->FreePool ||
       !p->bs->RaiseTPL || !p->bs->RestoreTPL)return EFI_ACCESS_DENIED;
    if(expected_command&~3U)return EFI_ACCESS_DENIED;
    EFI_STATUS st=te_identity(p,expected_command);
    if(EFI_ERROR(st))return st;
    p->original_command=3;
    /* EXCLUSIVE is NOT passive: the UEFI implementation may call Stop on
     * current BY_DRIVER owners. Reject all such ownership on every target
     * protocol first, without a callback window between the snapshot/claim.
     * No driver-disconnect fallback is implemented for this untested board. */
    EFI_TPL previous=p->bs->RaiseTPL(TPL_NOTIFY);
    EFI_GUID **protocols=0;
    UINTN protocol_count=0;
    st=p->bs->ProtocolsPerHandle(p->handle,&protocols,&protocol_count);
    if(!EFI_ERROR(st) && (!protocols || !protocol_count || protocol_count>128))st=EFI_UNSUPPORTED;
    for(UINTN i=0;!EFI_ERROR(st) && i<protocol_count;i++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *records=0;
        UINTN count=0;
        st=p->bs->OpenProtocolInformation(p->handle,protocols[i],&records,&count);
        if(!EFI_ERROR(st) && (count>256 || (count && !records)))st=EFI_UNSUPPORTED;
        for(UINTN j=0;!EFI_ERROR(st) && j<count;j++) {
            UINT32 flags=records[j].Attributes;
            if(flags!=EFI_OPEN_PROTOCOL_GET_PROTOCOL &&
               flags!=EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL &&
               flags!=EFI_OPEN_PROTOCOL_TEST_PROTOCOL)st=EFI_ACCESS_DENIED;
        }
        if(records){EFI_STATUS f=p->bs->FreePool(records);if(!EFI_ERROR(st) && EFI_ERROR(f))st=f;}
    }
    if(protocols){EFI_STATUS f=p->bs->FreePool(protocols);if(!EFI_ERROR(st) && EFI_ERROR(f))st=f;}
    EFI_PCI_IO_PROTOCOL *owned=0;
    if(!EFI_ERROR(st))st=p->bs->OpenProtocol(p->handle,&p->pci_guid,(VOID **)&owned,
        p->image,0,EFI_OPEN_PROTOCOL_EXCLUSIVE);
    p->bs->RestoreTPL(previous);
    if(EFI_ERROR(st))return st;
    p->owned=TRUE;
    if(owned!=p->pci){p->fault=TRUE;return EFI_COMPROMISED_DATA;}
    return EFI_SUCCESS;
}
static EFI_STATUS te_claim(TuEfi *p) {return te_claim_at(p,3);}

static EFI_STATUS te_allocate_options(TuEfi *p,EFI_MEMORY_TYPE memory,UINT64 cache) {
    if(!p || (!p->owned && !p->allocation_probe) || !p->profile_verified || p->fault || p->pool || p->mapping ||
       p->mapped || p->retained || p->released || p->tx.mutated ||
       !p->pci->Attributes || !p->pci->AllocateBuffer || !p->pci->FreeBuffer ||
       !p->pci->Map || !p->pci->Unmap)return EFI_ACCESS_DENIED;
    if((memory!=EfiBootServicesData && memory!=EfiRuntimeServicesData) ||
       (cache!=0 && cache!=EFI_PCI_IO_ATTRIBUTE_MEMORY_CACHED))return EFI_INVALID_PARAMETER;
    p->dma_step="identity";
    p->dma_allocate_status=p->dma_map_status=EFI_NOT_READY;
    EFI_STATUS st=te_identity(p,p->original_command);
    if(EFI_ERROR(st))return st;
    UINT64 supported=0;
    p->dma_step="supported-attributes";
    st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationSupported,0,&supported);
    p->dma_supported=supported;
    if(EFI_ERROR(st))return st;
    if(!(supported&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE) ||
       !(supported&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))return EFI_UNSUPPORTED;
    p->dma_step="current-attributes";
    st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationGet,0,&p->original_attrs);
    if(EFI_ERROR(st))return st;
    p->attrs_saved=TRUE;
    if(p->original_attrs&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)return EFI_SECURITY_VIOLATION;
    if(!(p->original_attrs&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE)) {
        /* A diagnostic profile must not toggle bus attributes underneath an
         * active controller driver. The unlock path reaches this point only
         * after its narrowly verified driver release and exclusive claim. */
        if(p->allocation_probe)return EFI_UNSUPPORTED;
        p->dma_step="enable-DAC";
        p->dac_changed=TRUE; /* also restore after partially applied API error */
        st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationEnable,
            EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,0);
        if(EFI_ERROR(st))return st;
    }
    st=te_identity(p,p->original_command);
    if(EFI_ERROR(st))return st;
    p->dma_step="verify-DAC";
    st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationGet,0,&p->dma_active);
    if(EFI_ERROR(st))return st;
    if(!(p->dma_active&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE) ||
       (p->dma_active&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))return EFI_SECURITY_VIOLATION;
    p->dma_step="AllocateBuffer";
    /* EDK2 PciIoAllocateBuffer rejects a DAC bit in this argument; it derives
     * DAC from the already enabled device Attributes and passes it down to
     * the root bridge internally. Use0 here, not a raw/root-bridge bypass.
     * Reference: edk2 dd87bafc40adb36d96e2bcc96ae1be464709db69 PciIo.c1120-1139. */
    st=p->pci->AllocateBuffer(p->pci,AllocateAnyPages,memory,
        TX_POOL_SIZE/4096,&p->pool,cache);
    p->dma_allocate_status=st;
    if(EFI_ERROR(st))return st;
    if(!p->pool || ((UINTN)p->pool&4095))return EFI_COMPROMISED_DATA;
    UINTN bytes=TX_POOL_SIZE;
    p->dma_step="Map-common-buffer";
    st=p->pci->Map(p->pci,EfiPciIoOperationBusMasterCommonBuffer,p->pool,
        &bytes,&p->device_address,&p->mapping);
    p->dma_map_status=st;p->dma_mapped_bytes=bytes;
    /* An error is never made successful by an apparently usable result. A
     * non-null token on failure is conservatively retained until Unmap. */
    p->mapped=(!EFI_ERROR(st) || p->mapping!=0);
    if(EFI_ERROR(st))return st;
    p->dma_step="validate-returned-mapping";
    /* Mapping is an opaque firmware value, not proof of translation. A
     * successful Map may return NULL on this AMI implementation; pass that
     * exact value to Unmap, and require Unmap success before freeing. */
    if(bytes!=TX_POOL_SIZE || p->device_address<0x100000000ULL ||
       (p->device_address&4095) || p->device_address>0x10000000000ULL-TX_POOL_SIZE)
        return EFI_UNSUPPORTED;
    st=te_identity(p,p->original_command);
    if(EFI_ERROR(st))return st;
    p->tx.pool=p->pool;p->tx.device_address=p->device_address;
    p->dma_step="complete";
    return EFI_SUCCESS;
}
static EFI_STATUS te_allocate(TuEfi *p) {
#ifdef TU102_AMD_DIRECT_PHYSICAL
    return da_allocate(p);
#else
    return te_allocate_options(p,EfiBootServicesData,0);
#endif
}

static EFI_STATUS te_enable_master(TuEfi *p) {
    if(!p || !p->owned || !p->profile_verified || p->fault || p->retained || p->allocation_probe ||
       !p->pool || (!p->mapped && !p->direct_physical) || p->master_pending || p->tx.mutated)
        return EFI_ACCESS_DENIED;
#ifdef TU102_AMD_DIRECT_PHYSICAL
    if(!p->direct_physical || !p->pages_owned)return EFI_ACCESS_DENIED;
    EFI_STATUS guarded=da_guard(p);if(EFI_ERROR(guarded))return guarded;
#endif
    EFI_STATUS st=te_identity(p,p->original_command);
    if(EFI_ERROR(st))return st;
    p->master_pending=TRUE;
    st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationEnable,
        EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,0);
    if(EFI_ERROR(st))return st;
    st=te_identity(p,p->original_command|4);
    if(EFI_ERROR(st))return st;
    p->master_enabled=TRUE;
    return EFI_SUCCESS;
}
static EFI_STATUS te_disable_master(TuEfi *p) {
    if(!p || !p->pci || !p->pci->Attributes)return EFI_INVALID_PARAMETER;
    if(!p->master_pending)return EFI_SUCCESS;
    EFI_STATUS st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationDisable,
        EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,0);
    EFI_STATUS checked=te_identity(p,p->original_command);
    if(EFI_ERROR(st) || EFI_ERROR(checked)) {
        p->retained=TRUE;return EFI_ERROR(st)?st:checked;
    }
    p->master_pending=FALSE;p->master_enabled=FALSE;
    return EFI_SUCCESS;
}

static EFI_STATUS te_release(TuEfi *p) {
    if(!p || p->released)return EFI_INVALID_PARAMETER;
    /* On uncertain post-write failure, retain all mappings and backing and
     * block handoff. Do not try an unvalidated reset/retry recovery sequence. */
    if(p->tx.mutated && !p->tx.quiescent) {
        p->retained=TRUE;
        (void)te_disable_master(p);
        return EFI_ACCESS_DENIED;
    }
    EFI_STATUS st=te_disable_master(p);
    if(EFI_ERROR(st) || p->retained)return EFI_ERROR(st)?st:EFI_ACCESS_DENIED;
    if(p->owned) {
        st=te_identity(p,p->original_command);
        if(EFI_ERROR(st)){p->retained=TRUE;return st;}
    }
    if(p->mapped) {
        st=p->pci->Unmap(p->pci,p->mapping);
        if(EFI_ERROR(st)){p->retained=TRUE;return st;}
        p->mapped=FALSE;p->mapping=0;
    }
    if(p->pool) {
        if(p->pages_owned)st=p->bs->FreePages((UINTN)p->pool,TX_POOL_SIZE/4096);
        else st=p->pci->FreeBuffer(p->pci,TX_POOL_SIZE/4096,p->pool);
        if(EFI_ERROR(st)){p->retained=TRUE;return st;}
        p->pool=0;p->tx.pool=0;p->pages_owned=FALSE;p->direct_physical=FALSE;
    }
    if(p->dac_changed) {
        st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationDisable,
            EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,0);
        if(EFI_ERROR(st)){p->retained=TRUE;return st;}
        p->dac_changed=FALSE;
    }
    if(p->attrs_saved) {
        UINT64 current=0;
        st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationGet,0,&current);
        if(EFI_ERROR(st) || current!=p->original_attrs) {
            p->retained=TRUE;return EFI_ERROR(st)?st:EFI_COMPROMISED_DATA;
        }
    }
    if(p->owned) {
        st=p->bs->CloseProtocol(p->handle,&p->pci_guid,p->image,0);
        if(EFI_ERROR(st)){p->retained=TRUE;return st;}
        p->owned=FALSE;
    }
    p->released=TRUE;
    return EFI_SUCCESS;
}

static int te_read(void *opaque,uint32_t off,uint32_t *out) {
    TuEfi *p=opaque;*out=0;
    if(p->fault || (!p->owned && !(p->read_only_probe && p->profile_verified)) ||
       (off&3) || off>0x1000000U-4)return 1;
    p->last_offset=off;
    p->last_status=p->pci->Mem.Read(p->pci,EfiPciIoWidthUint32,0,off,1,out);
    if(EFI_ERROR(p->last_status))p->fault=TRUE;
    return EFI_ERROR(p->last_status);
}
static int te_write(void *opaque,uint32_t off,uint32_t value) {
    TuEfi *p=opaque;
    if(p->fault || p->retained || p->read_only_probe || p->allocation_probe || !p->owned || !p->profile_verified ||
       (!p->mapped && !p->direct_physical) || !p->master_enabled || !tx_write_allowed(off))return 1;
    p->last_offset=off;
    p->last_status=p->pci->Mem.Write(p->pci,EfiPciIoWidthUint32,0,off,1,&value);
    if(EFI_ERROR(p->last_status))p->fault=TRUE;
    return EFI_ERROR(p->last_status);
}
static int te_delay(void *opaque,uint32_t usec) {
    TuEfi *p=opaque;
    p->last_status=p->bs->Stall(usec);return EFI_ERROR(p->last_status);
}
static int te_barrier(void *opaque) {
    TuEfi *p=opaque;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#ifdef TU102_AMD_DIRECT_PHYSICAL
    if(!p->direct_physical || !p->pages_owned || !p->owned || !p->master_enabled)return 1;
#ifdef TU102_AMD_DIRECT_HOST_TEST
#if __STDC_HOSTED__ != 1
#error TU102_AMD_DIRECT_HOST_TEST is forbidden in EFI builds
#endif
    extern void tu102_amd_cache_writeback_mock(void);
    tu102_amd_cache_writeback_mock();
#else
    /* Match the published physical-DMA wrapper's cache visibility barrier.
     * Only at firmware-start boundaries after checks, never in host tests. */
    __asm__ volatile("wbinvd" ::: "memory");
#endif
#endif
    if(!p->pci->Flush)return 1;
    p->last_status=p->pci->Flush(p->pci);return EFI_ERROR(p->last_status);
}
static int te_event(void *opaque,unsigned stage,uint32_t a,uint32_t b) {
    TuEfi *p=opaque;
    return !p->log_stage || p->log_stage(p->log_context,stage,a,b);
}
static void te_bind(TuEfi *p) {
    p->tx.opaque=p;p->tx.read=te_read;p->tx.write=te_write;
    p->tx.delay=te_delay;p->tx.barrier=te_barrier;p->tx.event=te_event;
}
#endif
