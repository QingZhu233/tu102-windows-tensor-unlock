/* Host-memory streaming-map precheck only. NOT common-buffer substitution,
 * NOT direct physical DMA, and NOT evidence of successful GPU execution. */
#ifndef TU102_HIGH_PAGES_PROBE_H
#define TU102_HIGH_PAGES_PROBE_H
typedef struct {
    EFI_STATUS status,allocation,map_status[2],unmap_status[2],free_status,restore_status;
    EFI_PHYSICAL_ADDRESS host,device[2];
    UINTN mapped_bytes[2];
    BOOLEAN token[2],high[2],uncertain,attempted;
    unsigned direction_count;
} TuHighReport;
static EFI_STATUS th_pick(EFI_BOOT_SERVICES *bs,EFI_PHYSICAL_ADDRESS *chosen) {
    if(!bs->GetMemoryMap || !bs->AllocatePool || !bs->FreePool)return EFI_UNSUPPORTED;
    UINTN bytes=0,key=0,stride=0;UINT32 version=0;
    EFI_STATUS st=bs->GetMemoryMap(&bytes,0,&key,&stride,&version);
    if(st!=EFI_BUFFER_TOO_SMALL || !bytes || bytes>MAP_LIMIT ||
       stride<sizeof(EFI_MEMORY_DESCRIPTOR) || stride>4096)return EFI_UNSUPPORTED;
    UINTN capacity=bytes+16*stride;VOID *map=0;
    st=bs->AllocatePool(EfiLoaderData,capacity,&map);
    if(EFI_ERROR(st) || !map)return EFI_OUT_OF_RESOURCES;
    bytes=capacity;st=bs->GetMemoryMap(&bytes,map,&key,&stride,&version);
    if(!EFI_ERROR(st) && (bytes>capacity || stride<sizeof(EFI_MEMORY_DESCRIPTOR) ||
       stride>4096 || bytes%stride))st=EFI_COMPROMISED_DATA;
    *chosen=0;
    if(!EFI_ERROR(st))for(UINTN off=0;off<bytes;off+=stride) {
        EFI_MEMORY_DESCRIPTOR *d=(VOID *)((UINT8 *)map+off);
        if(d->Type!=EfiConventionalMemory || (d->PhysicalStart&4095) ||
           d->NumberOfPages>(~(UINT64)0>>12))continue;
        UINT64 size=d->NumberOfPages<<12;
        if(d->PhysicalStart>~(UINT64)0-size)continue;
        UINT64 end=d->PhysicalStart+size;
        /* Match the published loader's initial4.25GiB and <8GiB fallback
         * region, rather than sampling the top of all RAM (~65.8GiB here).
         * This is address-selection research, not a Map failure bypass. */
        UINT64 start=d->PhysicalStart<0x110000000ULL?0x110000000ULL:d->PhysicalStart;
        if(end>0x200000000ULL)end=0x200000000ULL;
        if(start>end || TX_POOL_SIZE>end-start)continue;
        /* Take the lowest available verified free range in that window.
         * AllocateAddress must still succeed; no fixed-slot write or retry. */
        UINT64 address=start;
        if(address>=start && (!*chosen || address<*chosen))*chosen=address;
    }
    EFI_STATUS freed=bs->FreePool(map);
    if(EFI_ERROR(freed))return freed;
    if(EFI_ERROR(st))return st;
    return *chosen?EFI_SUCCESS:EFI_NOT_FOUND;
}
static TuHighReport th_run(TuEfi *p) {
    TuHighReport r={0};r.status=EFI_UNSUPPORTED;r.allocation=EFI_NOT_READY;
    r.map_status[0]=r.map_status[1]=EFI_NOT_READY;
    r.unmap_status[0]=r.unmap_status[1]=EFI_NOT_READY;r.free_status=EFI_NOT_READY;
    r.restore_status=EFI_NOT_READY;
    if(!p || !p->bs || !p->pci || !p->profile_verified || p->owned ||
       !p->bs->AllocatePages || !p->bs->FreePages || !p->pci->Attributes ||
       !p->pci->Map || !p->pci->Unmap)return r;
    UINT32 before[64],after[64];UINT64 attrs=0,supported=0;BOOLEAN dac_changed=FALSE;
    EFI_STATUS st=te_identity(p,3);
    if(!EFI_ERROR(st))st=p->pci->Pci.Read(p->pci,EfiPciIoWidthUint32,0,64,before);
    if(!EFI_ERROR(st))st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationSupported,0,&supported);
    if(!EFI_ERROR(st))st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationGet,0,&attrs);
    if(EFI_ERROR(st)){r.status=st;return r;}
    if(!(supported&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE) ||
       (attrs&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))return r;
    st=th_pick(p->bs,&r.host);if(EFI_ERROR(st)){r.status=st;return r;}
    EFI_PHYSICAL_ADDRESS requested=r.host;r.attempted=TRUE;
    st=p->bs->AllocatePages(AllocateAddress,EfiBootServicesData,TX_POOL_SIZE/4096,&r.host);
    r.allocation=st;
    if(EFI_ERROR(st)){r.status=st;return r;}
    if(r.host!=requested){r.status=EFI_COMPROMISED_DATA;r.uncertain=TRUE;return r;}
    /* This is allocated system RAM, not GPU VRAM. Zero only after firmware
     * allocation succeeds, before a streaming Read map can copy its contents. */
    zero_bytes((VOID *)(UINTN)r.host,TX_POOL_SIZE);
    if(!(attrs&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE)) {
        dac_changed=TRUE;st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationEnable,
            EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,0);
    }
    UINT64 active=0;
    if(!EFI_ERROR(st))st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationGet,0,&active);
    if(!EFI_ERROR(st) && (!(active&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE) ||
       (active&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)))st=EFI_SECURITY_VIOLATION;
    if(!EFI_ERROR(st))st=te_identity(p,3);
    if(!EFI_ERROR(st))for(unsigned i=0;i<2;i++) {
        VOID *token=0;UINTN bytes=TX_POOL_SIZE;
        EFI_PCI_IO_PROTOCOL_OPERATION op=i?EfiPciIoOperationBusMasterWrite:EfiPciIoOperationBusMasterRead;
        EFI_STATUS mapped=p->pci->Map(p->pci,op,(VOID *)(UINTN)r.host,&bytes,&r.device[i],&token);
        r.direction_count++;r.map_status[i]=mapped;r.mapped_bytes[i]=bytes;r.token[i]=token!=0;
        r.high[i]=!EFI_ERROR(mapped) && bytes==TX_POOL_SIZE && r.device[i]>=0x100000000ULL &&
            !(r.device[i]&4095) && r.device[i]<=0x10000000000ULL-TX_POOL_SIZE;
        if(!EFI_ERROR(mapped) || token) {
            r.unmap_status[i]=p->pci->Unmap(p->pci,token);
            if(EFI_ERROR(r.unmap_status[i])) {r.status=r.unmap_status[i];r.uncertain=TRUE;return r;}
        }
        if(EFI_ERROR(mapped)){st=mapped;break;}
    }
    r.free_status=p->bs->FreePages(r.host,TX_POOL_SIZE/4096);
    if(EFI_ERROR(r.free_status)){r.status=r.free_status;r.uncertain=TRUE;return r;}
    if(dac_changed)r.restore_status=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationDisable,EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,0);
    else r.restore_status=EFI_SUCCESS;
    UINT64 restored=0;EFI_STATUS a=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationGet,0,&restored);
    EFI_STATUS c=p->pci->Pci.Read(p->pci,EfiPciIoWidthUint32,0,64,after);
    BOOLEAN same=!EFI_ERROR(c);if(same)for(unsigned j=0;j<64;j++)if(before[j]!=after[j])same=FALSE;
    if(EFI_ERROR(r.restore_status) || EFI_ERROR(a) || restored!=attrs || !same) {
        r.status=EFI_COMPROMISED_DATA;r.uncertain=TRUE;return r;
    }
    r.status=st;return r;
}
#endif
