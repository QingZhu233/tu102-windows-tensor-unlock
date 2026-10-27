/* Actual armed entry + core + EFI DMA + Windows handoff in one mocked host.
 * StartImage cannot boot an OS here; when it returns, the runtime must STOP.
 */
#define TU102_ENABLE_UNVERIFIED_EXECUTION
#define TU102_HOST_TEST
#define TU102_APP_TEST_MAIN refusal_suite_main
#include "test_app.c"
#define main core_model_suite_main
#define setup core_model_setup
#include "test_core.c"
#undef setup
#undef main
#include <setjmp.h>

static jmp_buf stopped;
static _Alignas(4096) UINT8 DmaPool[TX_POOL_SIZE];
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL WinFs;
static EFI_FILE_PROTOCOL WinRoot,WinFile;
static EFI_LOADED_IMAGE_PROTOCOL WinLoaded;
static EFI_DEVICE_PATH_PROTOCOL WinPath={0x7f,0xff,{4,0}};
static int WinVolume,WinImage;
static unsigned DmaAlloc,DmaMap,DmaFree,DmaUnmap,WinStart,WinUnload,ExclusiveOpen,ExclusiveClose,StopCount;
static UINT64 PciAttrs;
static int NoDac,DeniedOwner,PartialDmaMap,FailDmaFree,FailDmaUnmap;
static int ChangeSelectors;
static unsigned PromPasses;
void tu102_host_stop(void){StopCount++;longjmp(stopped,1);}
static EFI_STATUS EFIAPI full_locate(EFI_LOCATE_SEARCH_TYPE t,EFI_GUID *g,VOID *key,UINTN *n,EFI_HANDLE **out) {
    if(guid_eq(g,&FsGuid)) {
        *n=1;*out=malloc(sizeof(**out));assert(*out);(*out)[0]=&WinVolume;return EFI_SUCCESS;
    }
    return app_locate(t,g,key,n,out);
}
static EFI_STATUS EFIAPI full_handle(EFI_HANDLE h,EFI_GUID *g,VOID **out) {
    if(h==&WinVolume && guid_eq(g,&FsGuid)){*out=&WinFs;return EFI_SUCCESS;}
    if(h==&WinVolume && guid_eq(g,&PathGuid)){*out=&WinPath;return EFI_SUCCESS;}
    if(h==&WinImage && guid_eq(g,&ImageGuid)){*out=&WinLoaded;return EFI_SUCCESS;}
    return app_handle(h,g,out);
}
static EFI_STATUS EFIAPI full_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,EFI_FILE_PROTOCOL **out){assert(fs==&WinFs);*out=&WinRoot;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI full_open(EFI_FILE_PROTOCOL *r,EFI_FILE_PROTOCOL **out,CHAR16 *n,UINT64 m,UINT64 a) {
    assert(r==&WinRoot && eq16(n,L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi") && m==EFI_FILE_MODE_READ && !a);
    *out=&WinFile;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_close(EFI_FILE_PROTOCOL *f){assert(f==&WinRoot || f==&WinFile);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI full_load(BOOLEAN b,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *p,VOID *s,UINTN n,EFI_HANDLE *out) {
    assert(!b && parent==&ImageToken && p && !s && !n);*out=&WinImage;
    WinLoaded.DeviceHandle=&WinVolume;WinLoaded.ImageBase=(VOID *)&full_load;WinLoaded.ImageSize=0x100000;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_start(EFI_HANDLE h,UINTN *n,CHAR16 **data) {
    assert(h==&WinImage && !n && !data && Port.tx.quiescent && Port.tx.unlocked);
    assert(Port.released && !Port.pool && !Port.mapping && Config[0][1]==3 && !Port.owned);
    WinStart++;return EFI_ABORTED;
}
static EFI_STATUS EFIAPI full_unload(EFI_HANDLE h){assert(h==&WinImage);WinUnload++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI full_protocols(EFI_HANDLE h,EFI_GUID ***g,UINTN *n) {
    assert(h==&Devices[0]);if(DeniedOwner)return EFI_ACCESS_DENIED;
    *g=malloc(sizeof(**g));assert(*g);(*g)[0]=&PciGuid;*n=1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_info(EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **r,UINTN *n) {
    assert(h==&Devices[0] && guid_eq(g,&PciGuid));*r=0;*n=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_claim(EFI_HANDLE h,EFI_GUID *g,VOID **p,EFI_HANDLE a,EFI_HANDLE c,UINT32 flags) {
    assert(h==&Devices[0] && guid_eq(g,&PciGuid) && a==&ImageToken && !c && flags==EFI_OPEN_PROTOCOL_EXCLUSIVE);
    *p=&Devices[0];ExclusiveOpen++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_release(EFI_HANDLE h,EFI_GUID *g,EFI_HANDLE a,EFI_HANDLE c) {
    assert(h==&Devices[0] && guid_eq(g,&PciGuid) && a==&ImageToken && !c);ExclusiveClose++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_attrs(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 m,UINT64 *r) {
    assert(p==&Devices[0]);
    if(op==EfiPciIoAttributeOperationSupported){assert(!m && r);*r=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER|(NoDac?0:EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE);return EFI_SUCCESS;}
    if(op==EfiPciIoAttributeOperationGet){assert(!m && r);*r=PciAttrs;return EFI_SUCCESS;}
    assert(!r && (m==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER || m==EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE));
    if(op==EfiPciIoAttributeOperationEnable){PciAttrs|=m;if(m==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)Config[0][1]|=4;}
    else {assert(op==EfiPciIoAttributeOperationDisable);PciAttrs&=~m;if(m==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)Config[0][1]&=~4u;}
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_alloc(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE t,EFI_MEMORY_TYPE m,UINTN n,VOID **cpu,UINT64 a) {
    assert(p==&Devices[0] && t==AllocateAnyPages && m==EfiBootServicesData && n==TX_POOL_SIZE/4096 && !a);
    assert(PciAttrs&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE);
    assert(Config[0][1]==3);*cpu=DmaPool;DmaAlloc++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_map(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION o,VOID *cpu,UINTN *n,EFI_PHYSICAL_ADDRESS *dev,VOID **map) {
    assert(p==&Devices[0] && o==EfiPciIoOperationBusMasterCommonBuffer && cpu==DmaPool && *n==TX_POOL_SIZE);
    *dev=0x800000000ULL;*map=&DmaMap;DmaMap++;if(PartialDmaMap)*n/=2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_free(EFI_PCI_IO_PROTOCOL *p,UINTN n,VOID *cpu) {
    assert(p==&Devices[0] && n==TX_POOL_SIZE/4096 && cpu==DmaPool && Config[0][1]==3);
    DmaFree++;return FailDmaFree?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_unmap(EFI_PCI_IO_PROTOCOL *p,VOID *m) {
    assert(p==&Devices[0] && m==&DmaMap && Config[0][1]==3);DmaUnmap++;return FailDmaUnmap?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT8 b,UINT64 off,UINTN n,VOID *out) {
    assert(p==&Devices[0] && w==EfiPciIoWidthUint32 && !b);
    if(off>=0x300000 && off+n*4<=0x380000) {
        memcpy(out,FullProm+off-0x300000,(size_t)n*4);
        if(off==0x37f000 && n==1024 && ++PromPasses==3 && ChangeSelectors)model.ss0^=1;
        return EFI_SUCCESS;
    }
    assert(n==1);return model_read(&model,(uint32_t)off,out)?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT8 b,UINT64 off,UINTN n,VOID *v) {
    assert(p==&Devices[0] && w==EfiPciIoWidthUint32 && !b && n==1 && Config[0][1]==7);
    return model_write(&model,(uint32_t)off,*(uint32_t *)v)?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI full_flush(EFI_PCI_IO_PROTOCOL *p){assert(p==&Devices[0]);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI full_stall(UINTN us){return model_delay(&model,(uint32_t)us)?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI watchdog(UINTN timeout,UINT64 code,UINTN n,CHAR16 *data) {
    assert(!timeout && !code && !n && !data);return EFI_SUCCESS;
}
static void full_setup(void) {
    app_setup();tx.pool=DmaPool;core_model_setup();model.tx=&Port.tx;
    Services.LocateHandleBuffer=full_locate;Services.HandleProtocol=full_handle;
    Services.OpenProtocol=full_claim;Services.CloseProtocol=full_release;
    Services.ProtocolsPerHandle=full_protocols;Services.OpenProtocolInformation=full_info;
    Services.LoadImage=full_load;Services.StartImage=full_start;Services.UnloadImage=full_unload;
    Services.Stall=full_stall;Services.SetWatchdogTimer=watchdog;
    Devices[0].Mem.Read=full_read;Devices[0].Mem.Write=full_write;Devices[0].Attributes=full_attrs;
    Devices[0].AllocateBuffer=full_alloc;Devices[0].FreeBuffer=full_free;
    Devices[0].Map=full_map;Devices[0].Unmap=full_unmap;Devices[0].Flush=full_flush;
    WinFs.OpenVolume=full_volume;WinRoot.Open=full_open;WinRoot.Close=WinFile.Close=full_close;
    DmaAlloc=DmaMap=DmaFree=DmaUnmap=WinStart=WinUnload=ExclusiveOpen=ExclusiveClose=StopCount=0;
    NoDac=DeniedOwner=PartialDmaMap=FailDmaFree=FailDmaUnmap=0;PciAttrs=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
    ChangeSelectors=0;PromPasses=0;
}
static int run_to_stop(void) {
    if(!setjmp(stopped)){
        EFI_STATUS s=efi_main(&ImageToken,&System);
        printf("runtime returned %#llx, owner=%u alloc=%u map=%u writes=%u stage=%u\n%s\n",
            (unsigned long long)s,ExclusiveOpen,DmaAlloc,DmaMap,model.writes,Port.tx.stage,Report);
        return 0;
    }
    return 1;
}
#ifndef TU102_FULL_ENTRY_MAIN
#define TU102_FULL_ENTRY_MAIN main
#endif
int TU102_FULL_ENTRY_MAIN(void) {
    load("captures/capture-04/NATIVE-FWSEC.BIN",native,sizeof(native));
    load("assets/upstream-cmp50hx-efi/bl_gsp_tu102.bin",generic,sizeof(generic));
    load("assets/upstream-cmp50hx-efi/booter_ucode_prod.bin",booter,sizeof(booter));
    load("assets/upstream-cmp50hx-efi/v67_payload.bin",signature,sizeof(signature));
    load("assets/upstream-cmp50hx-efi/gsp_rm_boot_dbg.bin",boot,sizeof(boot));
    load("captures/capture-04/LIVE-PROM.BIN",FullProm,sizeof(FullProm));
    unsigned tests=0;
    full_setup();assert(run_to_stop() && WinStart==1 && DmaFree==1 && DmaUnmap==1 && ExclusiveClose==1);tests++;
    assert(Port.tx.unlocked && Port.tx.quiescent && !Port.retained);tests++;
    full_setup();DeniedOwner=1;assert(efi_main(&ImageToken,&System)==EFI_ACCESS_DENIED && !DmaAlloc && !model.writes);tests++;
    full_setup();NoDac=1;assert(efi_main(&ImageToken,&System)==EFI_UNSUPPORTED && !DmaAlloc && !model.writes && ExclusiveClose==1);tests++;
    full_setup();PartialDmaMap=1;assert(efi_main(&ImageToken,&System)==EFI_UNSUPPORTED && !model.writes && DmaUnmap==1 && DmaFree==1);tests++;
    full_setup();model.ss0=0x88888888;assert(efi_main(&ImageToken,&System)==EFI_SECURITY_VIOLATION && !model.writes && !DmaAlloc);tests++;
    full_setup();model.ss0=0x12345678;assert(run_to_stop() && WinStart==1 && Port.tx.quiescent);tests++;
    full_setup();ChangeSelectors=1;assert(efi_main(&ImageToken,&System)==EFI_SECURITY_VIOLATION && !model.writes && DmaFree==1);tests++;
    full_setup();model.ss0=0x62004020;model.ss1=2;assert(run_to_stop() && WinStart==1 && Port.tx.quiescent);tests++;
    full_setup();model.fail_write=1;assert(run_to_stop() && !WinStart && Port.retained && !DmaFree && !DmaUnmap && Config[0][1]==3);tests++;
    full_setup();model.mode=5;assert(run_to_stop() && !WinStart && Port.retained && !DmaFree && !DmaUnmap);tests++;
    full_setup();FailDmaUnmap=1;assert(run_to_stop() && !WinStart && Port.retained && DmaUnmap==1 && !DmaFree);tests++;
    full_setup();FailDmaFree=1;assert(run_to_stop() && !WinStart && Port.retained && DmaUnmap==1 && DmaFree==1);tests++;
    full_setup();app_inactive_console();assert(run_to_stop() && WinStart==1 && Port.tx.quiescent && DmaFree==1);tests++;
    full_setup();app_inactive_console();model.fail_write=1;assert(run_to_stop() && !WinStart && Port.retained && !DmaFree && !DmaUnmap);tests++;
    full_setup();app_capture09_console();assert(run_to_stop() && WinStart==1 && Port.tx.quiescent);tests++;
    full_setup();app_capture09_console();DeniedOwner=1;assert(efi_main(&ImageToken,&System)==EFI_ACCESS_DENIED && !model.writes && !DmaAlloc);tests++;
    printf("PASS %u end-to-end EFI/core/DMA/Windows-handoff mock cases; no hardware\n",tests);
    return 0;
}
