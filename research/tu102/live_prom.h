/* SPDX-License-Identifier: GPL-2.0-only
 * Explicit stage-2 data acquisition for the local profile. No write/reset/DMA
 * callbacks are used. Does not authorize or implement any unlock transaction.
 */
#include <IndustryStandard/Acpi10.h>

#define LIVE_PROM_SIZE 0x80000U
#define LIVE_PROM_BASE 0x300000U
#define LIVE_PROM_CHUNK 0x1000U
#define LIVE_BAR0 0xdd000000ULL
#define LIVE_MIN_BAR_BYTES 0x840104ULL

static EFI_STATUS live_identity(EFI_PCI_IO_PROTOCOL *pci) {
    UINTN seg=0,bus=0,dev=0,fn=0;
    UINT32 id=0,sub=0,cmd=0,bar=0;
    if(!pci || !pci->GetLocation || !pci->Pci.Read || !pci->Mem.Read ||
       !pci->GetBarAttributes)return EFI_UNSUPPORTED;
    EFI_STATUS st=pci->GetLocation(pci,&seg,&bus,&dev,&fn);
    if(EFI_ERROR(st))return st;
    if(seg!=TU102_SEGMENT || bus!=TU102_BUS || dev!=TU102_DEVICE || fn!=TU102_FUNCTION)
        return EFI_SECURITY_VIOLATION;
    st=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,1,&id);
    if(!EFI_ERROR(st))st=pci->Pci.Read(pci,EfiPciIoWidthUint32,0x2c,1,&sub);
    if(!EFI_ERROR(st))st=pci->Pci.Read(pci,EfiPciIoWidthUint32,4,1,&cmd);
    if(!EFI_ERROR(st))st=pci->Pci.Read(pci,EfiPciIoWidthUint32,0x10,1,&bar);
    if(EFI_ERROR(st))return st;
    if(id!=TU102_VENDOR_DEVICE || sub!=TU102_SUBSYSTEM || (cmd&0xffff)!=3 || bar!=LIVE_BAR0)
        return EFI_SECURITY_VIOLATION;
    return EFI_SUCCESS;
}

static EFI_STATUS live_bounds(EFI_BOOT_SERVICES *bs,EFI_PCI_IO_PROTOCOL *pci) {
    VOID *resources=0;
    EFI_STATUS st=pci->GetBarAttributes(pci,0,0,&resources);
    if(EFI_ERROR(st)){if(resources)bs->FreePool(resources);return st;}
    if(!resources)return EFI_COMPROMISED_DATA;
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=resources;
    int valid=d->Desc==ACPI_ADDRESS_SPACE_DESCRIPTOR && d->Len==sizeof(*d)-3 &&
        d->ResType==ACPI_ADDRESS_SPACE_TYPE_MEM && d->AddrRangeMin==LIVE_BAR0 &&
        d->AddrTranslationOffset==0 && d->AddrLen>=LIVE_MIN_BAR_BYTES &&
        d->AddrLen<=0x1000000ULL && d->AddrRangeMax>=LIVE_BAR0 &&
        d->AddrRangeMax-LIVE_BAR0==d->AddrLen-1;
    if(valid) {
        const EFI_ACPI_END_TAG_DESCRIPTOR *end=(const VOID *)(d+1);
        valid=end->Desc==ACPI_END_TAG_DESCRIPTOR;
    }
    field("live_bar_resource_base",d->AddrRangeMin);
    field("live_bar_resource_bytes",d->AddrLen);
    EFI_STATUS freed=bs->FreePool(resources);
    if(EFI_ERROR(freed))return freed;
    return valid?EFI_SUCCESS:EFI_UNSUPPORTED;
}

static EFI_STATUS live_flush(EFI_FILE_PROTOCOL *trace,UINTN *written) {
    if(LogFault || *written>Used)return EFI_VOLUME_FULL;
    UINTN bytes=Used-*written,actual=bytes;
    EFI_STATUS st=EFI_SUCCESS;
    if(bytes)st=trace->Write(trace,&actual,Report+*written);
    if(EFI_ERROR(st))return st;
    if(actual!=bytes)return EFI_DEVICE_ERROR;
    *written=Used;
    return trace->Flush(trace);
}

static EFI_STATUS live_read32(EFI_PCI_IO_PROTOCOL *pci,UINT32 off,UINT32 *out) {
    *out=0;
    EFI_STATUS st=pci->Mem.Read(pci,EfiPciIoWidthUint32,0,off,1,out);
    text("live_reg_");hex(off,8);text("=");hex(*out,8);text("\r\n");
    return st;
}

static EFI_STATUS capture_live_prom(EFI_BOOT_SERVICES *bs,EFI_FILE_PROTOCOL *dir,
                                    EFI_PCI_IO_PROTOCOL *pci) {
    EFI_STATUS st=live_identity(pci);
    if(EFI_ERROR(st))return st;
    st=live_bounds(bs,pci);
    if(EFI_ERROR(st))return st;
    /* Durable intent log must exist before the first GPU MMIO read. */
    EFI_FILE_PROTOCOL *trace=0,*old=0;
    st=dir->Open(dir,&old,L"LIVE-TRACE.TXT",EFI_FILE_MODE_READ,0);
    if(!EFI_ERROR(st)){if(old)old->Close(old);return EFI_ACCESS_DENIED;}
    if(st!=EFI_NOT_FOUND)return st;
    st=dir->Open(dir,&trace,L"LIVE-TRACE.TXT",
        EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    if(EFI_ERROR(st) || !trace)return EFI_ERROR(st)?st:EFI_DEVICE_ERROR;
    UINTN written=0;
    UINT8 *first=0,*second=0;
    UINT32 value=0;
    text("live_stage=begin; BAR0_read_only=true; unlock_attempted=false\r\n");
    st=live_flush(trace,&written);
    if(EFI_ERROR(st))goto done;
    st=live_read32(pci,0,&value);
    if(EFI_ERROR(st))goto done;
    if(value!=0x162000a1U){st=EFI_UNSUPPORTED;goto done;}
    /* Principal TU102 diagnostic registers from the existing TU102 projects.
     * No range scans, protected reset registers, or indirect index writes. */
    static const UINT32 regs[]={0x100ce0,0x625f04,0x1fa824,0x1fa828,
        0x409650,0x409660,0x409664,0x409668,0x40966c,
        0x110100,0x840100,0x118234};
    for(UINTN i=0;i<sizeof(regs)/sizeof(regs[0]);i++) {
        st=live_read32(pci,regs[i],&value);
        if(EFI_ERROR(st))goto done;
        if(regs[i]==0x100ce0) {
            UINT64 fb=(UINT64)((value>>4)&63)<<((value&15)+20);
            if(value&(1U<<30))fb=fb/16*15;
            field("live_capacity_bytes",fb);
            if(fb!=(22ULL<<30)){st=EFI_UNSUPPORTED;goto done;}
        }
    }
    st=live_read32(pci,LIVE_PROM_BASE,&value);
    if(EFI_ERROR(st))goto done;
    if((value&0xffff)!=0xaa55){st=EFI_COMPROMISED_DATA;goto done;}
    st=bs->AllocatePool(EfiLoaderData,LIVE_PROM_SIZE,(VOID **)&first);
    if(EFI_ERROR(st) || !first){st=EFI_OUT_OF_RESOURCES;goto done;}
    st=bs->AllocatePool(EfiLoaderData,LIVE_PROM_SIZE,(VOID **)&second);
    if(EFI_ERROR(st) || !second){st=EFI_OUT_OF_RESOURCES;goto done;}
    for(UINTN i=0;i<LIVE_PROM_SIZE;i++){first[i]=0xa5;second[i]=0x5a;}
    for(UINTN pass=0;pass<2;pass++) {
        UINT8 *dst=pass?second:first;
        st=live_identity(pci);if(EFI_ERROR(st))goto done;
        field("live_prom_pass",pass);
        for(UINTN off=0;off<LIVE_PROM_SIZE;off+=LIVE_PROM_CHUNK) {
            if(!(off&0xffff)) {
                field("live_prom_next_offset",LIVE_PROM_BASE+off);
                st=live_flush(trace,&written);if(EFI_ERROR(st))goto done;
            }
            st=pci->Mem.Read(pci,EfiPciIoWidthUint32,0,LIVE_PROM_BASE+off,
                LIVE_PROM_CHUNK/4,dst+off);
            if(EFI_ERROR(st)){field("live_prom_failed_offset",LIVE_PROM_BASE+off);goto done;}
        }
    }
    if(first[0]!=0x55 || first[1]!=0xaa){st=EFI_COMPROMISED_DATA;goto done;}
    for(UINTN i=0;i<LIVE_PROM_SIZE;i++) {
        if(first[i]!=second[i]) {
            field("live_prom_difference_offset",i);st=EFI_COMPROMISED_DATA;goto done;
        }
    }
    st=live_identity(pci);if(EFI_ERROR(st))goto done;
    text("live_prom_identical_two_passes=true\r\n");
    field("live_prom_bytes",LIVE_PROM_SIZE);
    st=live_flush(trace,&written);if(EFI_ERROR(st))goto done;
    st=save_new(dir,L"LIVE-PROM.BIN",first,LIVE_PROM_SIZE);
done:
    field("live_capture_status",st);
    EFI_STATUS flushed=live_flush(trace,&written);
    EFI_STATUS closed=trace->Close(trace);
    if(!EFI_ERROR(st) && EFI_ERROR(flushed))st=flushed;
    if(!EFI_ERROR(st) && EFI_ERROR(closed))st=closed;
    if(second){EFI_STATUS f=bs->FreePool(second);if(!EFI_ERROR(st) && EFI_ERROR(f))st=f;}
    if(first){EFI_STATUS f=bs->FreePool(first);if(!EFI_ERROR(st) && EFI_ERROR(f))st=f;}
    return st;
}
