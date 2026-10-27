/* Local experiment: preload a unique Windows Boot Manager without modifying
 * any partition, BCD, NVRAM or boot order. No reset or automatic fallback.
 * Default build does not execute it; experimental runtime calls once only.
 */
#ifndef TU102_WINDOWS_HANDOFF_H
#define TU102_WINDOWS_HANDOFF_H
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

typedef struct {EFI_HANDLE image;BOOLEAN prepared,started;} TuWindows;

static EFI_STATUS tw_prepare(EFI_HANDLE parent,EFI_BOOT_SERVICES *bs,TuWindows *w) {
    static EFI_GUID fs_guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    static EFI_GUID image_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
    static EFI_GUID path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;
    static const CHAR16 filename[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    EFI_LOADED_IMAGE_PROTOCOL *self=0;
    EFI_HANDLE *handles=0,chosen=0;
    UINTN count=0,matches=0;
    if(!w || w->prepared || w->image || !bs->LoadImage || !bs->StartImage || !bs->UnloadImage)
        return EFI_INVALID_PARAMETER;
    EFI_STATUS st=bs->HandleProtocol(parent,&image_guid,(VOID **)&self);
    if(EFI_ERROR(st) || !self || !self->DeviceHandle)return EFI_NOT_FOUND;
    st=bs->LocateHandleBuffer(ByProtocol,&fs_guid,0,&count,&handles);
    if(EFI_ERROR(st))return st;
    if(!handles || count>128){if(handles)bs->FreePool(handles);return EFI_UNSUPPORTED;}
    for(UINTN i=0;i<count;i++) {
#ifndef TU102_AUTOMATIC_INTERNAL
        if(handles[i]==self->DeviceHandle)continue;
#endif
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=0;
        EFI_FILE_PROTOCOL *root=0,*file=0;
        st=bs->HandleProtocol(handles[i],&fs_guid,(VOID **)&fs);
        if(EFI_ERROR(st) || !fs || !fs->OpenVolume) {
            bs->FreePool(handles);return EFI_ERROR(st)?st:EFI_COMPROMISED_DATA;
        }
        st=fs->OpenVolume(fs,&root);
        if(EFI_ERROR(st) || !root) {
            bs->FreePool(handles);return EFI_ERROR(st)?st:EFI_DEVICE_ERROR;
        }
        st=root->Open(root,&file,(CHAR16 *)filename,EFI_FILE_MODE_READ,0);
        if(!EFI_ERROR(st) && file) {
            EFI_STATUS close=file->Close(file);
            if(EFI_ERROR(close)){root->Close(root);bs->FreePool(handles);return close;}
            matches++;chosen=handles[i];
        } else if(st!=EFI_NOT_FOUND) {
            root->Close(root);bs->FreePool(handles);return EFI_ERROR(st)?st:EFI_DEVICE_ERROR;
        }
        EFI_STATUS close=root->Close(root);
        if(EFI_ERROR(close)){bs->FreePool(handles);return close;}
    }
    st=bs->FreePool(handles);if(EFI_ERROR(st))return st;
    if(matches!=1)return matches?EFI_ACCESS_DENIED:EFI_NOT_FOUND;
    EFI_DEVICE_PATH_PROTOCOL *path=0;
    st=bs->HandleProtocol(chosen,&path_guid,(VOID **)&path);
    if(EFI_ERROR(st) || !path)return EFI_NOT_FOUND;
    UINTN length=0;
    BOOLEAN ended=FALSE;
    for(UINTN i=0;i<64;i++) {
        if(length>4096-sizeof(EFI_DEVICE_PATH_PROTOCOL))return EFI_COMPROMISED_DATA;
        const EFI_DEVICE_PATH_PROTOCOL *n=(const VOID *)((const UINT8 *)path+length);
        UINTN bytes=n->Length[0]+((UINTN)n->Length[1]<<8);
        if(bytes<4 || bytes>4096-length)return EFI_COMPROMISED_DATA;
        if(n->Type==0x7f) {
            if(n->SubType!=0xff || bytes!=4)return EFI_UNSUPPORTED;
            ended=TRUE;break;
        }
        length+=bytes;
    }
    if(!ended)return EFI_COMPROMISED_DATA;
    const UINTN node_size=sizeof(filename)+4;
    UINTN total=length+node_size+4;
    UINT8 *joined=0;
    st=bs->AllocatePool(EfiLoaderData,total,(VOID **)&joined);
    if(EFI_ERROR(st) || !joined)return EFI_OUT_OF_RESOURCES;
    for(UINTN i=0;i<length;i++)joined[i]=((UINT8 *)path)[i];
    EFI_DEVICE_PATH_PROTOCOL *file_node=(VOID *)(joined+length);
    file_node->Type=4;file_node->SubType=4;
    file_node->Length[0]=(UINT8)node_size;file_node->Length[1]=(UINT8)(node_size>>8);
    for(UINTN i=0;i<sizeof(filename);i++)joined[length+4+i]=((const UINT8 *)filename)[i];
    EFI_DEVICE_PATH_PROTOCOL *end=(VOID *)(joined+length+node_size);
    end->Type=0x7f;end->SubType=0xff;end->Length[0]=4;end->Length[1]=0;
    st=bs->LoadImage(FALSE,parent,(EFI_DEVICE_PATH_PROTOCOL *)joined,0,0,&w->image);
    EFI_STATUS freed=bs->FreePool(joined);
    if(EFI_ERROR(st) || !w->image || EFI_ERROR(freed)) {
        if(w->image){bs->UnloadImage(w->image);w->image=0;}
        return EFI_ERROR(st)?st:EFI_ERROR(freed)?freed:EFI_DEVICE_ERROR;
    }
    EFI_LOADED_IMAGE_PROTOCOL *win=0;
    st=bs->HandleProtocol(w->image,&image_guid,(VOID **)&win);
    if(EFI_ERROR(st) || !win || win->DeviceHandle!=chosen || !win->ImageBase || !win->ImageSize ||
       win->ImageSize>64U*1024U*1024U) {
        bs->UnloadImage(w->image);w->image=0;return EFI_COMPROMISED_DATA;
    }
    w->prepared=TRUE;return EFI_SUCCESS;
}
static EFI_STATUS tw_cancel(EFI_BOOT_SERVICES *bs,TuWindows *w) {
    if(!w || w->started)return EFI_ACCESS_DENIED;
    if(!w->image)return EFI_SUCCESS;
    EFI_STATUS st=bs->UnloadImage(w->image);
    if(!EFI_ERROR(st)){w->prepared=FALSE;w->image=0;}
    return st;
}
static EFI_STATUS tw_start(EFI_BOOT_SERVICES *bs,TuWindows *w) {
    if(!w || !w->image || !w->prepared || w->started)return EFI_ACCESS_DENIED;
    w->started=TRUE; /* spend before callback: no second attempt/fallback */
    return bs->StartImage(w->image,0,0);
}
#endif
