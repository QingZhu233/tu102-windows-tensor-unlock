/* Local B850/Zen5 experimental DIRECT PHYSICAL DMA profile.
 * This intentionally DOES NOT use UEFI Map and is NOT a formal mapped DMA.
 * Derived from the published Turing high-physical approach, with local
 * capture19 root-code and exact IVRS restrictions. No IOMMU disable/writes.
 * Runtime prerequisites must pass before U/driver release and again before
 * GPU execution. A pass reduces specific uncertainty, not all hardware risk.
 */
#ifndef TU102_AMD_DIRECT_H
#define TU102_AMD_DIRECT_H
#include <Guid/Acpi.h>
#include <Protocol/CpuIo2.h>
#include <cpuid.h>
#include "dma64_routes.h"
static EFI_SYSTEM_TABLE *DaSystem;
static EFI_GUID DaAcpiGuid=EFI_ACPI_20_TABLE_GUID;
static EFI_GUID DaCpuIoGuid=EFI_CPU_IO2_PROTOCOL_GUID;
static EFI_STATUS da_host_read(EFI_CPU_IO2_PROTOCOL *cpu,UINT64 address,UINT32 *out) {
    /* Host IOMMU registers are NOT PCI root-relative device addresses.
     * No raw-dereference fallback; only this exact validated host-MMIO set. */
    if(!cpu || !cpu->Mem.Read || !out || (address!=0xf7600018ULL && address!=0xf760001cULL &&
       address!=0xf7600030ULL && address!=0xf7600034ULL))return EFI_ACCESS_DENIED;
    *out=0;EFI_STATUS st=cpu->Mem.Read(cpu,EfiCpuIoWidthUint32,address,1,out);
    field("direct_host_MMIO_read_address",address);field("direct_host_MMIO_read_status",st);
    return st;
}
static UINT32 da32(const VOID *p){const UINT8 *b=p;return b[0]|((UINT32)b[1]<<8)|((UINT32)b[2]<<16)|((UINT32)b[3]<<24);}
static UINT64 da64(const VOID *p){const UINT8 *b=p;return da32(b)|((UINT64)da32(b+4)<<32);}
static int da_sum(const UINT8 *p,UINTN bytes){UINT8 sum=0;for(UINTN i=0;i<bytes;i++)sum+=p[i];return sum==0;}
static int da_readable(TuEfi *p,const VOID *address,UINTN bytes,int kind) {
    UINT64 start=(UINTN)address;if(!start || !bytes || start>~(UINT64)0-bytes)return 0;
    UINTN needed=0,key=0,stride=0;UINT32 version=0;
    EFI_STATUS st=p->bs->GetMemoryMap(&needed,0,&key,&stride,&version);
    if(st!=EFI_BUFFER_TOO_SMALL || needed>MAP_LIMIT || stride<sizeof(EFI_MEMORY_DESCRIPTOR) || stride>4096)return 0;
    UINTN capacity=needed+16*stride;VOID *map=0;
    st=p->bs->AllocatePool(EfiLoaderData,capacity,&map);if(EFI_ERROR(st) || !map)return 0;
    needed=capacity;st=p->bs->GetMemoryMap(&needed,map,&key,&stride,&version);int valid=0;
    if(!EFI_ERROR(st) && needed<=capacity && stride>=sizeof(EFI_MEMORY_DESCRIPTOR) && stride<=4096 && !(needed%stride) && version==1) {
        for(UINTN off=0;off<needed;off+=stride) {
            EFI_MEMORY_DESCRIPTOR *d=(VOID *)((UINT8 *)map+off);
            if(d->PhysicalStart>start || d->NumberOfPages>(~(UINT64)0>>12) || (d->Attribute&EFI_MEMORY_RP))continue;
            UINT64 size=d->NumberOfPages<<12,delta=start-d->PhysicalStart;
            if(delta>size || bytes>size-delta)continue;
            if(kind==2)valid=d->Type==EfiMemoryMappedIO || d->Type==EfiReservedMemoryType;
            else if(kind==1)valid=d->Type==EfiBootServicesData && (d->Attribute&EFI_MEMORY_WB) &&
                !(d->Attribute&(EFI_MEMORY_WP|EFI_MEMORY_RP|0x20000ULL));
            else valid=d->Type==EfiACPIReclaimMemory || d->Type==EfiACPIMemoryNVS ||
                d->Type==EfiBootServicesData || d->Type==EfiBootServicesCode ||
                d->Type==EfiRuntimeServicesData || d->Type==EfiRuntimeServicesCode;
            if(valid)break;
        }
    }
    if(EFI_ERROR(p->bs->FreePool(map)))return 0;
    return valid;
}
static EFI_STATUS da_cpu(void) {
    UINT32 vendor_b=0,vendor_c=0,vendor_d=0,signature=0,flags=0,bits=0,enc=0,encb=0;
    UINT64 syscfg=~(UINT64)0;
#ifdef TU102_AMD_DIRECT_HOST_TEST
#if __STDC_HOSTED__ != 1
#error TU102_AMD_DIRECT_HOST_TEST is forbidden in EFI builds
#endif
    extern void tu102_amd_cpu_snapshot(UINT32 *,UINT32 *,UINT32 *,UINT32 *,UINT32 *,UINT32 *,UINT32 *,UINT32 *,UINT64 *);
    tu102_amd_cpu_snapshot(&vendor_b,&vendor_c,&vendor_d,&signature,&flags,&bits,&enc,&encb,&syscfg);
#else
    UINT32 a=0,b=0,c=0,d=0;
    if(!__get_cpuid(0,&a,&vendor_b,&vendor_c,&vendor_d) ||
       !__get_cpuid(1,&signature,&b,&flags,&d) ||
       !__get_cpuid(0x80000008,&bits,&b,&c,&d) ||
       !__get_cpuid(0x8000001f,&enc,&encb,&c,&d))return EFI_UNSUPPORTED;
    /* Read this documented AMD MSR only AFTER matching the observed CPU.
     * Memory encryption must be disabled for this physical-identity profile. */
    if(vendor_b!=0x68747541 || vendor_d!=0x69746e65 || vendor_c!=0x444d4163 ||
       signature!=0x00b40f40 || (flags&0x80000000U) || !(enc&1))return EFI_UNSUPPORTED;
    UINT32 lo=0,hi=0;
    __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0010010U));
    syscfg=((UINT64)hi<<32)|lo;
#endif
    field("direct_CPU_signature",signature);field("direct_CPU_physical_bits",bits&255);
    field("direct_CPU_hypervisor",flags>>31);field("direct_CPU_SYSCFG",syscfg);
    if(vendor_b!=0x68747541 || vendor_d!=0x69746e65 || vendor_c!=0x444d4163 ||
       signature!=0x00b40f40 || (flags&0x80000000U) || (bits&255)<40 || (bits&255)>52 ||
       !(enc&1) || (encb&63)!=51 || syscfg==~(UINT64)0 ||
       (syscfg&((1ULL<<23)|(1ULL<<24)|(1ULL<<25))))return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
static EFI_STATUS da_ivrs(TuEfi *p,const UINT8 **ivrs) {
    if(!DaSystem || DaSystem->NumberOfTableEntries>128 || !DaSystem->ConfigurationTable)return EFI_UNSUPPORTED;
    const UINT8 *rsdp=0;
    for(UINTN i=0;i<DaSystem->NumberOfTableEntries;i++) {
        EFI_CONFIGURATION_TABLE *e=&DaSystem->ConfigurationTable[i];
        if(d64_guid(&e->VendorGuid,&DaAcpiGuid)){if(rsdp)return EFI_COMPROMISED_DATA;rsdp=e->VendorTable;}
    }
    if(!da_readable(p,rsdp,36,0) || !tr_equal_bytes(rsdp,"RSD PTR ",8) ||
       rsdp[15]<2 || !da_sum(rsdp,20))return EFI_COMPROMISED_DATA;
    UINT32 length=da32(rsdp+20);
    if(length<36 || length>4096 || !da_readable(p,rsdp,length,0) || !da_sum(rsdp,length))return EFI_COMPROMISED_DATA;
    const UINT8 *xsdt=(VOID *)(UINTN)da64(rsdp+24);
    if(!da_readable(p,xsdt,36,0) || !tr_equal_bytes(xsdt,"XSDT",4))return EFI_COMPROMISED_DATA;
    length=da32(xsdt+4);
    if(length<36 || length>65536 || (length-36)%8 || !da_readable(p,xsdt,length,0) || !da_sum(xsdt,length))return EFI_COMPROMISED_DATA;
    *ivrs=0;
    for(UINTN off=36;off<length;off+=8) {
        const UINT8 *table=(VOID *)(UINTN)da64(xsdt+off);
        if(!da_readable(p,table,36,0))return EFI_COMPROMISED_DATA;
        if(tr_equal_bytes(table,"IVRS",4)){if(*ivrs)return EFI_COMPROMISED_DATA;*ivrs=table;}
    }
    if(!*ivrs || da32(*ivrs+4)!=200 || !da_readable(p,*ivrs,200,0) || !da_sum(*ivrs,200) ||
       !GuHashMatches(*ivrs,200,"a977843575fcb5a18b44d25c2d068c4e5d6c55f8683ac2b7b91642bc604b6652"))return EFI_SECURITY_VIOLATION;
    /* Exact capture is two IVHD descriptions of the SAME unit, not two
     * independently addressed controllers. Reject any new layout/MMIO base. */
    const UINTN blocks[]={48,116};const UINT8 types[]={0x10,0x11};
    for(unsigned i=0;i<2;i++) {
        const UINT8 *h=*ivrs+blocks[i];
        if(h[0]!=types[i] || h[4]!=2 || h[5] || h[6]!=0x40 || h[7] ||
           da64(h+8)!=0xf7600000ULL || h[16] || h[17])return EFI_SECURITY_VIOLATION;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS da_guard(TuEfi *p) {
    if(!p || !p->profile_verified || !p->bs || !p->bs->LocateProtocol ||
       !p->bs->GetMemoryMap || !p->bs->AllocatePool || !p->bs->FreePool || !p->direct_root)return EFI_ACCESS_DENIED;
    EFI_STATUS st=te_identity(p,p->master_enabled?7:3);if(EFI_ERROR(st))return st;
    st=da_cpu();if(EFI_ERROR(st))return st;
    VOID *iommu=0;st=p->bs->LocateProtocol(&D64IommuGuid,0,&iommu);
    field("direct_IOMMU_protocol_status",st);
    if(st!=EFI_NOT_FOUND || iommu)return EFI_UNSUPPORTED;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root=p->direct_root,*actual=0;
    st=d64_root(p,&actual);if(EFI_ERROR(st) || actual!=root)return EFI_SECURITY_VIOLATION;
    UINT8 code[96];
    if(!d64_read_code(p->bs,(VOID *)(UINTN)root->Map,code) ||
       !GuHashMatches(code,sizeof(code),"ca5165fd369ab13cbd5dc9d2d1f9b4cc989b09ec81072ed5093bdc691bffd132"))return EFI_SECURITY_VIOLATION;
    UINT64 function=(UINTN)root->Map;
    if(function>~(UINT64)0-0x5a58 || !da_readable(p,(VOID *)(UINTN)(function+0x5a50),8,0))return EFI_UNSUPPORTED;
    UINT64 hook=da64((VOID *)(UINTN)(function+0x5a50));field("direct_root_Map_hook",hook);
    if(hook)return EFI_UNSUPPORTED; /* do not evade an internal translation provider */
    const UINT8 *ivrs=0;st=da_ivrs(p,&ivrs);if(EFI_ERROR(st))return st;
    if(!da_readable(p,(VOID *)(UINTN)0xf7600000ULL,0x38,2))return EFI_UNSUPPORTED;
    EFI_CPU_IO2_PROTOCOL *cpu_io=0;
    st=p->bs->LocateProtocol(&DaCpuIoGuid,0,(VOID **)&cpu_io);
    field("direct_host_CPU_IO2_protocol_status",st);
    if(EFI_ERROR(st))return st;
    if(!cpu_io || !cpu_io->Mem.Read)return EFI_UNSUPPORTED;
    UINT32 efr_low=0,efr_high=0;
    st=da_host_read(cpu_io,0xf7600030ULL,&efr_low);
    if(!EFI_ERROR(st))st=da_host_read(cpu_io,0xf7600034ULL,&efr_high);
    field("direct_AMD_IOMMU_EFR_low",efr_low);field("direct_AMD_IOMMU_EFR_high",efr_high);
    if(EFI_ERROR(st))return st;
    if((((UINT64)efr_high<<32)|efr_low)!=da64(ivrs+116+24) || !(efr_low|efr_high))return EFI_SECURITY_VIOLATION;
    /* AMD IOMMU Control offset18, bit0=IommuEn. READ ONLY. No guessed
     * chipset register writes, no IOMMU disable or domain/table modification. */
    for(unsigned pass=0;pass<2;pass++) {
        UINT32 low=0,high=0;
        st=da_host_read(cpu_io,0xf7600018ULL,&low);
        if(!EFI_ERROR(st))st=da_host_read(cpu_io,0xf760001cULL,&high);
        field("direct_AMD_IOMMU_control_low",low);field("direct_AMD_IOMMU_control_high",high);
        if(EFI_ERROR(st))return st;
        if(low==0xffffffffU || high==0xffffffffU || (low&1))return EFI_ACCESS_DENIED;
    }
    if(p->pages_owned && (!p->pool || !da_readable(p,p->pool,TX_POOL_SIZE,1)))return EFI_SECURITY_VIOLATION;
    return LogFault?EFI_VOLUME_FULL:EFI_SUCCESS;
}
static EFI_STATUS da_platform(TuEfi *p,EFI_SYSTEM_TABLE *s) {
    DaSystem=s;EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root=0;
    EFI_STATUS st=d64_root(p,&root);if(EFI_ERROR(st))return st;
    p->direct_root=root;return da_guard(p);
}
static EFI_STATUS da_allocate(TuEfi *p) {
    if(!p || !p->owned || !p->profile_verified || p->fault || p->retained ||
       p->pool || p->mapping || p->mapped || p->tx.mutated || p->master_pending ||
       !p->bs->AllocatePages || !p->bs->FreePages)return EFI_ACCESS_DENIED;
    p->dma_step="direct-AMD-environment-guard";EFI_STATUS st=da_guard(p);if(EFI_ERROR(st))return st;
    EFI_PHYSICAL_ADDRESS address=0;p->dma_step="direct-owned-high-pages";
    st=th_pick(p->bs,&address);if(EFI_ERROR(st))return st;
    EFI_PHYSICAL_ADDRESS wanted=address;
    st=p->bs->AllocatePages(AllocateAddress,EfiBootServicesData,TX_POOL_SIZE/4096,&address);
    p->dma_allocate_status=st;if(EFI_ERROR(st))return st;
    p->pool=(VOID *)(UINTN)address;p->pages_owned=TRUE;
    if(address!=wanted)return EFI_COMPROMISED_DATA;
    st=da_guard(p);if(EFI_ERROR(st))return st;
    p->direct_physical=TRUE;p->device_address=address;p->tx.pool=p->pool;p->tx.device_address=address;
    p->dma_step="DIRECT-PHYSICAL-AMD-NOT-UEFI-MAPPED";p->dma_map_status=EFI_NOT_STARTED;
    field("direct_physical_DMA_selected",1);field("direct_owned_host_and_device_address",address);
    text("Experimental direct physical DMA assumption; UEFI Map deliberately not used.\r\n");
    return EFI_SUCCESS;
}
static int da_next_input(void *context) {
    TuEfi *p=context;
    if(!p->direct_physical || !p->pages_owned || p->tx.stage!=TX_STAGE_BOOTER_RESET ||
       !p->tx.mutated)return 1;
    return EFI_ERROR(da_guard(p));
}
#endif
