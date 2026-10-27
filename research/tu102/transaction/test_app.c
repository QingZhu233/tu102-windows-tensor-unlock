/* Actual default/experimental entry, real PROM and hashes, all services mocked.
 * Tests refusal/cancel/owner boundaries before any device mutation.
 */
#include "experiment.c"
#define TU102_PREFLIGHT_SOURCE_INCLUDED
#define main retained_capture_suite_main
#include "../test_preflight.c"
#undef main

static EFI_RUNTIME_SERVICES Runtime;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Output;
static UINT8 FullProm[0x80000];
static UINT8 GopPath[40]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,1,1,6,0,1,1,1,1,6,0,0,0,2,3,8,0,0x40,3,1,0x80,0x7f,0xff,4,0};
static int GopToken,ConsoleToken;
static EFI_GRAPHICS_OUTPUT_PROTOCOL PhysicalGop,ConsoleGop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE PhysicalMode,ConsoleMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION PhysicalInfo,ConsoleInfo;
static int Secure,RomChanged,CardReadFail,DisplayChanged,Key,ResetFail,OwnerAttempts;
static unsigned ReadCalls,KeyCalls,WaitCalls;
static EFI_STATUS EFIAPI app_variable(CHAR16 *name,EFI_GUID *g,UINT32 *attrs,UINTN *n,VOID *v) {
    (void)g;assert(eq16(name,L"SecureBoot") && !attrs && *n==1);*(UINT8 *)v=(UINT8)Secure;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI app_locate(EFI_LOCATE_SEARCH_TYPE t,EFI_GUID *g,VOID *k,UINTN *n,EFI_HANDLE **out) {
    if(guid_eq(g,&PciGuid))return locate(t,g,k,n,out);
    assert(guid_eq(g,&GopGuid));*n=2;*out=malloc(2*sizeof(**out));assert(*out);
    (*out)[0]=&GopToken;(*out)[1]=&ConsoleToken;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI app_handle(EFI_HANDLE h,EFI_GUID *g,VOID **out) {
    if(h==&ConsoleToken && guid_eq(g,&TdTextOutGuid)){*out=&Output;return EFI_SUCCESS;}
    if(h==&GopToken && guid_eq(g,&GopGuid)){*out=&PhysicalGop;return EFI_SUCCESS;}
    if(h==&ConsoleToken && guid_eq(g,&GopGuid)){*out=&ConsoleGop;return EFI_SUCCESS;}
    if(h==&GopToken && guid_eq(g,&PathGuid)){GopPath[16]=(UINT8)(DisplayChanged?3:1);*out=GopPath;return EFI_SUCCESS;}
    if(h==&ConsoleToken && guid_eq(g,&PathGuid)){*out=0;return EFI_UNSUPPORTED;}
    return handle(h,g,out);
}
static EFI_STATUS EFIAPI app_bar(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT8 bar,UINT64 off,UINTN n,VOID *v) {
    assert(p==&Devices[0] && w==EfiPciIoWidthUint32 && !bar);ReadCalls++;
    assert(DiskSize[3]);
    if(CardReadFail)return EFI_DEVICE_ERROR;
    if(off==0 && n==1){*(UINT32 *)v=0x162000a1;return EFI_SUCCESS;}
    assert(off>=0x300000 && off+n*4<=0x380000 && n==1024);
    memcpy(v,FullProm+off-0x300000,(size_t)n*4);
    if(RomChanged)((UINT8 *)v)[0]^=1;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI app_resources(EFI_PCI_IO_PROTOCOL *p,UINT8 bar,UINT64 *supports,VOID **r) {
    assert(p==&Devices[0] && !bar && !supports);
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=calloc(1,sizeof(*d)+2);assert(d);
    d->Desc=ACPI_ADDRESS_SPACE_DESCRIPTOR;d->Len=sizeof(*d)-3;
    d->ResType=ACPI_ADDRESS_SPACE_TYPE_MEM;d->AddrRangeMin=LIVE_BAR0;
    d->AddrLen=0x1000000;d->AddrRangeMax=LIVE_BAR0+d->AddrLen-1;
    ((UINT8 *)(d+1))[0]=ACPI_END_TAG_DESCRIPTOR;*r=d;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI app_key_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *s,BOOLEAN ext) {
    assert(s==&Input && !ext);return ResetFail?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI app_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *s,EFI_INPUT_KEY *k) {
    assert(s==&Input);KeyCalls++;
    if(Key<0)return EFI_NOT_READY;
    k->UnicodeChar=(CHAR16)Key;k->ScanCode=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI app_output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *s,CHAR16 *msg) {
    assert(s==&Output && msg);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI app_stall(UINTN us){assert(us==100000);WaitCalls++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI app_claim(EFI_HANDLE h,EFI_GUID ***ids,UINTN *n) {
    assert(h==&Devices[0]);*ids=0;*n=0;OwnerAttempts++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI app_open_protocol(EFI_HANDLE h,EFI_GUID *g,VOID **p,EFI_HANDLE a,EFI_HANDLE c,UINT32 f) {
    (void)h;(void)g;(void)p;(void)a;(void)c;(void)f;assert(!"refused owner check must precede open");abort();
}
static EFI_STATUS EFIAPI app_close_protocol(EFI_HANDLE h,EFI_GUID *g,EFI_HANDLE a,EFI_HANDLE c) {
    (void)h;(void)g;(void)a;(void)c;assert(!"unowned target must not be closed");abort();
}
static EFI_STATUS EFIAPI app_info(EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **r,UINTN *n) {
    (void)h;(void)g;(void)r;(void)n;assert(!"refused protocol enumeration");abort();
}
static EFI_TPL EFIAPI app_raise(EFI_TPL t){assert(t==TPL_NOTIFY);return TPL_APPLICATION;}
static VOID EFIAPI app_restore(EFI_TPL t){assert(t==TPL_APPLICATION);}
static void app_setup(void) {
    setup();memset(&Runtime,0,sizeof(Runtime));memset(&Input,0,sizeof(Input));memset(&Output,0,sizeof(Output));
    Runtime.GetVariable=app_variable;System.RuntimeServices=&Runtime;System.ConsoleOutHandle=&ConsoleToken;
    System.ConIn=&Input;System.ConOut=&Output;Input.Reset=app_key_reset;Input.ReadKeyStroke=app_key;
    Output.OutputString=app_output;
    memset(&PhysicalGop,0,sizeof(PhysicalGop));memset(&ConsoleGop,0,sizeof(ConsoleGop));
    memset(&PhysicalMode,0,sizeof(PhysicalMode));memset(&ConsoleMode,0,sizeof(ConsoleMode));
    memset(&PhysicalInfo,0,sizeof(PhysicalInfo));
    PhysicalInfo.HorizontalResolution=1920;PhysicalInfo.VerticalResolution=1080;
    PhysicalInfo.PixelsPerScanLine=1920;PhysicalInfo.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;
    PhysicalMode.FrameBufferBase=0xc0000000;PhysicalMode.FrameBufferSize=1920*1080*4;
    PhysicalMode.Info=&PhysicalInfo;PhysicalMode.SizeOfInfo=sizeof(PhysicalInfo);
    ConsoleInfo=PhysicalInfo;ConsoleMode=PhysicalMode;ConsoleMode.Info=&ConsoleInfo;
    PhysicalGop.Mode=&PhysicalMode;ConsoleGop.Mode=&ConsoleMode;
    Services.LocateHandleBuffer=app_locate;Services.HandleProtocol=app_handle;Services.Stall=app_stall;
    Services.ProtocolsPerHandle=app_claim;Services.OpenProtocol=app_open_protocol;
    Services.CloseProtocol=app_close_protocol;Services.OpenProtocolInformation=app_info;
    Services.RaiseTPL=app_raise;Services.RestoreTPL=app_restore;
    Config[0][1]=3;Config[0][4]=(UINT32)LIVE_BAR0;
    Devices[0].Mem.Read=app_bar;Devices[0].GetBarAttributes=app_resources;
    Secure=RomChanged=CardReadFail=DisplayChanged=ResetFail=OwnerAttempts=0;
    Key='U';ReadCalls=KeyCalls=WaitCalls=0;
}
#ifndef TU102_APP_TEST_MAIN
#define TU102_APP_TEST_MAIN main
#endif
static void app_inactive_console(void) {
    /* Replay the capture08 zero framebuffer with the additional uninitialized
     * fields required by r3. Those fields remain a runtime hardware gate. */
    System.ConsoleOutHandle=&ImageToken;
    ConsoleMode.FrameBufferBase=0;ConsoleMode.FrameBufferSize=0;
    ConsoleMode.Info=0;ConsoleMode.SizeOfInfo=0;ConsoleMode.MaxMode=0;
}
static void app_capture09_console(void) {
    app_inactive_console();ConsoleMode.Info=&ConsoleInfo;
    ConsoleMode.MaxMode=14;ConsoleMode.Mode=2;
    PhysicalMode.MaxMode=14;PhysicalMode.Mode=3;
    PhysicalMode.FrameBufferBase=0xf800000000ULL;PhysicalMode.FrameBufferSize=0x300000;
    PhysicalInfo.HorizontalResolution=1024;PhysicalInfo.VerticalResolution=768;
    PhysicalInfo.PixelsPerScanLine=1024;
}
int TU102_APP_TEST_MAIN(void) {
    FILE *f=fopen("captures/capture-04/LIVE-PROM.BIN","rb");assert(f);
    assert(fread(FullProm,1,sizeof(FullProm),f)==sizeof(FullProm) && fgetc(f)==EOF);fclose(f);
    unsigned tests=0;
    app_setup();assert(experiment_assets()==EFI_SUCCESS);tests++;
    app_setup();
#ifdef TU102_ENABLE_UNVERIFIED_EXECUTION
    assert(efi_main(&ImageToken,&System)==EFI_ACCESS_DENIED && OwnerAttempts==1 && !Port.tx.mutated);tests++;
    app_setup();Key=27;assert(efi_main(&ImageToken,&System)==EFI_ABORTED && !OwnerAttempts);tests++;
    app_setup();Key=-1;assert(efi_main(&ImageToken,&System)==EFI_TIMEOUT && WaitCalls==300 && !OwnerAttempts);tests++;
    app_setup();ResetFail=1;assert(efi_main(&ImageToken,&System)==EFI_DEVICE_ERROR && !OwnerAttempts);tests++;
#else
    assert(efi_main(&ImageToken,&System)==EFI_UNSUPPORTED && !KeyCalls && !OwnerAttempts && !Port.tx.mutated);tests++;
#endif
    app_setup();HasTag=0;assert(efi_main(&ImageToken,&System)==EFI_ACCESS_DENIED && !ReadCalls);tests++;
    app_setup();IsRemovable=0;assert(efi_main(&ImageToken,&System)==EFI_UNSUPPORTED && !ReadCalls);tests++;
    app_setup();Secure=1;assert(efi_main(&ImageToken,&System)==EFI_SECURITY_VIOLATION && !ReadCalls);tests++;
    app_setup();WrongLocation=1;assert(efi_main(&ImageToken,&System)==EFI_NOT_FOUND && !ReadCalls);tests++;
    app_setup();Config[0][11]^=1;assert(efi_main(&ImageToken,&System)==EFI_NOT_FOUND && !ReadCalls);tests++;
    app_setup();Config[0][1]=7;assert(efi_main(&ImageToken,&System)==EFI_NOT_FOUND && !ReadCalls);tests++;
    app_setup();DisplayChanged=1;assert(efi_main(&ImageToken,&System)==EFI_UNSUPPORTED && !ReadCalls);tests++;
    app_setup();RomChanged=1;assert(efi_main(&ImageToken,&System)==EFI_SECURITY_VIOLATION && !OwnerAttempts);tests++;
    app_setup();CardReadFail=1;assert(efi_main(&ImageToken,&System)==EFI_UNSUPPORTED && !OwnerAttempts);tests++;
    app_setup();assert(efi_main(0,0)==EFI_INVALID_PARAMETER);tests++;
    printf("PASS %u actual %s EFI-entry refusal/cancel tests, real PROM/assets; no hardware\n",tests,
#ifdef TU102_ENABLE_UNVERIFIED_EXECUTION
        "experimental"
#else
        "default-disabled"
#endif
    );
    return 0;
}
