/* Actual display guard; unknown aliases must never be accepted by guessing. */
#define TU102_APP_TEST_MAIN retained_app_tests
#include "test_app.c"
static int OtherConsole,ExtraGop;
static int EmptyAlias,ExtraPhysical,PathError,MissingGop,TooMany;
static int StubPciPresent,StubPciError,ExtraInactive;
static int UnrelatedText,ParentLink,TextError;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL OtherOutput;
static int SplitterAgent,OtherAgent;
static EFI_STATUS EFIAPI parent_protocols(EFI_HANDLE h,EFI_GUID ***ids,UINTN *n) {
    assert(h==&GopToken);*ids=malloc(sizeof(**ids));assert(*ids);(*ids)[0]=&TdTextOutGuid;*n=1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI parent_info(EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *n) {
    assert(h==&GopToken && guid_eq(g,&TdTextOutGuid));*out=0;*n=0;
    if(ParentLink==4)return EFI_DEVICE_ERROR;
    if(!ParentLink)return EFI_SUCCESS;
    *out=calloc(2,sizeof(**out));assert(*out);*n=2;
    (*out)[0].AgentHandle=&SplitterAgent;(*out)[0].Attributes=ParentLink==5?EFI_OPEN_PROTOCOL_GET_PROTOCOL:EFI_OPEN_PROTOCOL_BY_DRIVER;
    (*out)[1].AgentHandle=ParentLink==2?&OtherAgent:&SplitterAgent;
    (*out)[1].ControllerHandle=ParentLink==3?&OtherConsole:&ConsoleToken;
    (*out)[1].Attributes=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI display_handles(EFI_LOCATE_SEARCH_TYPE t,EFI_GUID *g,VOID *k,UINTN *n,EFI_HANDLE **out) {
    assert(guid_eq(g,&GopGuid));
    if(TooMany){*n=17;*out=calloc(17,sizeof(**out));assert(*out);return EFI_SUCCESS;}
    EFI_STATUS st=app_locate(t,g,k,n,out);
    if(ExtraPhysical || ExtraInactive){free(*out);*n=3;*out=malloc(3*sizeof(**out));assert(*out);(*out)[0]=&GopToken;(*out)[1]=&ConsoleToken;(*out)[2]=&ExtraGop;}
    return st;
}
static EFI_STATUS EFIAPI display_handle(EFI_HANDLE h,EFI_GUID *g,VOID **out) {
    if(h==&ConsoleToken && guid_eq(g,&TdTextOutGuid)){
        *out=TextError?0:UnrelatedText?&OtherOutput:&Output;return TextError?EFI_DEVICE_ERROR:EFI_SUCCESS;
    }
    if(h==&ExtraGop)return app_handle(ExtraInactive?&ConsoleToken:&GopToken,g,out);
    if(h==&ConsoleToken && guid_eq(g,&PciGuid)) {
        *out=StubPciPresent?&Devices[0]:0;
        return StubPciError?EFI_DEVICE_ERROR:StubPciPresent?EFI_SUCCESS:EFI_NOT_FOUND;
    }
    if(h==&ConsoleToken && guid_eq(g,&PathGuid)) {
        if(PathError){*out=0;return EFI_DEVICE_ERROR;}
        if(EmptyAlias){*out=&EndPath;return EFI_SUCCESS;}
    }
    if(h==&ConsoleToken && guid_eq(g,&GopGuid) && MissingGop){*out=0;return EFI_UNSUPPORTED;}
    return app_handle(h,g,out);
}
static void display_setup(void) {
    app_setup();TxSystem=&System;Used=0;LogFault=0;Report[0]=0;
    memset(&Port,0,sizeof(Port));Port.bs=&Services;
    Services.HandleProtocol=display_handle;Services.LocateHandleBuffer=display_handles;
    EmptyAlias=ExtraPhysical=PathError=MissingGop=TooMany=0;
    StubPciPresent=StubPciError=ExtraInactive=0;
    UnrelatedText=ParentLink=TextError=0;
    Services.ProtocolsPerHandle=parent_protocols;Services.OpenProtocolInformation=parent_info;
}
static EFI_STATUS check(void){return experiment_display(&Port);}
int main(void) {
    unsigned tests=0;
    display_setup();assert(check()==EFI_SUCCESS);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;assert(check()==EFI_SUCCESS && strstr(Report,"identical_5070Ti_framebuffer"));tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;EmptyAlias=1;assert(check()==EFI_SUCCESS);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;ConsoleMode.FrameBufferBase+=4096;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;ConsoleMode.FrameBufferSize-=4;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;ConsoleMode.FrameBufferBase=0;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;ConsoleInfo.PixelFormat=PixelBltOnly;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;ConsoleInfo.HorizontalResolution--;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;ConsoleInfo.PixelsPerScanLine=1;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;ConsoleInfo.PixelInformation.RedMask=1;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;MissingGop=1;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();System.ConsoleOutHandle=&OtherConsole;ConsoleMode.SizeOfInfo=0;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();PathError=1;assert(check()==EFI_DEVICE_ERROR);tests++;
    display_setup();ExtraPhysical=1;assert(check()==EFI_UNSUPPORTED && strstr(Report,"physical_display_count"));tests++;
    display_setup();TooMany=1;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();DisplayChanged=1;assert(check()==EFI_UNSUPPORTED && strstr(Report,"unrecognized_physical_path"));tests++;
    display_setup();app_inactive_console();assert(check()==EFI_SUCCESS && strstr(Report,"verified_virtual_console"));tests++;
    display_setup();app_inactive_console();StubPciPresent=1;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_inactive_console();StubPciError=1;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_inactive_console();ConsoleMode.Info=&ConsoleInfo;assert(check()==EFI_SUCCESS);tests++;
    display_setup();app_inactive_console();ConsoleMode.SizeOfInfo=sizeof(ConsoleInfo);assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_inactive_console();ConsoleMode.FrameBufferBase=4096;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_inactive_console();PhysicalMode.FrameBufferBase=0;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_inactive_console();ExtraInactive=1;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_capture09_console();assert(check()==EFI_SUCCESS && strstr(Report,"current_text_output_protocol"));tests++;
    display_setup();app_capture09_console();UnrelatedText=1;ParentLink=1;assert(check()==EFI_SUCCESS && strstr(Report,"physical_console_driver_child"));tests++;
    display_setup();app_capture09_console();UnrelatedText=1;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_capture09_console();UnrelatedText=1;ParentLink=2;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_capture09_console();UnrelatedText=1;ParentLink=3;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_capture09_console();UnrelatedText=1;ParentLink=4;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_capture09_console();TextError=1;assert(check()==EFI_UNSUPPORTED);tests++;
    display_setup();app_capture09_console();ConsoleMode.Info=(VOID *)(UINTN)1;assert(check()==EFI_SUCCESS);tests++;
    display_setup();app_capture09_console();UnrelatedText=1;ParentLink=5;assert(check()==EFI_UNSUPPORTED);tests++;
    for(unsigned i=0;i<36;i++) {
        display_setup();UINT8 original=GopPath[i];GopPath[i]^=0x80;
        assert(!td_known_path((VOID *)GopPath));GopPath[i]=original;tests++;
    }
    assert(!ReadCalls && !OwnerAttempts && !Port.tx.mutated);
    printf("PASS %u display topology/alias/invalid-path cases; no GPU writes or DMA\n",tests);
    return 0;
}
