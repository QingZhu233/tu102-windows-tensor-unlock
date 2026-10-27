/* Survey official64-bit mappings only. No physical-DMA fallback or GPU I/O.
 * TianoCore's PCI_IO adapter calls RootBridge.Map(Read64) after DAC selection,
 * then the IOMMU's per-device SetAttribute. Here both layers are explicit;
 * each token is cleaned by the same protocol that created it. */
#ifndef TU102_DMA64_ROUTES_H
#define TU102_DMA64_ROUTES_H
#include <Protocol/PciRootBridgeIo.h>
#include "vendor/IoMmu.h"
#include "high_pages_probe.h"
#ifndef D64_PROGRESS
#define D64_PROGRESS(stage) (0)
#endif
static EFI_GUID D64RootGuid=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
static EFI_GUID D64IommuGuid=EDKII_IOMMU_PROTOCOL_GUID;
typedef struct {
    EFI_STATUS map,grant,revoke,unmap;
    EFI_PHYSICAL_ADDRESS device;
    UINTN bytes;
    BOOLEAN attempted,token,high;
} D64Observation;
typedef struct {
    EFI_STATUS status,root_status,iommu_status,allocate_status,free_status;
    EFI_PHYSICAL_ADDRESS host;
    D64Observation root[2],iommu[2];
    BOOLEAN uncertain,config_equal,root_map_called_with_IOMMU;
    EFI_PHYSICAL_ADDRESS root_map_entry;
    UINT8 root_map_code[96];
    BOOLEAN root_map_code_read;
} D64Report;
static BOOLEAN d64_read_code(EFI_BOOT_SERVICES *bs,VOID *entry,UINT8 output[96]) {
    UINTN count=0,key=0,stride=0,capacity=0;UINT32 version=0;
    EFI_STATUS st=bs->GetMemoryMap(&count,0,&key,&stride,&version);
    if(st!=EFI_BUFFER_TOO_SMALL || count>MAP_LIMIT || stride<sizeof(EFI_MEMORY_DESCRIPTOR) || stride>4096)return FALSE;
    VOID *map=0;capacity=count+16*stride;
    st=bs->AllocatePool(EfiLoaderData,capacity,&map);if(EFI_ERROR(st) || !map)return FALSE;
    count=capacity;st=bs->GetMemoryMap(&count,map,&key,&stride,&version);
    BOOLEAN found=FALSE;UINT64 start=(UINTN)entry;
    if(!EFI_ERROR(st) && count<=capacity && stride>=sizeof(EFI_MEMORY_DESCRIPTOR) && stride<=4096 && !(count%stride) && start && start<=~(UINT64)0-96) {
        for(UINTN i=0;i<count;i+=stride) {
            EFI_MEMORY_DESCRIPTOR *d=(VOID *)((UINT8 *)map+i);
            if(d->Type!=EfiBootServicesCode || (d->Attribute&EFI_MEMORY_RP) ||
               d->PhysicalStart>start || d->NumberOfPages>(~(UINT64)0>>12))continue;
            UINT64 bytes=d->NumberOfPages<<12,delta=start-d->PhysicalStart;
            if(delta<=bytes && 96<=bytes-delta){copy_bytes(output,entry,96);found=TRUE;break;}
        }
    }
    if(EFI_ERROR(bs->FreePool(map)))return FALSE;
    return found;
}
static int d64_guid(const EFI_GUID *a,const EFI_GUID *b) {
    const UINT8 *x=(const VOID *)a,*y=(const VOID *)b;
    for(UINTN i=0;i<sizeof(*a);i++)if(x[i]!=y[i])return 0;
    return 1;
}
static EFI_STATUS d64_root(TuEfi *p,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL **out) {
    if(!p->bs->LocateDevicePath || !p->bs->HandleProtocol)return EFI_UNSUPPORTED;
    EFI_DEVICE_PATH_PROTOCOL *path=0;
    EFI_STATUS st=p->bs->HandleProtocol(p->handle,&PathGuid,(VOID **)&path);
    if(EFI_ERROR(st) || !path)return EFI_NOT_FOUND;
    /* The exact2080Ti physical route was recorded by the PROM collector.
     * Never guess which root owns this target from a global root list. */
    static const UINT8 route[]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,
        1,1,6,0,3,1,1,1,6,0,0,0,0x7f,0xff,4,0};
    UINTN at=0;
    while(at<sizeof(route)) {
        const UINT8 *b=(const UINT8 *)path+at;
        UINTN size=b[2]+((UINTN)b[3]<<8),expected=route[at+2]+((UINTN)route[at+3]<<8);
        if(size!=expected)return EFI_SECURITY_VIOLATION;
        for(UINTN i=0;i<size;i++)if(b[i]!=route[at+i])return EFI_SECURITY_VIOLATION;
        at+=size;
    }
    EFI_DEVICE_PATH_PROTOCOL *rest=path;EFI_HANDLE parent=0;
    st=p->bs->LocateDevicePath(&D64RootGuid,&rest,&parent);
    if(EFI_ERROR(st))return st;
    if(!parent || parent==p->handle || rest!=(VOID *)((UINT8 *)path+12))return EFI_SECURITY_VIOLATION;
    st=p->bs->HandleProtocol(parent,&D64RootGuid,(VOID **)out);
    if(EFI_ERROR(st))return st;
    if(!*out || (*out)->SegmentNumber!=0 || !(*out)->Map || !(*out)->Unmap)return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
static void d64_init(D64Observation *o) {
    zero_bytes(o,sizeof(*o));o->map=o->grant=o->revoke=o->unmap=EFI_NOT_READY;
}
static int d64_valid(EFI_STATUS st,UINTN bytes,UINT64 address) {
    return !EFI_ERROR(st) && bytes==TX_POOL_SIZE && address>=0x100000000ULL &&
        !(address&4095) && address<=0x10000000000ULL-TX_POOL_SIZE;
}
static EFI_STATUS d64_map_root(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root,
                              EFI_PHYSICAL_ADDRESS host,unsigned direction,D64Observation *o) {
    VOID *token=0;o->attempted=TRUE;o->bytes=TX_POOL_SIZE;
    if(D64_PROGRESS(direction?"root-Write64-Map":"root-Read64-Map"))return EFI_DEVICE_ERROR;
    o->map=root->Map(root,direction?EfiPciOperationBusMasterWrite64:EfiPciOperationBusMasterRead64,
                    (VOID *)(UINTN)host,&o->bytes,&o->device,&token);
    o->token=token!=0;o->high=d64_valid(o->map,o->bytes,o->device);
    if(!EFI_ERROR(o->map) || token) {
        /* Cleanup is attempted even if the progress log fails. */
        int log_failed=D64_PROGRESS("root-Unmap");
        o->unmap=root->Unmap(root,token);
        if(EFI_ERROR(o->unmap))return o->unmap;
        if(log_failed)return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS; /* unsupported/low maps are observations, no fake success */
}
static EFI_STATUS d64_map_iommu(EDKII_IOMMU_PROTOCOL *iommu,EFI_HANDLE target,
                               EFI_PHYSICAL_ADDRESS host,unsigned direction,D64Observation *o) {
    VOID *token=0;o->attempted=TRUE;o->bytes=TX_POOL_SIZE;
    if(D64_PROGRESS(direction?"IOMMU-Write64-Map":"IOMMU-Read64-Map"))return EFI_DEVICE_ERROR;
    o->map=iommu->Map(iommu,direction?EdkiiIoMmuOperationBusMasterWrite64:EdkiiIoMmuOperationBusMasterRead64,
                       (VOID *)(UINTN)host,&o->bytes,&o->device,&token);
    o->token=token!=0;o->high=d64_valid(o->map,o->bytes,o->device);
    if(!EFI_ERROR(o->map) && o->high) {
        if(D64_PROGRESS("IOMMU-device-grant")) {
            /* No grant was made; only the mapping needs cleanup. */
            o->unmap=iommu->Unmap(iommu,token);
            return EFI_DEVICE_ERROR;
        }
        o->grant=iommu->SetAttribute(iommu,target,token,direction?EDKII_IOMMU_ACCESS_WRITE:EDKII_IOMMU_ACCESS_READ);
        /* Even a failed grant might have partially changed permissions.
         * Always revoke before Unmap and never free on a revoke error. */
        int log_failed=D64_PROGRESS("IOMMU-device-revoke");
        o->revoke=iommu->SetAttribute(iommu,target,token,0);
        if(EFI_ERROR(o->revoke))return o->revoke;
        if(EFI_ERROR(o->grant))o->high=FALSE;
        if(log_failed) {
            o->unmap=iommu->Unmap(iommu,token);
            return EFI_DEVICE_ERROR;
        }
    }
    if(!EFI_ERROR(o->map) || token) {
        int log_failed=D64_PROGRESS("IOMMU-Unmap");
        o->unmap=iommu->Unmap(iommu,token);
        if(EFI_ERROR(o->unmap))return o->unmap;
        if(log_failed)return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
}
static D64Report d64_survey(TuEfi *p) {
    D64Report r={0};r.status=r.root_status=r.iommu_status=r.allocate_status=r.free_status=EFI_NOT_READY;
    for(unsigned i=0;i<2;i++){d64_init(&r.root[i]);d64_init(&r.iommu[i]);}
    if(!p || !p->profile_verified || p->owned || !p->bs || !p->bs->LocateProtocol ||
       !p->bs->AllocatePages || !p->bs->FreePages || !p->pci || !p->pci->Attributes){r.status=EFI_UNSUPPORTED;return r;}
    UINT32 before[64],after[64];UINT64 attributes=0,after_attrs=0;
    EFI_STATUS st=te_identity(p,3);
    if(!EFI_ERROR(st))st=p->pci->Pci.Read(p->pci,EfiPciIoWidthUint32,0,64,before);
    if(!EFI_ERROR(st))st=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationGet,0,&attributes);
    if(EFI_ERROR(st) || (attributes&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)){r.status=EFI_ERROR(st)?st:EFI_ACCESS_DENIED;return r;}
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root=0;EDKII_IOMMU_PROTOCOL *iommu=0;
    if(D64_PROGRESS("select-exact-parent-root")){r.status=EFI_DEVICE_ERROR;return r;}
    r.root_status=d64_root(p,&root);
    if(!EFI_ERROR(r.root_status)) {
        r.root_map_entry=(UINTN)root->Map;
        r.root_map_code_read=d64_read_code(p->bs,(VOID *)(UINTN)root->Map,r.root_map_code);
    }
    r.iommu_status=p->bs->LocateProtocol(&D64IommuGuid,0,(VOID **)&iommu);
    if(!EFI_ERROR(r.iommu_status)) {
        if(!iommu || iommu->Revision<EDKII_IOMMU_PROTOCOL_REVISION ||
           !iommu->Map || !iommu->Unmap || !iommu->SetAttribute){r.status=EFI_COMPROMISED_DATA;return r;}
    } else if(r.iommu_status!=EFI_NOT_FOUND){r.status=r.iommu_status;return r;}
    if(!iommu && EFI_ERROR(r.root_status)){r.status=r.root_status;return r;}
    if(D64_PROGRESS("select-high-RAM")){r.status=EFI_DEVICE_ERROR;return r;}
    st=th_pick(p->bs,&r.host);if(EFI_ERROR(st)){r.status=st;return r;}
    EFI_PHYSICAL_ADDRESS wanted=r.host;
    if(D64_PROGRESS("AllocateAddress32MiB")){r.status=EFI_DEVICE_ERROR;return r;}
    r.allocate_status=p->bs->AllocatePages(AllocateAddress,EfiBootServicesData,TX_POOL_SIZE/4096,&r.host);
    if(EFI_ERROR(r.allocate_status)){r.status=r.allocate_status;return r;}
    if(wanted!=r.host){r.status=EFI_COMPROMISED_DATA;r.uncertain=TRUE;return r;}
    zero_bytes((VOID *)(UINTN)r.host,TX_POOL_SIZE);
    /* No DMA is started. The direction-specific protocol is tested and
     * immediately cleaned; no CPU writes during a live Read/Write mapping. */
    for(unsigned i=0;i<2;i++) {
        st=te_identity(p,3);if(EFI_ERROR(st))break;
        if(iommu)st=d64_map_iommu(iommu,p->handle,r.host,i,&r.iommu[i]);
        else st=d64_map_root(root,r.host,i,&r.root[i]);
        if(EFI_ERROR(st)){r.status=st;r.uncertain=TRUE;return r;}
    }
    int free_log_failed=D64_PROGRESS("FreePages");
    r.free_status=p->bs->FreePages(r.host,TX_POOL_SIZE/4096);
    if(EFI_ERROR(r.free_status)){r.status=r.free_status;r.uncertain=TRUE;return r;}
    EFI_STATUS read=p->pci->Pci.Read(p->pci,EfiPciIoWidthUint32,0,64,after);
    EFI_STATUS attrs=p->pci->Attributes(p->pci,EfiPciIoAttributeOperationGet,0,&after_attrs);
    r.config_equal=!EFI_ERROR(read) && !EFI_ERROR(attrs) && after_attrs==attributes;
    if(r.config_equal)for(unsigned i=0;i<64;i++)if(before[i]!=after[i])r.config_equal=FALSE;
    if(!r.config_equal){r.status=EFI_COMPROMISED_DATA;r.uncertain=TRUE;return r;}
    r.status=free_log_failed?EFI_DEVICE_ERROR:st;return r;
}
#endif
