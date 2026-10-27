/* Automatic-only Windows recovery. No GPU register writes here.
 * Boot0000 is pinned to the preserved local Windows Boot Manager option.
 * Arm one-shot Windows BootNext and a firmware watchdog BEFORE GPU mutation;
 * remove the one-shot choice on success. Do not disable the mainboard boot menu.
 */
#ifndef TU102_AUTOMATIC_RECOVERY_H
#define TU102_AUTOMATIC_RECOVERY_H
static EFI_GUID ArGlobal={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
static BOOLEAN ArOwnNext,ArWatchdog;
static EFI_STATUS ArFailureStatus;
static void ar_init(void){ArOwnNext=FALSE;ArWatchdog=FALSE;ArFailureStatus=EFI_DEVICE_ERROR;}
static EFI_STATUS ar_windows_option(EFI_SYSTEM_TABLE *s) {
    if(!s->RuntimeServices->GetVariable || !s->RuntimeServices->SetVariable ||
       !s->RuntimeServices->ResetSystem || !s->BootServices->SetWatchdogTimer)return EFI_UNSUPPORTED;
    UINT8 option[512];UINTN bytes=sizeof(option);UINT32 attributes=0;
    EFI_STATUS st=s->RuntimeServices->GetVariable(L"Boot0000",&ArGlobal,&attributes,&bytes,option);
    if(EFI_ERROR(st))return st;
    if(attributes!=7 || bytes!=300 || !GuHashMatches(option,bytes,
       "9ded5321fc59e15cf7d024fe088fb19d78ac7e31d8eaef7392e0ab5425213d33"))return EFI_SECURITY_VIOLATION;
    return EFI_SUCCESS;
}
static EFI_STATUS ar_next(EFI_SYSTEM_TABLE *s,BOOLEAN clear) {
    if(clear && !ArOwnNext)return EFI_SUCCESS;
    if(!s->RuntimeServices->GetVariable || !s->RuntimeServices->SetVariable)return EFI_UNSUPPORTED;
    UINT16 next=0xffff;UINTN bytes=sizeof(next);UINT32 attributes=0;
    EFI_STATUS st=s->RuntimeServices->GetVariable(L"BootNext",&ArGlobal,&attributes,&bytes,&next);
    if(clear) {
        if(st==EFI_NOT_FOUND){ArOwnNext=FALSE;return EFI_SUCCESS;}
        if(EFI_ERROR(st) || attributes!=7 || bytes!=2 || next!=0)return EFI_ACCESS_DENIED;
        st=s->RuntimeServices->SetVariable(L"BootNext",&ArGlobal,0,0,0);
        if(EFI_ERROR(st))return st;
        bytes=sizeof(next);st=s->RuntimeServices->GetVariable(L"BootNext",&ArGlobal,0,&bytes,&next);
        if(st!=EFI_NOT_FOUND)return EFI_DEVICE_ERROR;
        ArOwnNext=FALSE;return EFI_SUCCESS;
    }
    if(st!=EFI_NOT_FOUND) {
        if(ArOwnNext && !EFI_ERROR(st) && attributes==7 && bytes==2 && next==0)return EFI_SUCCESS;
        return EFI_ACCESS_DENIED; /* never replace another pending boot choice */
    }
    next=0;st=s->RuntimeServices->SetVariable(L"BootNext",&ArGlobal,7,sizeof(next),&next);
    if(EFI_ERROR(st))return st;
    ArOwnNext=TRUE;bytes=sizeof(next);attributes=0;next=0xffff;
    st=s->RuntimeServices->GetVariable(L"BootNext",&ArGlobal,&attributes,&bytes,&next);
    return !EFI_ERROR(st) && attributes==7 && bytes==2 && next==0?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
static EFI_STATUS ar_prepare(EFI_SYSTEM_TABLE *s) {
    if(ArOwnNext && ArWatchdog)return EFI_SUCCESS;
    EFI_STATUS st=ar_windows_option(s);if(EFI_ERROR(st))return st;
    UINT16 next=0xffff;UINTN bytes=sizeof(next);
    st=s->RuntimeServices->GetVariable(L"BootNext",&ArGlobal,0,&bytes,&next);
    if(st!=EFI_NOT_FOUND)return EFI_ACCESS_DENIED;
    st=s->BootServices->SetWatchdogTimer(120,0,0,0);if(EFI_ERROR(st))return st;
    ArWatchdog=TRUE;st=ar_next(s,FALSE);
    field("automatic_recovery_BootNext_status",st);
    if(!EFI_ERROR(st))text("automatic_recovery=Boot0000_pinned_and_watchdog120s_armed\r\n");
    return st;
}
static EFI_STATUS ar_commit(EFI_SYSTEM_TABLE *s) {
    if(ArWatchdog) {
        EFI_STATUS st=s->BootServices->SetWatchdogTimer(0,0,0,0);
        if(EFI_ERROR(st))return st;
        ArWatchdog=FALSE;
    }
    return ar_next(s,TRUE);
}
static void ar_message(EFI_SYSTEM_TABLE *s,const char *action,EFI_STATUS error) {
    experiment_screen_ascii(s,"\r\nTU102 unlock error at: ");experiment_screen_ascii(s,Step);
    experiment_screen_ascii(s,"\r\nStatus: ");
    static const CHAR16 digits[]=L"0123456789abcdef";CHAR16 number[19];number[0]='0';number[1]='x';
    for(unsigned i=0;i<16;i++)number[i+2]=digits[(error>>((15-i)*4))&15];
    number[18]=0;
    if(s->ConOut && s->ConOut->OutputString)s->ConOut->OutputString(s->ConOut,number);
    experiment_screen_ascii(s,"\r\n");experiment_screen_ascii(s,action);experiment_screen_ascii(s,"\r\n");
}
static EFI_STATUS ar_fallback(EFI_HANDLE parent,EFI_SYSTEM_TABLE *s,TuWindows *w,EFI_STATUS error) {
    ar_message(s,"Starting Windows without this unlock attempt.",error);
    EFI_STATUS disarmed=ar_commit(s);field("automatic_fallback_disarm_status",disarmed);
    if(!w->prepared) {
        EFI_STATUS loaded=tw_prepare(parent,s->BootServices,w);
        if(EFI_ERROR(loaded)) {
            ar_message(s,"Windows loader could not be loaded; returning to firmware boot manager.",loaded);
            return loaded;
        }
    }
    (void)s->BootServices->Stall(3000000); /* error-only visibility; no key prompt */
    EFI_STATUS st=tw_start(s->BootServices,w);
    ar_message(s,"Windows loader returned; returning to firmware boot manager.",st);
    return EFI_ERROR(st)?st:EFI_ABORTED;
}
static VOID ar_reset_recovery(void) {
    EFI_STATUS st=ar_windows_option(TxSystem);
    if(!EFI_ERROR(st))st=ar_next(TxSystem,FALSE);
    field("automatic_error_recovery_status",st);
    text("automatic_error_recovery=cold_reset_to_original_Windows_no_unlock_retry\r\n");
    if(TxLog)(void)experiment_flush();
    ar_message(TxSystem,"Restarting into Windows Boot Manager without retrying unlock.",ArFailureStatus);
    (void)TxSystem->BootServices->Stall(3000000);
    if(TxSystem->RuntimeServices->ResetSystem)
        TxSystem->RuntimeServices->ResetSystem(EfiResetCold,EFI_ABORTED,0,0);
    /* A broken ResetSystem/watchdog cannot be repaired in software. The saved
     * Windows entry remains available in the firmware menu for physical recovery. */
    ar_message(TxSystem,"Firmware reset failed. Power off and select Windows Boot Manager.",EFI_DEVICE_ERROR);
#ifdef TU102_HOST_TEST
    extern void tu102_host_stop(void);tu102_host_stop();
#endif
    for(;;)TxSystem->BootServices->Stall(1000000);
}
#endif
