/* Actual handoff helper, injected services; no disk/BCD/NVRAM/GPU access. */
#include <Uefi.h>
#include "handoff.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EFI_BOOT_SERVICES bs;
static EFI_LOADED_IMAGE_PROTOCOL self_image,windows_image;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL filesystems[3];
static EFI_FILE_PROTOCOL roots[3],files[3];
static int parent_token,source_token,volume_tokens[3],win_token;
static UINT8 path[16]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,0x7f,0xff,4,0};
static unsigned count,found_mask,loads,unloads,starts;
static int fail_open,fail_load,wrong_volume,no_base,large_image,fail_close,fail_unload;
static int bad_path,alloc_fail,fail_free,returned_error_image;
static EFI_GUID fs_guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID li_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID dp_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static int eq_guid(EFI_GUID *a,EFI_GUID *b){return !memcmp(a,b,sizeof(*a));}
static int eq16(CHAR16 *a,CHAR16 *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static EFI_STATUS EFIAPI alloc_mock(EFI_MEMORY_TYPE type,UINTN n,VOID **out) {
    assert(type==EfiLoaderData);*out=alloc_fail?0:calloc(1,(size_t)n);
    return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI free_mock(VOID *p){free(p);return fail_free?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI locate_mock(EFI_LOCATE_SEARCH_TYPE t,EFI_GUID *g,VOID *key,UINTN *n,EFI_HANDLE **out) {
    assert(t==ByProtocol && eq_guid(g,&fs_guid) && !key);
    *n=count;*out=calloc(count?count:1,sizeof(**out));assert(*out);
    for(UINTN i=0;i<count;i++)(*out)[i]=&volume_tokens[i];
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle_mock(EFI_HANDLE h,EFI_GUID *g,VOID **out) {
    if(h==&parent_token && eq_guid(g,&li_guid)){*out=&self_image;return EFI_SUCCESS;}
    if(h==&win_token && eq_guid(g,&li_guid)){*out=&windows_image;return EFI_SUCCESS;}
    for(unsigned i=0;i<3;i++)if(h==&volume_tokens[i]) {
        if(eq_guid(g,&fs_guid)){*out=&filesystems[i];return EFI_SUCCESS;}
        if(eq_guid(g,&dp_guid)) {
            path[2]=(UINT8)(bad_path?0:12);*out=path;return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI open_volume_mock(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,EFI_FILE_PROTOCOL **out) {
    int index=(int)(fs-filesystems);assert(index>=0 && index<3);*out=&roots[index];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI open_mock(EFI_FILE_PROTOCOL *root,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 mode,UINT64 attrs) {
    int index=(int)(root-roots);assert(index>=0 && index<3);
    assert(eq16(name,L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi") && mode==EFI_FILE_MODE_READ && !attrs);
    if(fail_open)return EFI_DEVICE_ERROR;
    if(!(found_mask&(1U<<index)))return EFI_NOT_FOUND;
    *out=&files[index];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_mock(EFI_FILE_PROTOCOL *f){(void)f;return fail_close?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI load_mock(BOOLEAN policy,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *p,VOID *src,UINTN n,EFI_HANDLE *out) {
    assert(!policy && parent==&parent_token && !src && !n);loads++;
    UINT8 *b=(VOID *)p;assert(!memcmp(b,path,12));
    EFI_DEVICE_PATH_PROTOCOL *file=(VOID *)(b+12);assert(file->Type==4 && file->SubType==4);
    assert(eq16((CHAR16 *)(b+16),L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"));
    if(fail_load){*out=returned_error_image?&win_token:0;return EFI_SECURITY_VIOLATION;}
    *out=&win_token;
    windows_image.DeviceHandle=wrong_volume?&source_token:&volume_tokens[0];
    windows_image.ImageBase=no_base?0:(VOID *)&load_mock;
    windows_image.ImageSize=large_image?0x8000000:0x100000;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unload_mock(EFI_HANDLE h){assert(h==&win_token);unloads++;return fail_unload?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI start_mock(EFI_HANDLE h,UINTN *size,CHAR16 **data) {
    assert(h==&win_token && !size && !data);starts++;return EFI_DEVICE_ERROR;
}
static void setup(void) {
    memset(&bs,0,sizeof(bs));memset(&self_image,0,sizeof(self_image));memset(&windows_image,0,sizeof(windows_image));
    for(unsigned i=0;i<3;i++) {
        memset(&roots[i],0,sizeof(roots[i]));memset(&files[i],0,sizeof(files[i]));
        filesystems[i].OpenVolume=open_volume_mock;roots[i].Open=open_mock;
        roots[i].Close=files[i].Close=close_mock;
    }
    bs.HandleProtocol=handle_mock;bs.LocateHandleBuffer=locate_mock;
    bs.AllocatePool=alloc_mock;bs.FreePool=free_mock;bs.LoadImage=load_mock;bs.UnloadImage=unload_mock;bs.StartImage=start_mock;
    self_image.DeviceHandle=&source_token;
    count=1;found_mask=1;loads=unloads=starts=0;
    fail_open=fail_load=wrong_volume=no_base=large_image=fail_close=fail_unload=bad_path=alloc_fail=fail_free=returned_error_image=0;
}
int main(void) {
    unsigned cases=0;TuWindows w;
    setup();memset(&w,0,sizeof(w));assert(tw_prepare(&parent_token,&bs,&w)==EFI_SUCCESS && w.prepared && loads==1 && !starts);cases++;
    assert(tw_cancel(&bs,&w)==EFI_SUCCESS && unloads==1 && !w.image);cases++;
    setup();memset(&w,0,sizeof(w));assert(tw_prepare(&parent_token,&bs,&w)==EFI_SUCCESS);
    assert(tw_start(&bs,&w)==EFI_DEVICE_ERROR && starts==1 && w.started);cases++;
    assert(tw_start(&bs,&w)==EFI_ACCESS_DENIED && starts==1);cases++;
    assert(tw_cancel(&bs,&w)==EFI_ACCESS_DENIED && !unloads);cases++;
    for(unsigned bad=0;bad<10;bad++) {
        setup();memset(&w,0,sizeof(w));
        switch(bad) {
        case 0:count=0;break;case 1:count=2;found_mask=3;break;case 2:found_mask=0;break;
        case 3:fail_open=1;break;case 4:fail_close=1;break;case 5:bad_path=1;break;
        case 6:alloc_fail=1;break;case 7:fail_load=1;break;case 8:fail_free=1;break;
        case 9:self_image.DeviceHandle=&volume_tokens[0];break;
        }
        assert(EFI_ERROR(tw_prepare(&parent_token,&bs,&w)) && !w.prepared && !starts);cases++;
    }
    for(unsigned bad=0;bad<4;bad++) {
        setup();memset(&w,0,sizeof(w));
        switch(bad){case 0:wrong_volume=1;break;case 1:no_base=1;break;case 2:large_image=1;break;
            case 3:fail_load=returned_error_image=1;break;}
        assert(EFI_ERROR(tw_prepare(&parent_token,&bs,&w)) && !w.prepared && unloads==1 && !starts);cases++;
    }
    setup();memset(&w,0,sizeof(w));assert(tw_prepare(&parent_token,&bs,&w)==EFI_SUCCESS);
    assert(tw_prepare(&parent_token,&bs,&w)==EFI_INVALID_PARAMETER && loads==1);cases++;
    printf("PASS %u Windows handoff helper mock cases; no disk/boot/GPU operations\n",cases);
    return 0;
}
