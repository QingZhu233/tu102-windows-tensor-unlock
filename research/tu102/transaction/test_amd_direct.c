/* Actual direct-AMD entry and full core with mocked firmware/CPU/MMIO.
 * No real MSR, IOMMU, PCI, GPU or physical high RAM access in tests. */
#define TU102_AMD_DIRECT_PHYSICAL
#define TU102_AMD_DIRECT_HOST_TEST
#define TU102_RELEASE_TEST_MAIN retained_release_tests
#include "test_owner_release.c"
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL AmdRoot;
static EFI_CPU_IO2_PROTOCOL AmdCpuIo;
static int AmdRootToken;
static _Alignas(4096) UINT8 AmdMapCode[0x6000];
static _Alignas(4096) UINT8 AcpiMemory[4096];
static UINT8 *Rsdp,*Xsdt,*Ivrs;
static EFI_CONFIGURATION_TABLE AcpiConfig;
static UINT8 CardPath[]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,1,1,6,0,3,1,1,1,6,0,0,0,0x7f,0xff,4,0};
static unsigned DirectPages,DirectFrees,ControlReads,ActualMapCalls;
static unsigned CacheWritebacks;
static int TranslationEnabled,CpuWrong,HypervisorActive,EncryptionActive,HookPresent;
static int IvrsCorrupt,RootCodeCorrupt,PageAllocationFail,UnownedPages,ControlReadError;
static int EnableTranslationAtPhase2,EncryptionSnp,WrongEFR,HostIoMissing,RootIoRejects;
void tu102_amd_cache_writeback_mock(void){CacheWritebacks++;}
static EFI_STATUS EFIAPI amd_forbid_unmap(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root,VOID *token) {
    (void)root;(void)token;assert(!"Direct profile must not call Map/Unmap");return EFI_UNSUPPORTED;
}
static void put64(UINT8 *p,UINT64 v){for(unsigned i=0;i<8;i++)p[i]=(UINT8)(v>>(8*i));}
static void checksum(UINT8 *p,UINTN n,UINTN offset){p[offset]=0;UINT8 sum=0;for(UINTN i=0;i<n;i++)sum+=p[i];p[offset]=(UINT8)(0-sum);}
void tu102_amd_cpu_snapshot(UINT32 *b,UINT32 *c,UINT32 *d,UINT32 *sig,UINT32 *flags,UINT32 *bits,UINT32 *enc,UINT32 *encb,UINT64 *syscfg) {
    *b=0x68747541;*d=0x69746e65;*c=0x444d4163;*sig=CpuWrong?0:0xb40f40;
    *flags=HypervisorActive?0x80000000U:0;*bits=48;*enc=1;*encb=51;
    *syscfg=EncryptionActive?1ULL<<23:EncryptionSnp?1ULL<<24:0;
}
static EFI_STATUS EFIAPI amd_memory_map(UINTN *n,EFI_MEMORY_DESCRIPTOR *map,UINTN *key,UINTN *stride,UINT32 *version) {
    *key=1;*stride=sizeof(*map);*version=1;const UINTN count=5;
    if(!map || *n<count*sizeof(*map)){*n=count*sizeof(*map);return EFI_BUFFER_TOO_SMALL;}
    memset(map,0,count*sizeof(*map));
    map[0].Type=EfiBootServicesCode;map[0].PhysicalStart=(UINTN)DriverMemory;map[0].NumberOfPages=sizeof(DriverMemory)/4096;
    map[1].Type=EfiBootServicesCode;map[1].PhysicalStart=(UINTN)AmdMapCode;map[1].NumberOfPages=sizeof(AmdMapCode)/4096;
    map[2].Type=EfiACPIMemoryNVS;map[2].PhysicalStart=(UINTN)AcpiMemory;map[2].NumberOfPages=1;
    map[3].Type=DirectPages>DirectFrees && !UnownedPages?EfiBootServicesData:EfiConventionalMemory;
    map[3].PhysicalStart=(UINTN)DmaPool;map[3].NumberOfPages=TX_POOL_SIZE/4096;map[3].Attribute=EFI_MEMORY_WB;
    map[4].Type=EfiMemoryMappedIO;map[4].PhysicalStart=0xf7600000ULL;map[4].NumberOfPages=16;
    *n=count*sizeof(*map);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI amd_handle(EFI_HANDLE h,EFI_GUID *g,VOID **out) {
    if(h==&Devices[0] && guid_eq(g,&PathGuid)){*out=CardPath;return EFI_SUCCESS;}
    if(h==&AmdRootToken && guid_eq(g,&D64RootGuid)){*out=&AmdRoot;return EFI_SUCCESS;}
    return release_handle(h,g,out);
}
static EFI_STATUS EFIAPI amd_path(EFI_GUID *g,EFI_DEVICE_PATH_PROTOCOL **path,EFI_HANDLE *out) {
    assert(guid_eq(g,&D64RootGuid) && (VOID *)*path==CardPath);
    *path=(VOID *)(CardPath+12);*out=&AmdRootToken;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI amd_iommu_protocol(EFI_GUID *g,VOID *registration,VOID **out) {
    assert(!registration);*out=0;
    if(guid_eq(g,&DaCpuIoGuid)){if(HostIoMissing)return EFI_NOT_FOUND;*out=&AmdCpuIo;return EFI_SUCCESS;}
    assert(guid_eq(g,&D64IommuGuid));return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI amd_control(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 address,UINTN count,VOID *out);
static EFI_STATUS EFIAPI amd_cpu_read(EFI_CPU_IO2_PROTOCOL *cpu,EFI_CPU_IO_PROTOCOL_WIDTH w,UINT64 address,UINTN count,VOID *out) {
    assert(cpu==&AmdCpuIo && w==EfiCpuIoWidthUint32);
    return amd_control(&AmdRoot,EfiPciWidthUint32,address,count,out);
}
static EFI_STATUS EFIAPI amd_root_reject(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 address,UINTN count,VOID *out) {
    (void)root;(void)w;(void)address;(void)count;(void)out;RootIoRejects++;return EFI_INVALID_PARAMETER;
}
static EFI_STATUS EFIAPI amd_control(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 address,UINTN count,VOID *out) {
    assert(root==&AmdRoot && w==EfiPciWidthUint32 && count==1 && (address==0xf7600018ULL || address==0xf760001cULL || address==0xf7600030ULL || address==0xf7600034ULL));
    ControlReads++;if(ControlReadError)return EFI_DEVICE_ERROR;
    if(EnableTranslationAtPhase2 && Port.tx.stage==TX_STAGE_BOOTER_RESET)TranslationEnabled=1;
    if(address==0xf7600030ULL)*(UINT32 *)out=da32(Ivrs+116+24)^(WrongEFR?1:0);
    else if(address==0xf7600034ULL)*(UINT32 *)out=da32(Ivrs+116+28);
    else *(UINT32 *)out=address==0xf7600018ULL?(UINT32)TranslationEnabled:0;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI amd_pages(EFI_ALLOCATE_TYPE type,EFI_MEMORY_TYPE memory,UINTN n,EFI_PHYSICAL_ADDRESS *address) {
    assert(type==AllocateAddress && memory==EfiBootServicesData && n==TX_POOL_SIZE/4096 && *address==(UINTN)DmaPool && Port.owned && !Port.master_enabled);
    if(PageAllocationFail)return EFI_OUT_OF_RESOURCES;
    DirectPages++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI amd_free(EFI_PHYSICAL_ADDRESS address,UINTN n) {
    assert(address==(UINTN)DmaPool && n==TX_POOL_SIZE/4096 && Config[0][1]==3);DirectFrees++;return EFI_SUCCESS;
}
static void amd_setup(void) {
    release_setup();memset(&AmdRoot,0,sizeof(AmdRoot));memset(AmdMapCode,0,sizeof(AmdMapCode));memset(AcpiMemory,0,sizeof(AcpiMemory));
    memset(&AmdCpuIo,0,sizeof(AmdCpuIo));AmdCpuIo.Mem.Read=amd_cpu_read;
    /* Test fixtures pin the exact live code/IVRS; no firmware blobs execute. */
    load("build/transaction/capture-19/ROOT-MAP-FIRST96.bin",AmdMapCode,96);
    Rsdp=AcpiMemory;Xsdt=AcpiMemory+64;Ivrs=AcpiMemory+128;
    load("build/transaction/amd-platform/WINDOWS-IVRS.bin",Ivrs,200);
    memcpy(Rsdp,"RSD PTR ",8);Rsdp[15]=2;tx_put32(Rsdp+20,36);put64(Rsdp+24,(UINTN)Xsdt);checksum(Rsdp,20,8);checksum(Rsdp,36,32);
    memcpy(Xsdt,"XSDT",4);tx_put32(Xsdt+4,44);put64(Xsdt+36,(UINTN)Ivrs);checksum(Xsdt,44,9);
    AcpiConfig.VendorGuid=DaAcpiGuid;AcpiConfig.VendorTable=Rsdp;
    System.NumberOfTableEntries=1;System.ConfigurationTable=&AcpiConfig;
    AmdRoot.Map=(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_MAP)(UINTN)AmdMapCode;AmdRoot.Unmap=amd_forbid_unmap;AmdRoot.Mem.Read=amd_root_reject;
    Services.GetMemoryMap=amd_memory_map;Services.HandleProtocol=amd_handle;
    Services.LocateDevicePath=amd_path;Services.LocateProtocol=amd_iommu_protocol;
    Services.AllocatePages=amd_pages;Services.FreePages=amd_free;
    DirectPages=DirectFrees=ControlReads=ActualMapCalls=0;
    CacheWritebacks=0;
    TranslationEnabled=CpuWrong=HypervisorActive=EncryptionActive=HookPresent=0;
    IvrsCorrupt=RootCodeCorrupt=PageAllocationFail=UnownedPages=ControlReadError=0;
    EnableTranslationAtPhase2=EncryptionSnp=WrongEFR=HostIoMissing=RootIoRejects=0;CardPath[16]=3;
}
static void amd_refusal(EFI_STATUS expected) {
    EFI_STATUS st=efi_main(&ImageToken,&System);
    if(st!=expected)printf("unexpected AMD status:%llx expected:%llx\n%s\n",(unsigned long long)st,(unsigned long long)expected,Report);
    assert(st==expected && !DriverCalls && !DirectPages && !model.writes && !WinStart);
}
#ifndef TU102_AMD_TEST_MAIN
#define TU102_AMD_TEST_MAIN main
#endif
int TU102_AMD_TEST_MAIN(void) {
    load("captures/capture-04/NATIVE-FWSEC.BIN",native,sizeof(native));
    load("assets/upstream-cmp50hx-efi/bl_gsp_tu102.bin",generic,sizeof(generic));
    load("assets/upstream-cmp50hx-efi/booter_ucode_prod.bin",booter,sizeof(booter));
    load("assets/upstream-cmp50hx-efi/v67_payload.bin",signature,sizeof(signature));
    load("assets/upstream-cmp50hx-efi/gsp_rm_boot_dbg.bin",boot,sizeof(boot));
    load("captures/capture-04/LIVE-PROM.BIN",FullProm,sizeof(FullProm));
    assert((UINTN)DmaPool>=0x110000000ULL && (UINTN)DmaPool+TX_POOL_SIZE<=0x200000000ULL);
    unsigned tests=0;
    amd_setup();int finished=run_to_stop();
    if(!finished || !WinStart)fprintf(stderr,"AMD stopped: stage=%s\n%s\n",Step,Report);
    assert(finished && WinStart==1 && Port.tx.unlocked && Port.tx.quiescent && DirectPages==1 && DirectFrees==1 && !DmaMap && !DmaUnmap && !DmaAlloc && CacheWritebacks==2);tests++;
    amd_setup();TranslationEnabled=1;amd_refusal(EFI_ACCESS_DENIED);tests++;
    amd_setup();CpuWrong=1;amd_refusal(EFI_UNSUPPORTED);tests++;
    amd_setup();HypervisorActive=1;amd_refusal(EFI_UNSUPPORTED);tests++;
    amd_setup();EncryptionActive=1;amd_refusal(EFI_UNSUPPORTED);tests++;
    amd_setup();EncryptionSnp=1;amd_refusal(EFI_UNSUPPORTED);tests++;
    amd_setup();put64(AmdMapCode+0x5a50,1);amd_refusal(EFI_UNSUPPORTED);tests++;
    amd_setup();AmdMapCode[55]^=1;amd_refusal(EFI_SECURITY_VIOLATION);tests++;
    amd_setup();Ivrs[74]^=1;amd_refusal(EFI_SECURITY_VIOLATION);tests++;
    amd_setup();ControlReadError=1;amd_refusal(EFI_DEVICE_ERROR);tests++;
    amd_setup();CardPath[16]=1;amd_refusal(EFI_SECURITY_VIOLATION);tests++;
    amd_setup();Key=27;amd_refusal(EFI_ABORTED);tests++;
    amd_setup();PageAllocationFail=1;assert(run_to_stop() && DriverCalls==1 && !DirectPages && !model.writes && !WinStart);tests++;
    amd_setup();UnownedPages=1;assert(run_to_stop() && DriverCalls==1 && DirectPages==1 && DirectFrees==1 && !model.writes && !WinStart);tests++;
    amd_setup();EnableTranslationAtPhase2=1;assert(run_to_stop() && model.fw_starts==1 && !model.booter_starts && Port.retained && DirectPages==1 && !DirectFrees && !WinStart && Config[0][1]==3);tests++;
    amd_setup();model.fail_write=1;assert(run_to_stop() && Port.retained && !DirectFrees && !WinStart);tests++;
    amd_setup();StopChangesCommand=2;assert(run_to_stop() && DecodeRestores==1 && WinStart==1 && DirectFrees==1);tests++;
    amd_setup();ChildOwner=1;amd_refusal(EFI_ACCESS_DENIED);tests++;
    amd_setup();NoWindowsLoader=1;amd_refusal(EFI_NOT_FOUND);tests++;
    amd_setup();WrongEFR=1;amd_refusal(EFI_SECURITY_VIOLATION);tests++;
    amd_setup();System.NumberOfTableEntries=0;amd_refusal(EFI_COMPROMISED_DATA);tests++;
    amd_setup();Rsdp[8]^=1;amd_refusal(EFI_COMPROMISED_DATA);tests++;
    amd_setup();Xsdt[10]^=1;amd_refusal(EFI_COMPROMISED_DATA);tests++;
    amd_setup();HostIoMissing=1;amd_refusal(EFI_NOT_FOUND);tests++;
    amd_setup();assert(run_to_stop() && WinStart==1 && !RootIoRejects && ControlReads>0);tests++;
    amd_setup();UINT32 rejected=0;assert(da_host_read(&AmdCpuIo,0xf7600020,&rejected)==EFI_ACCESS_DENIED && !ControlReads);tests++;
    amd_setup();AmdCpuIo.Mem.Read=0;amd_refusal(EFI_UNSUPPORTED);tests++;
    printf("PASS %u actual AMD direct-entry/guards/lifetime mock cases; no real CPU-MSR/IOMMU/GPU execution\n",tests);
    return 0;
}
