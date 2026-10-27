/* Local conditional release, informed by capture11's nvgop-tu1xx owner.
 * No broad DisconnectController(handle,NULL,NULL), resets, retries or raw PCI.
 * Candidate checks are read-only. The ONLY release is one specific driver on
 * the exact2080Ti target after U, native guards and Windows preload.
 */
#ifndef TU102_OWNER_RELEASE_H
#define TU102_OWNER_RELEASE_H
#include <Protocol/DriverBinding.h>
static EFI_GUID TrBindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID;
typedef struct {
    EFI_HANDLE agent,image,source;
    VOID *base;
    UINT64 bytes;
    UINT32 version;
    EFI_DRIVER_BINDING_STOP stop;
    UINT8 digest[32];
    UINT32 config[64];
} TuReleaseOwner;
static int tr_equal_bytes(const VOID *a,const VOID *b,UINTN n) {
    const UINT8 *x=a,*y=b;
    for(UINTN i=0;i<n;i++)if(x[i]!=y[i])return 0;
    return 1;
}
static int tr_console_handle(EFI_SYSTEM_TABLE *s,EFI_HANDLE h) {
    return h && (h==s->ConsoleOutHandle || h==s->ConsoleInHandle || h==s->StandardErrorHandle);
}
static int tr_driver_filename(EFI_DEVICE_PATH_PROTOCOL *p) {
    static const CHAR16 name[]=L"nvgop-tu1xx";
    if(!p || p->Type!=4 || p->SubType!=4)return 0;
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n!=4+sizeof(name) || !tr_equal_bytes((UINT8 *)p+4,name,sizeof(name)))return 0;
    EFI_DEVICE_PATH_PROTOCOL *end=(VOID *)((UINT8 *)p+n);
    return end->Type==0x7f && end->SubType==0xff && end->Length[0]==4 && !end->Length[1];
}
static EFI_STATUS tr_candidate(TuEfi *p,EFI_SYSTEM_TABLE *s,TuReleaseOwner *out,BOOLEAN log) {
    if(!p || !s || !out || !p->profile_verified || p->owned || p->fault ||
       p->tx.mutated || !p->bs->DisconnectController || !p->bs->ProtocolsPerHandle ||
       !p->bs->OpenProtocolInformation || !p->bs->RaiseTPL || !p->bs->RestoreTPL)
        return EFI_ACCESS_DENIED;
    zero_bytes(out,sizeof(*out));
    EFI_STATUS st=te_identity(p,3);
    if(EFI_ERROR(st))return st;
    EFI_GUID **protocols=0;UINTN n=0,drivers=0;
    EFI_TPL old=p->bs->RaiseTPL(TPL_NOTIFY);
    st=p->bs->ProtocolsPerHandle(p->handle,&protocols,&n);
    if(!EFI_ERROR(st) && (!protocols || !n || n>32))st=EFI_UNSUPPORTED;
    for(UINTN i=0;!EFI_ERROR(st) && i<n;i++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *r=0;UINTN count=0;
        if(!protocols[i]){st=EFI_COMPROMISED_DATA;break;}
        st=p->bs->OpenProtocolInformation(p->handle,protocols[i],&r,&count);
        if(!EFI_ERROR(st) && (count>32 || (count && !r)))st=EFI_UNSUPPORTED;
        for(UINTN j=0;!EFI_ERROR(st) && j<count;j++) {
            UINT32 a=r[j].Attributes;
            if(log){field("release_protocol_index",i);field("release_protocol_guid_data1",protocols[i]->Data1);
                field("release_owner_flags",a);field("release_owner_count",r[j].OpenCount);}
            if(a==EFI_OPEN_PROTOCOL_GET_PROTOCOL || a==EFI_OPEN_PROTOCOL_TEST_PROTOCOL ||
               a==EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL)continue;
            /* Exactly one active BY_DRIVER record, on PCI_IO only. Any child
             * on ANY protocol, second owner, exclusive or unknown open refuses. */
            if(a!=EFI_OPEN_PROTOCOL_BY_DRIVER || !tr_equal_bytes(protocols[i],&p->pci_guid,sizeof(EFI_GUID)) ||
               r[j].OpenCount!=1 || r[j].ControllerHandle!=p->handle || !r[j].AgentHandle ||
               r[j].AgentHandle==p->image || r[j].AgentHandle==p->handle ||
               tr_console_handle(s,r[j].AgentHandle) || ++drivers!=1)st=EFI_ACCESS_DENIED;
            else out->agent=r[j].AgentHandle;
        }
        if(r){EFI_STATUS freed=p->bs->FreePool(r);if(!EFI_ERROR(st) && EFI_ERROR(freed))st=freed;}
    }
    if(protocols){EFI_STATUS freed=p->bs->FreePool(protocols);if(!EFI_ERROR(st) && EFI_ERROR(freed))st=freed;}
    p->bs->RestoreTPL(old);
    if(EFI_ERROR(st))return st;
    if(drivers!=1)return EFI_ACCESS_DENIED;
    EFI_DRIVER_BINDING_PROTOCOL *binding=0;
    st=p->bs->HandleProtocol(out->agent,&TrBindingGuid,(VOID **)&binding);
    if(EFI_ERROR(st) || !binding || binding->DriverBindingHandle!=out->agent ||
       binding->Version!=0x50005 || !binding->Stop || !binding->ImageHandle ||
       binding->ImageHandle==p->image)return EFI_UNSUPPORTED;
    EFI_LOADED_IMAGE_PROTOCOL *image=0;
    st=p->bs->HandleProtocol(binding->ImageHandle,&ImageGuid,(VOID **)&image);
    if(EFI_ERROR(st) || !image || image->DeviceHandle!=p->handle ||
       image->ImageSize!=0x20960 || image->ImageCodeType!=EfiBootServicesCode ||
       !tr_driver_filename(image->FilePath) ||
       !rom_in_host_ram(p->bs,image->ImageBase,image->ImageSize))return EFI_UNSUPPORTED;
    out->image=binding->ImageHandle;out->source=image->DeviceHandle;
    out->base=image->ImageBase;out->bytes=image->ImageSize;out->version=binding->Version;
    out->stop=binding->Stop;GuSha256(out->base,(UINTN)out->bytes,out->digest);
    st=p->pci->Pci.Read(p->pci,EfiPciIoWidthUint32,0,64,out->config);
    if(EFI_ERROR(st))return st;
    if(log){text("release_candidate=nvgop-tu1xx_target2080Ti_no_children\r\n");
        field("release_candidate_image_bytes",out->bytes);field("release_candidate_version",out->version);}
    return LogFault?EFI_VOLUME_FULL:EFI_SUCCESS;
}
static int tr_same(const TuReleaseOwner *a,const TuReleaseOwner *b) {
    return a->agent==b->agent && a->image==b->image && a->source==b->source &&
        a->base==b->base && a->bytes==b->bytes && a->version==b->version &&
        a->stop==b->stop && tr_equal_bytes(a->digest,b->digest,32) &&
        tr_equal_bytes(a->config,b->config,sizeof(a->config));
}
static EFI_STATUS tr_decode_delta(TuEfi *p,const TuReleaseOwner *saved,UINT32 current[64],UINT16 *missing) {
    EFI_STATUS st=p->pci->Pci.Read(p->pci,EfiPciIoWidthUint32,0,64,current);
    if(EFI_ERROR(st))return st;
    *missing=(UINT16)(saved->config[1]&~current[1]&3U);
    field("release_saved_command",saved->config[1]&0xffff);
    field("release_current_command",current[1]&0xffff);
    field("release_missing_decode_bits",*missing);
    for(UINTN i=0;i<64;i++) {
        UINT32 normalized=current[i];if(i==1)normalized|=*missing;
        if(normalized!=saved->config[i]) {
            field("release_changed_config_offset",i*4);
            field("release_config_saved",saved->config[i]);field("release_config_actual",current[i]);
            return EFI_SECURITY_VIOLATION;
        }
    }
    if((saved->config[1]&0xffff)!=3 || (current[1]&0xfffc))return EFI_SECURITY_VIOLATION;
    return te_identity(p,(UINT16)current[1]);
}
static EFI_STATUS tr_restore_decode(TuEfi *p,const TuReleaseOwner *saved) {
    if(!p->owned || !p->profile_verified || p->fault || p->pool || p->mapped ||
       p->master_pending || p->tx.mutated || !p->pci->Attributes)return EFI_ACCESS_DENIED;
    UINT32 current[64];UINT16 missing=0;
    EFI_STATUS st=tr_decode_delta(p,saved,current,&missing);
    if(EFI_ERROR(st) || !missing)return st;
    UINT64 mask=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
    UINT64 supported=0,attrs=0;
    st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationSupported,0,&supported);
    if(!EFI_ERROR(st))st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationGet,0,&attrs);
    if(EFI_ERROR(st))return st;
    if((supported&mask)!=mask || (attrs&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))return EFI_UNSUPPORTED;
    /* Restore only originally enabled IO/MEM decode via the standard PCI_IO
     * API. Never use EFI_PCI_DEVICE_ENABLE: it would enable bus mastering. */
    field("decode_restore_attributes",mask);
    if(p->log_stage && p->log_stage(p->log_context,0x100,missing,(UINT32)mask))return EFI_DEVICE_ERROR;
    EFI_STATUS enabled=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationEnable,mask,0);
    field("decode_restore_API_status",enabled);
    st=p->pci->Pci.Read(p->pci,EfiPciIoWidthUint32,0,64,current);
    if(EFI_ERROR(enabled))return enabled; /* a partial effect never makes an API error success */
    if(EFI_ERROR(st))return st;
    for(UINTN i=0;i<64;i++)if(current[i]!=saved->config[i]) {
        field("decode_readback_mismatch_offset",i*4);return EFI_COMPROMISED_DATA;
    }
    field("decode_restore_full_config_matched",1);
    return te_identity(p,3);
}
static EFI_STATUS tr_disconnect(TuEfi *p,EFI_SYSTEM_TABLE *s,const TuReleaseOwner *expected,
                                BOOLEAN *attempted) {
    if(!attempted || *attempted || !expected)return EFI_ACCESS_DENIED;
    TuReleaseOwner now={0};EFI_STATUS st=tr_candidate(p,s,&now,FALSE);
    if(EFI_ERROR(st))return st;
    if(!tr_same(expected,&now))return EFI_ABORTED;
    /* No logging/file IO between the final candidate check and this one call.
     * DisconnectController invokes driver Stop and may change GPU state; a
     * failure from here requires full power-off, even without our MMIO writes. */
    *attempted=TRUE;
    st=p->bs->DisconnectController(p->handle,now.agent,0);
    field("driver_disconnect_status",st);
    if(EFI_ERROR(st))return st;
    EFI_PCI_IO_PROTOCOL *fresh=0;
    st=p->bs->HandleProtocol(p->handle,&p->pci_guid,(VOID **)&fresh);
    field("driver_release_PCI_refresh_status",st);
    if(EFI_ERROR(st) || !fresh || fresh!=p->pci)return EFI_COMPROMISED_DATA;
    UINT32 config[64];UINT16 missing=0;
    st=tr_decode_delta(p,expected,config,&missing);
    if(!EFI_ERROR(st))p->release_command=(UINT16)config[1];
    field("driver_release_identity_status",st);
    return st;
}
#endif
