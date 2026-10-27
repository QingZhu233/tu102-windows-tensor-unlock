/* Actual EFI entry, release and GPU adapter, all firmware services mocked.
 * Firmware outcome simulation is NOT hardware validation. */
#define TU102_FULL_ENTRY_MAIN retained_full_entry_suite
#include "test_full_entry.c"
static int DriverToken,OtherDriver;
static EFI_DRIVER_BINDING_PROTOCOL DriverBinding;
static EFI_LOADED_IMAGE_PROTOCOL DriverImage;
static _Alignas(4096) UINT8 DriverMemory[0x21000];
static struct {EFI_DEVICE_PATH_PROTOCOL node;CHAR16 name[12];EFI_DEVICE_PATH_PROTOCOL end;} DriverPath;
static int DriverGone,StopFailure,StopKeepsOwner,StopChangesCommand,StopChangesGpu;
static int ChildOwner,OtherOwner,WrongController,WrongRecordCount,ExclusiveOwner;
static int MutateImageOnCheck,ImageCalls,DriverCalls,KeepPCI,WrongDriverPath;
static int StopDisplayChange,NoWindowsLoader,ChangeOwnerAfterPreload;
static int FailBeforeReleaseLog;
static unsigned ColdReadCount;
static unsigned DecodeRestores;
static int DecodeFail,DecodePartial,DecodeMismatch,DecodeUnsupported;
static EFI_STATUS EFIAPI driver_stop(EFI_DRIVER_BINDING_PROTOCOL *b,EFI_HANDLE h,UINTN n,EFI_HANDLE *c) {
    (void)b;(void)h;(void)n;(void)c;assert(!"Do not call Stop directly");return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI release_map(UINTN *n,EFI_MEMORY_DESCRIPTOR *p,UINTN *key,UINTN *stride,UINT32 *ver) {
    *stride=sizeof(*p);*key=1;*ver=1;
    if(!p || *n<sizeof(*p)){*n=sizeof(*p);return EFI_BUFFER_TOO_SMALL;}
    memset(p,0,sizeof(*p));p->Type=EfiBootServicesCode;
    p->PhysicalStart=(UINTN)DriverMemory;p->NumberOfPages=sizeof(DriverMemory)/4096;
    *n=sizeof(*p);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI release_handle(EFI_HANDLE h,EFI_GUID *g,VOID **out) {
    if(h==&DriverToken && guid_eq(g,&TrBindingGuid)){*out=&DriverBinding;return EFI_SUCCESS;}
    if(h==&DriverToken && guid_eq(g,&ImageGuid)) {
        ImageCalls++;if(MutateImageOnCheck && ImageCalls==2)DriverMemory[31]^=1;
        *out=&DriverImage;return EFI_SUCCESS;
    }
    if(DriverGone && !KeepPCI && h==&Devices[0] && guid_eq(g,&PciGuid)){*out=0;return EFI_NOT_FOUND;}
    return full_handle(h,g,out);
}
static EFI_STATUS EFIAPI release_protocols(EFI_HANDLE h,EFI_GUID ***g,UINTN *n) {
    assert(h==&Devices[0]);*n=2;*g=malloc(2*sizeof(**g));assert(*g);
    (*g)[0]=&PciGuid;(*g)[1]=&PathGuid;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI release_info(EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *n) {
    assert(h==&Devices[0]);int pci=guid_eq(g,&PciGuid);*n=0;*out=0;
    if((DriverGone && !StopKeepsOwner) || (!pci && !ChildOwner && !OtherOwner))return EFI_SUCCESS;
    *n=1;*out=calloc(1,sizeof(**out));assert(*out);
    (*out)[0].AgentHandle=((!pci && OtherOwner) || (ChangeOwnerAfterPreload && WinLoaded.ImageBase))?&OtherDriver:&DriverToken;
    (*out)[0].ControllerHandle=WrongController?(VOID *)&OtherDriver:(VOID *)&Devices[0];
    (*out)[0].OpenCount=WrongRecordCount?2:1;
    (*out)[0].Attributes=(!pci && ChildOwner)?EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER:
        ExclusiveOwner?EFI_OPEN_PROTOCOL_EXCLUSIVE:EFI_OPEN_PROTOCOL_BY_DRIVER;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI release_disconnect(EFI_HANDLE h,EFI_HANDLE d,EFI_HANDLE c) {
    assert(h==&Devices[0] && d==&DriverToken && !c && !DriverGone && !Port.tx.mutated);
#ifdef TU102_AUTOMATIC_INTERNAL
    assert(!KeyCalls && WinLoaded.ImageBase && !DmaAlloc && !DmaMap && !model.writes);
#else
    assert(KeyCalls && WinLoaded.ImageBase && !DmaAlloc && !DmaMap && !model.writes);
#endif
    DriverCalls++;DriverGone=1;
    if(StopFailure)return EFI_DEVICE_ERROR;
    if(StopChangesCommand==1){Config[0][1]=1;PciAttrs&=~EFI_PCI_IO_ATTRIBUTE_MEMORY;}
    if(StopChangesCommand==2){Config[0][1]=0;PciAttrs&=~(EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_IO);}
    if(StopChangesCommand==3){Config[0][1]=2;PciAttrs&=~EFI_PCI_IO_ATTRIBUTE_IO;}
    if(StopChangesCommand==4){Config[0][1]=7;PciAttrs|=EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;}
    if(StopChangesCommand==5)Config[0][1]^=0x10000000;
    if(StopChangesCommand==6)Config[0][4]^=0x1000000;
    if(StopChangesCommand==7)Config[0][16]^=1;
    if(StopChangesCommand==8)Config[0][1]^=0x100;
    if(StopChangesGpu)model.ss0^=1;
    if(StopDisplayChange)DisplayChanged=1;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI release_locate(EFI_LOCATE_SEARCH_TYPE t,EFI_GUID *g,VOID *key,UINTN *n,EFI_HANDLE **out) {
    if(NoWindowsLoader && guid_eq(g,&FsGuid)){*n=0;*out=malloc(sizeof(**out));assert(*out);return EFI_SUCCESS;}
    return full_locate(t,g,key,n,out);
}
static EFI_STATUS EFIAPI release_read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT8 b,UINT64 off,UINTN n,VOID *out) {
    EFI_STATUS s=full_read(p,w,b,off,n,out);
    if(off==0x409664)ColdReadCount++;
    return s;
}
static EFI_STATUS EFIAPI release_log_flush(EFI_FILE_PROTOCOL *file) {
    if(FailBeforeReleaseLog && strstr(Report,"one2080Ti nvgop-tu1xx release"))return EFI_DEVICE_ERROR;
    return file_flush(file);
}
static EFI_STATUS EFIAPI release_attrs(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 m,UINT64 *r) {
    UINT64 decode=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
    if(op==EfiPciIoAttributeOperationEnable && m && !(m&~decode)) {
        assert(p==&Devices[0] && DriverGone && Port.owned && !DmaAlloc && !model.writes);
        DecodeRestores++;
        if(DecodeFail && !DecodePartial)return EFI_DEVICE_ERROR;
        PciAttrs|=m;
        if(m&EFI_PCI_IO_ATTRIBUTE_IO)Config[0][1]|=1;
        if(m&EFI_PCI_IO_ATTRIBUTE_MEMORY)Config[0][1]|=2;
        if(DecodeMismatch)Config[0][17]^=1;
        return DecodeFail?EFI_DEVICE_ERROR:EFI_SUCCESS;
    }
    EFI_STATUS st=full_attrs(p,op,m,r);
    if(op==EfiPciIoAttributeOperationSupported && DecodeUnsupported)*r&=~decode;
    return st;
}
static void release_setup(void) {
    full_setup();memset(&DriverBinding,0,sizeof(DriverBinding));memset(&DriverImage,0,sizeof(DriverImage));
    memset(DriverMemory,0x71,sizeof(DriverMemory));memset(&DriverPath,0,sizeof(DriverPath));
    DriverBinding.ImageHandle=&DriverToken;DriverBinding.DriverBindingHandle=&DriverToken;
    DriverBinding.Version=0x50005;DriverBinding.Stop=driver_stop;
    DriverImage.ImageBase=DriverMemory;DriverImage.ImageSize=0x20960;
    DriverImage.ImageCodeType=EfiBootServicesCode;DriverImage.DeviceHandle=&Devices[0];
    DriverPath.node.Type=4;DriverPath.node.SubType=4;DriverPath.node.Length[0]=28;
    memcpy(DriverPath.name,L"nvgop-tu1xx",sizeof(L"nvgop-tu1xx"));
    DriverPath.end.Type=0x7f;DriverPath.end.SubType=0xff;DriverPath.end.Length[0]=4;
    DriverImage.FilePath=&DriverPath.node;
    Services.GetMemoryMap=release_map;Services.HandleProtocol=release_handle;
    Services.ProtocolsPerHandle=release_protocols;Services.OpenProtocolInformation=release_info;
    Services.DisconnectController=release_disconnect;Services.LocateHandleBuffer=release_locate;
    Devices[0].Mem.Read=release_read;Files[3].Flush=release_log_flush;
    Devices[0].Attributes=release_attrs;
    DriverGone=StopFailure=StopKeepsOwner=StopChangesCommand=StopChangesGpu=0;
    ChildOwner=OtherOwner=WrongController=WrongRecordCount=ExclusiveOwner=0;
    MutateImageOnCheck=ImageCalls=DriverCalls=WrongDriverPath=StopDisplayChange=NoWindowsLoader=0;
    KeepPCI=1;WinLoaded.ImageBase=0;
    ChangeOwnerAfterPreload=FailBeforeReleaseLog=0;ColdReadCount=0;
    DecodeRestores=0;DecodeFail=DecodePartial=DecodeMismatch=DecodeUnsupported=0;
}
static void refusal(EFI_STATUS expected) {
    assert(efi_main(&ImageToken,&System)==expected);
    assert(!DriverCalls && !DmaAlloc && !DmaMap && !model.writes && !WinStart && !StopCount);
}
static void stopped_before_dma(void) {
    assert(run_to_stop() && DriverCalls==1 && !DmaAlloc && !DmaMap && !model.writes && !WinStart);
}
#ifndef TU102_RELEASE_TEST_MAIN
#define TU102_RELEASE_TEST_MAIN main
#endif
int TU102_RELEASE_TEST_MAIN(void) {
    load("captures/capture-04/NATIVE-FWSEC.BIN",native,sizeof(native));
    load("assets/upstream-cmp50hx-efi/bl_gsp_tu102.bin",generic,sizeof(generic));
    load("assets/upstream-cmp50hx-efi/booter_ucode_prod.bin",booter,sizeof(booter));
    load("assets/upstream-cmp50hx-efi/v67_payload.bin",signature,sizeof(signature));
    load("assets/upstream-cmp50hx-efi/gsp_rm_boot_dbg.bin",boot,sizeof(boot));
    load("captures/capture-04/LIVE-PROM.BIN",FullProm,sizeof(FullProm));
    unsigned tests=0;
    release_setup();assert(run_to_stop() && DriverCalls==1 && WinStart==1 && Port.tx.unlocked && Port.tx.quiescent && DmaFree==1);tests++;
    release_setup();ChildOwner=1;refusal(EFI_ACCESS_DENIED);tests++;
    release_setup();OtherOwner=1;refusal(EFI_ACCESS_DENIED);tests++;
    release_setup();WrongController=1;refusal(EFI_ACCESS_DENIED);tests++;
    release_setup();WrongRecordCount=1;refusal(EFI_ACCESS_DENIED);tests++;
    release_setup();ExclusiveOwner=1;refusal(EFI_ACCESS_DENIED);tests++;
    release_setup();DriverBinding.Version^=1;refusal(EFI_UNSUPPORTED);tests++;
    release_setup();DriverImage.ImageSize--;refusal(EFI_UNSUPPORTED);tests++;
    release_setup();DriverImage.DeviceHandle=&GopToken;refusal(EFI_UNSUPPORTED);tests++;
    release_setup();DriverPath.name[0]='x';refusal(EFI_UNSUPPORTED);tests++;
    release_setup();DriverImage.ImageCodeType=EfiReservedMemoryType;refusal(EFI_UNSUPPORTED);tests++;
    release_setup();DriverBinding.Stop=0;refusal(EFI_UNSUPPORTED);tests++;
    release_setup();DriverImage.ImageBase=0;refusal(EFI_UNSUPPORTED);tests++;
    release_setup();MutateImageOnCheck=1;refusal(EFI_ABORTED);tests++;
    release_setup();Key=27;refusal(EFI_ABORTED);tests++;
    release_setup();NoWindowsLoader=1;refusal(EFI_NOT_FOUND);tests++;
    release_setup();model.ss0=0x88888888;refusal(EFI_SECURITY_VIOLATION);tests++;
    release_setup();StopFailure=1;stopped_before_dma();tests++;
    release_setup();StopKeepsOwner=1;stopped_before_dma();tests++;
    release_setup();StopChangesCommand=1;assert(run_to_stop() && DriverCalls==1 && WinStart==1 && DecodeRestores==1 && Port.tx.quiescent);tests++;
    release_setup();StopChangesGpu=1;stopped_before_dma();tests++;
    release_setup();StopDisplayChange=1;stopped_before_dma();tests++;
    release_setup();KeepPCI=0;stopped_before_dma();tests++;
    release_setup();NoDac=1;stopped_before_dma();tests++;
    release_setup();PartialDmaMap=1;assert(run_to_stop() && DriverCalls==1 && !model.writes && !WinStart && DmaUnmap==1 && DmaFree==1);tests++;
    release_setup();model.fail_write=1;assert(run_to_stop() && DriverCalls==1 && !WinStart && Port.retained && !DmaFree);tests++;
    release_setup();Port.read_only_probe=TRUE;assert(te_write(&Port,0x1103c0,1)!=0 && !model.writes);tests++;
    release_setup();ChangeOwnerAfterPreload=1;refusal(EFI_UNSUPPORTED);tests++;
    release_setup();FailBeforeReleaseLog=1;refusal(EFI_DEVICE_ERROR);tests++;
    release_setup();assert(run_to_stop() && DriverCalls==1 && ColdReadCount>=4 && WinStart==1);tests++;
    for(int bits=2;bits<=3;bits++) {
        release_setup();StopChangesCommand=bits;
        assert(run_to_stop() && DriverCalls==1 && WinStart==1 && DecodeRestores==1 && Port.tx.quiescent);tests++;
    }
    for(int bad=4;bad<=8;bad++) {
        release_setup();StopChangesCommand=bad;stopped_before_dma();assert(!DecodeRestores);tests++;
    }
    release_setup();StopChangesCommand=1;DecodeFail=1;stopped_before_dma();assert(DecodeRestores==1);tests++;
    release_setup();StopChangesCommand=1;DecodeFail=DecodePartial=1;stopped_before_dma();assert(DecodeRestores==1 && Config[0][1]==3);tests++;
    release_setup();StopChangesCommand=1;DecodeMismatch=1;stopped_before_dma();assert(DecodeRestores==1);tests++;
    release_setup();StopChangesCommand=1;DecodeUnsupported=1;stopped_before_dma();assert(!DecodeRestores);tests++;
    release_setup();StopChangesCommand=1;StopKeepsOwner=1;stopped_before_dma();assert(!DecodeRestores);tests++;
    printf("PASS %u specific2080Ti driver-release entry/refusal/containment cases; firmware services mocked\n",tests);
    return 0;
}
