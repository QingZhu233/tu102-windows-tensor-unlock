/* Automatic actual-entry tests: all filesystem, CPU and GPU services mocked. */
#define TU102_AUTOMATIC_INTERNAL
#define TU102_AMD_TEST_MAIN retained_amd_suite
#include "test_amd_direct.c"
static UINT8 AutoPath[46];
static int MissingMarker,BadMarker,InfoFailure,TruncateFailure;
static unsigned Truncations;
static UINT8 RecoveryBootOption[300];
static int BootNextPresent,FailBootNextWrite,WrongRecoveryOption,FailRecoveryWatchdog,ReturnWindowsLoader;
static unsigned AutoResets,SafeFallbacks,RecoveryWrites,WatchdogSeconds;
static unsigned ScreenCalls;
static int FailNextDelete,FailWatchdogStop;
static EFI_STATUS EFIAPI auto_output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *s,CHAR16 *message) {
    assert(s==&Output && message);ScreenCalls++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI auto_variable(CHAR16 *name,EFI_GUID *guid,UINT32 *attrs,UINTN *n,VOID *out) {
    if(eq16(name,L"SecureBoot"))return app_variable(name,guid,attrs,n,out);
    assert(guid_eq(guid,&ArGlobal));
    if(eq16(name,L"Boot0000")) {
        if(*n<300){*n=300;return EFI_BUFFER_TOO_SMALL;}
        memcpy(out,RecoveryBootOption,300);if(WrongRecoveryOption)((UINT8 *)out)[0]^=1;
        *n=300;if(attrs)*attrs=7;return EFI_SUCCESS;
    }
    assert(eq16(name,L"BootNext"));
    if(!BootNextPresent)return EFI_NOT_FOUND;
    assert(*n>=2);*(UINT16 *)out=BootNextPresent==2?1:0;*n=2;if(attrs)*attrs=7;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI auto_set_variable(CHAR16 *name,EFI_GUID *guid,UINT32 attrs,UINTN n,VOID *data) {
    assert(guid_eq(guid,&ArGlobal) && eq16(name,L"BootNext"));
    if(FailBootNextWrite || (FailNextDelete && !n))return EFI_WRITE_PROTECTED;
    if(n){assert(n==2 && attrs==7 && data && !*(UINT16 *)data);BootNextPresent=1;}
    else{assert(!attrs && !data);BootNextPresent=0;}
    RecoveryWrites++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI auto_watchdog(UINTN timeout,UINT64 code,UINTN n,CHAR16 *data) {
    assert((timeout==120 || !timeout) && !code && !n && !data);
    if(FailRecoveryWatchdog && timeout)return EFI_UNSUPPORTED;
    if(FailWatchdogStop && !timeout)return EFI_DEVICE_ERROR;
    WatchdogSeconds=(unsigned)timeout;return EFI_SUCCESS;
}
static VOID EFIAPI auto_reset(EFI_RESET_TYPE type,EFI_STATUS status,UINTN n,VOID *data) {
    assert(type==EfiResetCold && status==EFI_ABORTED && !n && !data && BootNextPresent==1);
    AutoResets++;tu102_host_stop();
}
static EFI_STATUS EFIAPI auto_start(EFI_HANDLE h,UINTN *n,CHAR16 **data) {
    assert(h==&WinImage && !n && !data);WinStart++;
    if(Port.tx.unlocked)assert(Port.tx.quiescent && Port.released && !Port.pool && !ArOwnNext && !ArWatchdog && !WatchdogSeconds && !ScreenCalls);
    else {assert(!model.writes && !DriverCalls && ScreenCalls);SafeFallbacks++;}
    if(ReturnWindowsLoader)return EFI_ABORTED;
    tu102_host_stop();return EFI_ABORTED;
}
static EFI_STATUS EFIAPI auto_handle(EFI_HANDLE h,EFI_GUID *g,VOID **out) {
    if(h==&VolumeToken && guid_eq(g,&PathGuid)){*out=AutoPath;return EFI_SUCCESS;}
    if(h==&VolumeToken && guid_eq(g,&BlockGuid)){*out=&Block;return EFI_SUCCESS;}
    return amd_handle(h,g,out);
}
static EFI_STATUS EFIAPI auto_open(EFI_FILE_PROTOCOL *self,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 mode,UINT64 attr) {
    assert(!attr);*out=0;
    if(self==&Root){assert(eq16(name,L"\\EFI\\TU102Unlock") && mode==3);*out=&Dir;return EFI_SUCCESS;}
    if(self==&Dir && eq16(name,L"AUTO.ENABLED")) {
        assert(mode==1);if(MissingMarker)return EFI_NOT_FOUND;*out=&Tag;return EFI_SUCCESS;
    }
    assert(self==&Dir && eq16(name,L"TRANSACTION.TXT") && mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE));*out=&Files[3];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI auto_read(EFI_FILE_PROTOCOL *self,UINTN *n,VOID *out) {
    static const char token[]="TU102-CAPTURE21-LOCAL-AUTO-269bc7cdbeb7e86a49d311d3795108b962ecf5bcee17267447353bc0575cae5a\n";
    assert(self==&Tag && *n>=sizeof(token));memcpy(out,token,sizeof(token)-1);*n=sizeof(token)-1;
    if(BadMarker)((char *)out)[0]^=1;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI auto_info(EFI_FILE_PROTOCOL *self,EFI_GUID *g,UINTN *n,VOID *out) {
    assert(self==&Files[3] && guid_eq(g,&TaInfoGuid));
    if(InfoFailure)return EFI_DEVICE_ERROR;
    assert(*n>=sizeof(EFI_FILE_INFO));memset(out,0,sizeof(EFI_FILE_INFO));
    EFI_FILE_INFO *info=out;info->Size=sizeof(*info);info->FileSize=20000;*n=sizeof(*info);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI auto_set_info(EFI_FILE_PROTOCOL *self,EFI_GUID *g,UINTN n,VOID *out) {
    assert(self==&Files[3] && guid_eq(g,&TaInfoGuid) && n==sizeof(EFI_FILE_INFO) && !((EFI_FILE_INFO *)out)->FileSize);
    if(TruncateFailure)return EFI_VOLUME_FULL;
    DiskSize[3]=0;Truncations++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI auto_position(EFI_FILE_PROTOCOL *self,UINT64 n){assert(self==&Files[3] && !n);return EFI_SUCCESS;}
static void auto_setup(void) {
    amd_setup();Services.HandleProtocol=auto_handle;Key=-1;
    memset(AutoPath,0,sizeof(AutoPath));AutoPath[0]=4;AutoPath[1]=1;AutoPath[2]=42;
    tx_put32(AutoPath+4,1);put64(AutoPath+8,2048);put64(AutoPath+16,409600);
    const UINT8 sig[]=TU102_LOCAL_ESP_SIGNATURE;
    memcpy(AutoPath+24,sig,16);AutoPath[40]=AutoPath[41]=2;AutoPath[42]=0x7f;AutoPath[43]=0xff;AutoPath[44]=4;
    Media.RemovableMedia=FALSE;Media.LogicalPartition=TRUE;Media.BlockSize=512;Media.LastBlock=409599;
    Root.Open=Dir.Open=auto_open;Tag.Read=auto_read;
    Files[3].GetInfo=auto_info;Files[3].SetInfo=auto_set_info;Files[3].SetPosition=auto_position;
    MissingMarker=BadMarker=InfoFailure=TruncateFailure=0;Truncations=0;
    Runtime.GetVariable=auto_variable;Runtime.SetVariable=auto_set_variable;Runtime.ResetSystem=auto_reset;
    Services.SetWatchdogTimer=auto_watchdog;Services.StartImage=auto_start;
    load("build/transaction/automatic-platform/WINDOWS-BOOT0000.bin",RecoveryBootOption,sizeof(RecoveryBootOption));
    BootNextPresent=FailBootNextWrite=WrongRecoveryOption=FailRecoveryWatchdog=ReturnWindowsLoader=0;
    AutoResets=SafeFallbacks=RecoveryWrites=WatchdogSeconds=0;
    ScreenCalls=0;FailNextDelete=FailWatchdogStop=0;Output.OutputString=auto_output;
}
static void auto_fallback_at(const char *step) {
    assert(run_to_stop() && WinStart==1 && SafeFallbacks==1 && !DriverCalls && !model.writes && !AutoResets);
    assert(!strcmp(Step,step));
}
int main(void) {
    load("captures/capture-04/NATIVE-FWSEC.BIN",native,sizeof(native));
    load("assets/upstream-cmp50hx-efi/bl_gsp_tu102.bin",generic,sizeof(generic));
    load("assets/upstream-cmp50hx-efi/booter_ucode_prod.bin",booter,sizeof(booter));
    load("assets/upstream-cmp50hx-efi/v67_payload.bin",signature,sizeof(signature));
    load("assets/upstream-cmp50hx-efi/gsp_rm_boot_dbg.bin",boot,sizeof(boot));
    load("captures/capture-04/LIVE-PROM.BIN",FullProm,sizeof(FullProm));
    unsigned cases=0;
    auto_setup();assert(run_to_stop() && WinStart==1 && DirectFrees==1 && Port.tx.unlocked && Truncations==1 && !KeyCalls);cases++;
    auto_setup();Key=27;ResetFail=1;assert(run_to_stop() && WinStart==1 && Port.tx.unlocked && !KeyCalls);cases++;
    auto_setup();MissingMarker=1;auto_fallback_at("internal-log-volume");cases++;
    auto_setup();BadMarker=1;auto_fallback_at("internal-log-volume");cases++;
    auto_setup();Media.RemovableMedia=TRUE;auto_fallback_at("internal-log-volume");cases++;
    auto_setup();Media.ReadOnly=TRUE;auto_fallback_at("internal-log-volume");cases++;
    auto_setup();Media.LastBlock--;auto_fallback_at("internal-log-volume");cases++;
    auto_setup();AutoPath[24]^=1;auto_fallback_at("internal-log-volume");cases++;
    auto_setup();AutoPath[8]^=1;auto_fallback_at("internal-log-volume");cases++;
    auto_setup();AutoPath[2]=0;auto_fallback_at("internal-log-volume");cases++;
    auto_setup();InfoFailure=1;auto_fallback_at("internal-log-truncate");cases++;
    auto_setup();TruncateFailure=1;auto_fallback_at("internal-log-truncate");cases++;
    auto_setup();TranslationEnabled=1;auto_fallback_at("AMD-direct-read-only-environment");cases++;
    auto_setup();WrongEFR=1;auto_fallback_at("AMD-direct-read-only-environment");cases++;
    auto_setup();model.fail_write=1;assert(run_to_stop() && AutoResets==1 && BootNextPresent==1 && !WinStart && Port.retained && !DirectFrees);cases++;
    auto_setup();StopChangesCommand=2;assert(run_to_stop() && DecodeRestores==1 && WinStart==1);cases++;
    auto_setup();System.ConIn=0;assert(ta_arm(&System)==EFI_SUCCESS);cases++;
    auto_setup();WrongRecoveryOption=1;auto_fallback_at("driver-release");assert(!RecoveryWrites);cases++;
    auto_setup();BootNextPresent=2;auto_fallback_at("driver-release");assert(BootNextPresent==2 && !RecoveryWrites);cases++;
    auto_setup();FailBootNextWrite=1;auto_fallback_at("driver-release");assert(!WatchdogSeconds);cases++;
    auto_setup();FailRecoveryWatchdog=1;auto_fallback_at("driver-release");assert(!RecoveryWrites);cases++;
    auto_setup();PageAllocationFail=1;assert(run_to_stop() && AutoResets==1 && BootNextPresent==1 && !model.writes && !WinStart);cases++;
    auto_setup();EnableTranslationAtPhase2=1;assert(run_to_stop() && AutoResets==1 && !model.booter_starts && !WinStart);cases++;
    auto_setup();ReturnWindowsLoader=1;assert(run_to_stop() && WinStart==1 && AutoResets==1 && BootNextPresent==1);cases++;
    auto_setup();Runtime.ResetSystem=0;auto_fallback_at("driver-release");cases++;
    auto_setup();NoWindowsLoader=1;assert(efi_main(&ImageToken,&System)==EFI_NOT_FOUND && !WinStart && !DriverCalls && !RecoveryWrites && ScreenCalls);cases++;
    auto_setup();MissingMarker=1;ReturnWindowsLoader=1;assert(efi_main(&ImageToken,&System)==EFI_ABORTED && WinStart==1 && !AutoResets && !model.writes && ScreenCalls);cases++;
    auto_setup();FailNextDelete=1;assert(run_to_stop() && AutoResets==1 && BootNextPresent==1 && !WinStart && Port.tx.quiescent && ScreenCalls);cases++;
    auto_setup();FailWatchdogStop=1;assert(run_to_stop() && AutoResets==1 && BootNextPresent==1 && !WinStart && ScreenCalls);cases++;
    printf("PASS %u automatic no-keyboard/error-to-Windows/one-shot-reset/watchdog/core containment mock cases; no hardware\n",cases);
    return 0;
}
