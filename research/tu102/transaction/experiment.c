/* LOCAL, unverified TU102 Windows experiment; copyleft-next-0.3.1.
 * No installer and no automatic execution. Default artifact cannot arm.
 */
#define TU102_LIVE_PROM_CAPTURE
#define efi_main retained_capture_entry
#include "../preflight.c"
#undef efi_main
#define TX_EXPERIMENT_V4
#define TX_ENTRY_LATCHED_LOCAL
#include "efi_dma.h"
#include "handoff.h"
#include "../../../unlock/sha256.h"
#include "../build/transaction_assets.h"

static EFI_FILE_PROTOCOL *TxLog;
static UINTN TxWritten;
static EFI_SYSTEM_TABLE *TxSystem;
static TuEfi Port;
static const char *Step;
#include "owner_release.h"
#ifdef TU102_AMD_DIRECT_PHYSICAL
#include "amd_direct.h"
#endif

static EFI_STATUS experiment_flush(void) {
    if(!TxLog || LogFault || TxWritten>Used)return EFI_DEVICE_ERROR;
    UINTN bytes=Used-TxWritten,actual=bytes;
    EFI_STATUS st=EFI_SUCCESS;
    if(bytes)st=TxLog->Write(TxLog,&actual,Report+TxWritten);
    if(EFI_ERROR(st))return st;
    if(actual!=bytes)return EFI_DEVICE_ERROR;
    TxWritten=Used;
    return TxLog->Flush(TxLog);
}
static int experiment_event(void *ctx,unsigned stage,uint32_t a,uint32_t b) {
    (void)ctx;field("transaction_stage",stage);field("stage_a",a);field("stage_b",b);
    return EFI_ERROR(experiment_flush());
}
static int experiment_guard(void *ctx,uint32_t off,uint32_t want,uint32_t got,uint32_t mask) {
    (void)ctx;
    if((got&mask)!=(want&mask)) {
        field("guard_failed_offset",off);field("guard_expected",want);field("guard_actual",got);
    }
    return LogFault;
}
#ifdef TU102_AUTOMATIC_INTERNAL
static VOID ar_reset_recovery(void);
#endif
static VOID experiment_stop(void) {
#ifdef TU102_AUTOMATIC_INTERNAL
    ar_reset_recovery();
#else
    text("STOP: uncertain state or failed cleanup. Do not continue into Windows. Fully power off.\r\n");
    (void)experiment_flush();
    if(TxSystem->ConOut && TxSystem->ConOut->OutputString)
        TxSystem->ConOut->OutputString(TxSystem->ConOut,
            L"TU102 EXPERIMENT STOPPED. Fully power off. Do not continue to Windows or retry.\r\n");
#ifdef TU102_HOST_TEST
#if __STDC_HOSTED__ != 1
#error TU102_HOST_TEST is forbidden in firmware builds
#endif
    extern void tu102_host_stop(void);
    tu102_host_stop(); /* test harness escapes via longjmp, never a real boot */
#endif
    for(;;)TxSystem->BootServices->Stall(1000000);
#endif
}
static EFI_STATUS experiment_find(TuEfi *p) {
    EFI_HANDLE *handles=0;UINTN count=0,matches=0;
    EFI_STATUS st=p->bs->LocateHandleBuffer(ByProtocol,&p->pci_guid,0,&count,&handles);
    if(EFI_ERROR(st))return st;
    if(!handles || count>4096){if(handles)p->bs->FreePool(handles);return EFI_UNSUPPORTED;}
    for(UINTN i=0;i<count;i++) {
        EFI_PCI_IO_PROTOCOL *pci=0;
        st=p->bs->HandleProtocol(handles[i],&p->pci_guid,(VOID **)&pci);
        if(EFI_ERROR(st) || !pci)continue;
        TuEfi probe={0};probe.pci=pci;
        if(te_identity(&probe,3)==EFI_SUCCESS){matches++;p->pci=pci;p->handle=handles[i];}
    }
    st=p->bs->FreePool(handles);
    if(EFI_ERROR(st))return st;
    return matches==1?EFI_SUCCESS:EFI_NOT_FOUND;
}
static EFI_STATUS experiment_prom(TuEfi *p) {
    EFI_STATUS st=live_identity(p->pci);
    if(!EFI_ERROR(st))st=live_bounds(p->bs,p->pci);
    if(EFI_ERROR(st))return st;
    UINT32 boot0=0;
    st=p->pci->Mem.Read(p->pci,EfiPciIoWidthUint32,0,0,1,&boot0);
    if(EFI_ERROR(st) || boot0!=0x162000a1)return EFI_UNSUPPORTED;
    VOID *prom=0;
    st=p->bs->AllocatePool(EfiLoaderData,0x80000,&prom);
    if(EFI_ERROR(st) || !prom)return EFI_OUT_OF_RESOURCES;
    for(UINTN off=0;off<0x80000;off+=4096) {
        st=p->pci->Mem.Read(p->pci,EfiPciIoWidthUint32,0,0x300000+off,1024,(UINT8 *)prom+off);
        if(EFI_ERROR(st))break;
    }
    BOOLEAN matched=!EFI_ERROR(st) && GuHashMatches(prom,0x80000,
        "d88ee7d835cb5be26b9c606ae2a6ae51337537fc46c146d777eed0291d431a43");
    EFI_STATUS freed=p->bs->FreePool(prom);
    if(EFI_ERROR(st))return st;
    if(EFI_ERROR(freed))return freed;
    field("full_PROM_matched",matched);
    return matched?EFI_SUCCESS:EFI_SECURITY_VIOLATION;
}
#include "display.h"

static void experiment_screen_ascii(EFI_SYSTEM_TABLE *s,const char *message) {
    if(!s->ConOut || !s->ConOut->OutputString)return;
    CHAR16 line[96];UINTN n=0;
    while(*message && n<95)line[n++]=(UINT8)*message++;
    line[n]=0;s->ConOut->OutputString(s->ConOut,line);
}
static EFI_STATUS experiment_assets(void) {
    if(sizeof(asset_native)!=TX_FWSEC_SIZE || sizeof(asset_generic)!=TX_GENERIC_SIZE ||
       sizeof(asset_booter)!=TX_BOOTER_SIZE || sizeof(asset_signature)!=TX_SIGNATURE_SIZE ||
       sizeof(asset_boot)!=TX_BOOT_CONTAINER_SIZE)return EFI_COMPROMISED_DATA;
    if(!GuHashMatches(asset_native,sizeof(asset_native),"1516eb686e3b4a379f9c5739720b4ebf938700c2b5bf65686006ccd3999e2227") ||
       !GuHashMatches(asset_generic,sizeof(asset_generic),"f21f1cfbb8fffa8067685314367fc4c50f3f84dd99eda7c19db4292d9269b9d9") ||
       !GuHashMatches(asset_booter,sizeof(asset_booter),"e0f0fc93da097b5b68e5af3adb58b2235ed1fad5fce0b23592d3895af44cb743") ||
       !GuHashMatches(asset_signature,sizeof(asset_signature),"de7288bcfb80c2d4b259a59e823c926ba0a9adcab594219cc6802c94257ce271") ||
       !GuHashMatches(asset_boot,sizeof(asset_boot),"fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471"))
        return EFI_SECURITY_VIOLATION;
    return EFI_SUCCESS;
}
/* Automatic mode changes the entry/own-log policy only; the core is shared. */
#ifdef TU102_AUTOMATIC_INTERNAL
#include "internal_auto.h"
#include "automatic_recovery.h"
#endif
static EFI_STATUS experiment_watchdog(void) {
#ifdef TU102_AUTOMATIC_INTERNAL
    return ar_prepare(TxSystem);
#else
    return Port.bs->SetWatchdogTimer(0,0,0,0);
#endif
}
static EFI_STATUS experiment_arm(EFI_SYSTEM_TABLE *system) {
#ifdef TU102_AUTOMATIC_INTERNAL
    return ta_arm(system);
#else
#ifndef TU102_ENABLE_UNVERIFIED_EXECUTION
    (void)system;
    text("DEFAULT BUILD: transaction disabled. No GPU writes or DMA.\r\n");
    return EFI_UNSUPPORTED;
#else
    if(!system->ConIn || !system->ConIn->ReadKeyStroke || !system->ConIn->Reset ||
       !system->ConOut || !system->ConOut->OutputString)return EFI_UNSUPPORTED;
    EFI_STATUS st=system->ConIn->Reset(system->ConIn,FALSE);
    if(EFI_ERROR(st))return st;
    system->ConOut->OutputString(system->ConOut,
        L"UNVERIFIED TU102 22GiB EXPERIMENT. GPU firmware and DMA can hang/corrupt the host.\r\n"
#ifdef TU102_AMD_DIRECT_PHYSICAL
        L"AMD DIRECT PHYSICAL DMA: UEFI Map is NOT used. This profile is hardware-unverified.\r\n"
#endif
        L"U may release this2080Ti's nvgop driver and restore only cleared IO/MEM decode.\r\n"
        L"Press U once to attempt. Esc or 30 seconds cancels. No retry/reset/flash.\r\n");
    for(UINTN i=0;i<300;i++) {
        EFI_INPUT_KEY key={0};
        st=system->ConIn->ReadKeyStroke(system->ConIn,&key);
        if(!EFI_ERROR(st)) {
            if(key.ScanCode==0x17 || key.UnicodeChar==27)return EFI_ABORTED;
            if(key.UnicodeChar=='U' || key.UnicodeChar=='u')return EFI_SUCCESS;
        } else if(st!=EFI_NOT_READY)return st;
        st=system->BootServices->Stall(100000);if(EFI_ERROR(st))return st;
    }
    return EFI_TIMEOUT;
#endif
#endif
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *system) {
    if(!system || !system->BootServices || !system->RuntimeServices)return EFI_INVALID_PARAMETER;
    TxSystem=system;TxLog=0;TxWritten=0;Used=0;LogFault=0;Step="startup";
    zero_bytes(Report,sizeof(Report));zero_bytes(&Port,sizeof(Port));
#ifdef TU102_AUTOMATIC_INTERNAL
    ar_init();
#endif
    Port.bs=system->BootServices;Port.image=image;Port.pci_guid=PciGuid;
    Port.log_stage=experiment_event;te_bind(&Port);Port.tx.guard_note=experiment_guard;
    Port.tx.native_fwsec=asset_native;Port.tx.generic_bl=asset_generic;
    EFI_FILE_PROTOCOL *dir=0;TuWindows windows={0};TuReleaseOwner pending={0};
    BOOLEAN release_ready=FALSE,release_attempted=FALSE;
    EFI_STATUS st;
#ifdef TU102_AUTOMATIC_INTERNAL
    st=ta_open(image,Port.bs,&dir);
#else
    st=open_capture(image,Port.bs,&dir);
#endif
    if(EFI_ERROR(st)){
#ifdef TU102_AUTOMATIC_INTERNAL
        Step="internal-log-volume";return ar_fallback(image,system,&windows,st);
#else
        return st;
#endif
    }
    st=dir->Open(dir,&TxLog,L"TRANSACTION.TXT",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    if(EFI_ERROR(st) || !TxLog){
        dir->Close(dir);
#ifdef TU102_AUTOMATIC_INTERNAL
        Step="internal-log-open";return ar_fallback(image,system,&windows,EFI_DEVICE_ERROR);
#else
        return EFI_DEVICE_ERROR;
#endif
    }
#ifdef TU102_AUTOMATIC_INTERNAL
    st=ta_truncate(TxLog);
    if(EFI_ERROR(st)){TxLog->Close(TxLog);TxLog=0;dir->Close(dir);Step="internal-log-truncate";return ar_fallback(image,system,&windows,st);}
    text("LOCAL TU102 AUTOMATIC: core measured successful in CAPTURE21; automatic entry unverified.\r\n");
#else
    text("LOCAL TU102 WINDOWS EXPERIMENT: hardware unverified; no automatic installer\r\n");
#endif
    st=experiment_flush();if(EFI_ERROR(st))goto finish;
    UINT8 secure=0xff;UINTN size=1;
    EFI_GUID global={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
    st=system->RuntimeServices->GetVariable(L"SecureBoot",&global,0,&size,&secure);
    if(EFI_ERROR(st) || size!=1 || secure){st=EFI_SECURITY_VIOLATION;goto finish;}
    Step="assets";st=experiment_assets();if(EFI_ERROR(st))goto finish;
    Step="target";st=experiment_find(&Port);if(EFI_ERROR(st))goto finish;
    Step="display";st=experiment_display(&Port);if(EFI_ERROR(st))goto finish;
    Step="PROM";st=experiment_prom(&Port);if(EFI_ERROR(st))goto finish;
#ifdef TU102_AMD_DIRECT_PHYSICAL
    Port.profile_verified=TRUE;
    Step="AMD-direct-read-only-environment";
    text("DIRECT PHYSICAL DMA experimental profile: not UEFI-mapped, not guaranteed safe.\r\n");
    st=da_platform(&Port,system);if(EFI_ERROR(st))goto finish;
    Port.tx.next_input=da_next_input;
#endif
    Step="manual-U";st=experiment_arm(system);if(EFI_ERROR(st))goto finish;
    Port.profile_verified=TRUE;
    Step="ownership";st=te_claim(&Port);
    if(st==EFI_ACCESS_DENIED) {
        Step="release-candidate";st=tr_candidate(&Port,system,&pending,TRUE);
        if(!EFI_ERROR(st))release_ready=TRUE;
    }
    if(EFI_ERROR(st))goto finish;
    Step="cold-state";
    Port.read_only_probe=release_ready;
    int cold=tx_guards(&Port.tx);Port.read_only_probe=FALSE;
    if(cold!=TX_OK){st=EFI_SECURITY_VIOLATION;goto finish;}
    field("entry_ss0",Port.tx.initial_ss0);field("entry_ss1",Port.tx.initial_ss1);
    st=experiment_prom(&Port);if(EFI_ERROR(st))goto finish;
    Step="Windows-preload";st=tw_prepare(image,Port.bs,&windows);if(EFI_ERROR(st))goto finish;
    if(release_ready) {
        Step="display-before-driver-release";st=experiment_display(&Port);if(EFI_ERROR(st))goto finish;
        Step="PROM-before-driver-release";st=experiment_prom(&Port);if(EFI_ERROR(st))goto finish;
        Port.read_only_probe=TRUE;cold=tx_guards(&Port.tx);Port.read_only_probe=FALSE;
        if(cold!=TX_OK){st=EFI_SECURITY_VIOLATION;goto finish;}
        Step="driver-release";
#ifdef TU102_AUTOMATIC_INTERNAL
        text("Automatic consent: one2080Ti nvgop-tu1xx release, no5070Ti operation.\r\n");
#else
        text("Manual U confirmed: one2080Ti nvgop-tu1xx release, no5070Ti operation.\r\n");
#endif
        st=experiment_flush();if(EFI_ERROR(st))goto finish;
        st=experiment_watchdog();if(EFI_ERROR(st))goto finish;
        st=tr_disconnect(&Port,system,&pending,&release_attempted);if(EFI_ERROR(st))goto finish;
        Step="ownership-after-release";st=te_claim_at(&Port,Port.release_command);if(EFI_ERROR(st))goto finish;
        Step="display-after-release";st=experiment_display(&Port);if(EFI_ERROR(st))goto finish;
        Step="decode-after-release";st=tr_restore_decode(&Port,&pending);if(EFI_ERROR(st))goto finish;
        Step="PROM-after-release";st=experiment_prom(&Port);if(EFI_ERROR(st))goto finish;
        Step="cold-state-after-release";
        if(tx_guards(&Port.tx)!=TX_OK){st=EFI_SECURITY_VIOLATION;goto finish;}
    }
    Step="formal-EFI-DMA-map";
#ifdef TU102_AMD_DIRECT_PHYSICAL
    Step="guarded-AMD-direct-high-RAM";
#endif
    st=te_allocate(&Port);if(EFI_ERROR(st))goto finish;
    Step="metadata-prepare";
    if(tx_prepare(&Port.tx,asset_booter,asset_signature,asset_boot)!=TX_OK) {
        st=EFI_COMPROMISED_DATA;goto finish;
    }
    Step="final-display";st=experiment_display(&Port);if(EFI_ERROR(st))goto finish;
    Step="final-PROM";
    st=experiment_prom(&Port);if(EFI_ERROR(st))goto finish;
    Step="final-cold-state";if(tx_guards(&Port.tx)!=TX_OK){st=EFI_SECURITY_VIOLATION;goto finish;}
#ifdef TU102_AMD_DIRECT_PHYSICAL
    text("One transaction only.22GiB native FWSEC/FRTS; experimental DIRECT PHYSICAL DMA, not UEFI-mapped.\r\n");
#else
    text("One transaction only. 22GiB FWSEC/FRTS prepared; verified UEFI DMA mapping.\r\n");
#endif
    st=experiment_flush();if(EFI_ERROR(st))goto finish;
    st=experiment_watchdog();if(EFI_ERROR(st))goto finish;
    Step="enable-master";st=te_enable_master(&Port);if(EFI_ERROR(st))goto finish;
#ifdef TU102_AMD_DIRECT_PHYSICAL
    Step="direct-final-before-firmware";st=da_guard(&Port);if(EFI_ERROR(st))goto finish;
#endif
    Step="TU102-transaction";
    int result=tx_run(&Port.tx);
    field("core_result",result);field("mutated",Port.tx.mutated);
    field("unlocked_observation",Port.tx.unlocked);field("quiescent",Port.tx.quiescent);
    st=result?EFI_DEVICE_ERROR:EFI_SUCCESS;
finish:
    text("last_step=");text(Step);text("\r\n");
    if(Port.dma_step) {
        text("dma_step=");text(Port.dma_step);text("\r\n");
        field("dma_supported_attributes",Port.dma_supported);field("dma_active_attributes",Port.dma_active);
        field("dma_allocate_status",Port.dma_allocate_status);field("dma_map_status",Port.dma_map_status);
        field("dma_host_address",(UINTN)Port.pool);field("dma_device_address",Port.device_address);
        field("dma_returned_bytes",Port.dma_mapped_bytes);field("dma_mapping_token_present",Port.mapping!=0);
    }
    field("gpu_write_attempted",Port.tx.mutated);
    field("direct_physical_DMA",Port.direct_physical);
    field("driver_release_attempted",release_attempted);
    field("experiment_status",st);
    EFI_STATUS cleaned=te_release(&Port);
    field("cleanup_status",cleaned);
    EFI_STATUS logged=experiment_flush();
#ifdef TU102_AUTOMATIC_INTERNAL
    ArFailureStatus=EFI_ERROR(st)?st:EFI_ERROR(cleaned)?cleaned:EFI_ERROR(logged)?logged:EFI_DEVICE_ERROR;
#endif
    if(Port.retained || ((Port.tx.mutated || release_attempted) &&
       (EFI_ERROR(st) || EFI_ERROR(cleaned) || EFI_ERROR(logged))))
        experiment_stop();
    if(!EFI_ERROR(st) && EFI_ERROR(cleaned))st=cleaned;
    if(!EFI_ERROR(st) && EFI_ERROR(logged))st=logged;
    EFI_STATUS closed=TxLog->Close(TxLog);TxLog=0;
    EFI_STATUS closed_dir=dir->Close(dir);
    if((Port.tx.mutated || release_attempted) && (EFI_ERROR(closed) || EFI_ERROR(closed_dir)))experiment_stop();
    if(!EFI_ERROR(st) && Port.tx.unlocked && Port.tx.quiescent) {
#ifdef TU102_AUTOMATIC_INTERNAL
        Step="Windows-handoff";
        st=ar_commit(system);if(EFI_ERROR(st)){ArFailureStatus=st;experiment_stop();}
#endif
        st=tw_start(Port.bs,&windows); /* no reset and never a retry */
#ifdef TU102_AUTOMATIC_INTERNAL
        ArFailureStatus=EFI_ERROR(st)?st:EFI_ABORTED;
#endif
        (void)st;experiment_stop();
    }
    if(release_attempted)experiment_stop(); /* never return modified driver state to GRUB */
#ifdef TU102_AUTOMATIC_INTERNAL
    return ar_fallback(image,system,&windows,st);
#else
    (void)tw_cancel(Port.bs,&windows);
    experiment_screen_ascii(system,"\r\nTU102 refused/cancelled at: ");
    experiment_screen_ascii(system,Step);
    experiment_screen_ascii(system,"\r\n");
    if(!Port.tx.mutated)experiment_screen_ascii(system,"No GPU unlock writes or firmware execution attempted.\r\n");
    experiment_screen_ascii(system,"Details: USB TU102-CAPTURE-*/TRANSACTION.TXT. No unlock success.\r\n");
    return st;
#endif
}
