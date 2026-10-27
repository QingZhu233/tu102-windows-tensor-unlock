/* Local, opt-in automatic entry for the measured CAPTURE21 profile.
 * Writes only its own fixed log on the exact local GPT ESP. Never creates
 * directories, changes boot variables, or changes a partition from firmware.
 */
#ifndef TU102_INTERNAL_AUTO_H
#define TU102_INTERNAL_AUTO_H
#include <Guid/FileInfo.h>
#include "../build/local_bindings.h"
#if !defined(TU102_AMD_DIRECT_PHYSICAL) || !defined(TU102_ENABLE_UNVERIFIED_EXECUTION)
#error Internal automatic mode requires the explicitly selected local direct profile
#endif
static EFI_GUID TaInfoGuid=EFI_FILE_INFO_ID;
static int ta_partition(const EFI_DEVICE_PATH_PROTOCOL *path) {
    static const UINT8 signature[16]=TU102_LOCAL_ESP_SIGNATURE;
    if(!path)return 0;
    UINTN length=0;unsigned found=0;
    for(unsigned i=0;i<64;i++) {
        if(length>4092)return 0;
        const UINT8 *n=(const UINT8 *)path+length;
        UINTN bytes=n[2]+((UINTN)n[3]<<8);
        if(bytes<4 || bytes>4096-length)return 0;
        if(n[0]==0x7f)return n[1]==0xff && bytes==4 && found==1;
        if(n[0]==4 && n[1]==1) {
            if(bytes!=42 || found++ || da32(n+4)!=1 || da64(n+8)!=2048 ||
               da64(n+16)!=409600 || !tr_equal_bytes(n+24,signature,16) || n[40]!=2 || n[41]!=2)return 0;
        }
        length+=bytes;
    }
    return 0;
}
static EFI_STATUS ta_open(EFI_HANDLE image,EFI_BOOT_SERVICES *bs,EFI_FILE_PROTOCOL **directory) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded=0;
    EFI_BLOCK_IO_PROTOCOL *block=0;
    EFI_DEVICE_PATH_PROTOCOL *path=0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=0;
    EFI_FILE_PROTOCOL *root=0,*dir=0,*marker=0;
    *directory=0;
    EFI_STATUS st=bs->HandleProtocol(image,&ImageGuid,(VOID **)&loaded);
    if(EFI_ERROR(st) || !loaded || !loaded->DeviceHandle)return EFI_NOT_FOUND;
    st=bs->HandleProtocol(loaded->DeviceHandle,&BlockGuid,(VOID **)&block);
    if(EFI_ERROR(st) || !block || !block->Media || !block->Media->MediaPresent ||
       block->Media->RemovableMedia || block->Media->ReadOnly || !block->Media->LogicalPartition ||
       block->Media->BlockSize!=512 || block->Media->LastBlock!=409599)return EFI_UNSUPPORTED;
    st=bs->HandleProtocol(loaded->DeviceHandle,&PathGuid,(VOID **)&path);
    if(EFI_ERROR(st) || !ta_partition(path))return EFI_SECURITY_VIOLATION;
    st=bs->HandleProtocol(loaded->DeviceHandle,&FsGuid,(VOID **)&fs);
    if(EFI_ERROR(st) || !fs || !fs->OpenVolume)return EFI_NOT_FOUND;
    st=fs->OpenVolume(fs,&root);if(EFI_ERROR(st) || !root)return EFI_DEVICE_ERROR;
    st=root->Open(root,&dir,L"\\EFI\\TU102Unlock",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
    EFI_STATUS close=root->Close(root);
    if(EFI_ERROR(st) || !dir)return EFI_ERROR(st)?st:EFI_NOT_FOUND;
    if(EFI_ERROR(close)){dir->Close(dir);return close;}
    st=dir->Open(dir,&marker,L"AUTO.ENABLED",EFI_FILE_MODE_READ,0);
    if(EFI_ERROR(st) || !marker){dir->Close(dir);return EFI_ACCESS_DENIED;}
    static const char token[]="TU102-CAPTURE21-LOCAL-AUTO-269bc7cdbeb7e86a49d311d3795108b962ecf5bcee17267447353bc0575cae5a\n";
    char actual[sizeof(token)];UINTN bytes=sizeof(actual);
    st=marker->Read?marker->Read(marker,&bytes,actual):EFI_UNSUPPORTED;
    close=marker->Close(marker);
    if(EFI_ERROR(st) || EFI_ERROR(close) || bytes!=sizeof(token)-1 ||
       !tr_equal_bytes(actual,token,sizeof(token)-1)){dir->Close(dir);return EFI_ACCESS_DENIED;}
    *directory=dir;text("capture_directory=internal-ESP/EFI/TU102Unlock\r\n");
    return EFI_SUCCESS;
}
static EFI_STATUS ta_truncate(EFI_FILE_PROTOCOL *file) {
    if(!file || !file->GetInfo || !file->SetInfo || !file->SetPosition)return EFI_UNSUPPORTED;
    union {UINT64 align;UINT8 bytes[512];} storage;
    UINTN size=sizeof(storage);zero_bytes(&storage,sizeof(storage));
    EFI_STATUS st=file->GetInfo(file,&TaInfoGuid,&size,&storage);
    EFI_FILE_INFO *info=(VOID *)&storage;
    if(EFI_ERROR(st))return st;
    if(size<SIZE_OF_EFI_FILE_INFO+2 || size>sizeof(storage) || info->Size!=size ||
       (info->Attribute&EFI_FILE_DIRECTORY))return EFI_COMPROMISED_DATA;
    info->FileSize=0;st=file->SetInfo(file,&TaInfoGuid,size,info);
    if(EFI_ERROR(st))return st;
    return file->SetPosition(file,0);
}
static EFI_STATUS ta_arm(EFI_SYSTEM_TABLE *system) {
    /* Installer consent and exact marker replace U only for this local build.
     * Every GPU, PROM, platform, owner, cold-state and cleanup guard remains.
     * Start immediately: no screen output, keyboard read/reset, skip or wait.
     * Refusal/STOP paths retain their visible diagnostics. */
    (void)system;
    text("automatic_consent=exact_ESP_marker_and_installed_local_profile\r\n");
    text("automatic_start=immediate_no_keyboard_no_skip\r\n");
    return EFI_SUCCESS;
}
#endif
