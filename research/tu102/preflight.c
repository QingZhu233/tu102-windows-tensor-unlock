/* SPDX-License-Identifier: GPL-2.0-only
 * Local Windows/TU102 port preparation using the upstream EDK2 headers.
 * Default: firmware-cached option ROM only. Explicit live-PROM build adds
 * guarded BAR0 reads; neither build writes PCI/GPU state, starts DMA, resets,
 * executes firmware, installs boot entries, or changes Windows.
 */
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include "tu102_profile.h"

#define LOG_LIMIT 32768U
#define ROM_LIMIT 0x100000U
#define MAP_LIMIT 0x100000U

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID ImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID FsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID BlockGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID PathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static char Report[LOG_LIMIT];
static UINTN Used;
static int LogFault;
/* Forces a load-base relocation; verified by the PE checker after linking. */
static char * volatile ReportBase = Report;

static void zero_bytes(void *dst, UINTN n) {
    UINT8 *p = dst;
    while (n--) *p++ = 0;
}
static void copy_bytes(void *dst, const void *src, UINTN n) {
    UINT8 *d = dst;
    const UINT8 *s = src;
    while (n--) *d++ = *s++;
}
static void text(const char *s) {
    while (*s) {
        if (Used + 1 >= LOG_LIMIT) { LogFault = 1; return; }
        ReportBase[Used++] = *s++;
    }
    ReportBase[Used] = 0;
}
static void hex(UINT64 v, UINTN digits) {
    static const char chars[] = "0123456789abcdef";
    char s[17];
    if (digits > 16) { LogFault = 1; return; }
    for (UINTN i = 0; i < digits; i++) s[i] = chars[(v >> ((digits-i-1)*4)) & 15];
    s[digits] = 0;
    text(s);
}
static void field(const char *name, UINT64 v) {
    text(name); text("=0x"); hex(v,16); text("\r\n");
}
static void location(UINTN seg, UINTN bus, UINTN dev, UINTN fn) {
    hex(seg,4); text(":"); hex(bus,2); text(":"); hex(dev,2); text("."); hex(fn,1);
}

/* Returns a new, exclusively used capture directory on the launch volume.
 * A marker file is required. RemovableMedia is required so this program cannot
 * write reports onto an internal Windows/system volume by accident.
 */
static EFI_STATUS open_capture(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                               EFI_FILE_PROTOCOL **directory) {
#ifndef TU102_CAPTURE_TRACE
#define TU102_CAPTURE_TRACE(label, status) ((void)0)
#endif
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_BLOCK_IO_PROTOCOL *block = 0;
    EFI_FILE_PROTOCOL *root = 0, *marker = 0, *existing = 0;
    EFI_STATUS st;
    *directory = 0;
    st = bs->HandleProtocol(image, &ImageGuid, (VOID **)&loaded);
    TU102_CAPTURE_TRACE("loaded-image",st);
    if (EFI_ERROR(st) || !loaded || !loaded->DeviceHandle) return EFI_NOT_FOUND;
    st = bs->HandleProtocol(loaded->DeviceHandle, &BlockGuid, (VOID **)&block);
    TU102_CAPTURE_TRACE("launch-volume-block-io",st);
    if(block && block->Media) {
        TU102_CAPTURE_TRACE("media-present",block->Media->MediaPresent);
        TU102_CAPTURE_TRACE("media-removable",block->Media->RemovableMedia);
        TU102_CAPTURE_TRACE("media-read-only",block->Media->ReadOnly);
    }
    if (EFI_ERROR(st) || !block || !block->Media || !block->Media->MediaPresent ||
        !block->Media->RemovableMedia || block->Media->ReadOnly) return EFI_UNSUPPORTED;
    st = bs->HandleProtocol(loaded->DeviceHandle, &FsGuid, (VOID **)&fs);
    TU102_CAPTURE_TRACE("launch-volume-filesystem",st);
    if (EFI_ERROR(st) || !fs || !fs->OpenVolume) return EFI_NOT_FOUND;
    st = fs->OpenVolume(fs, &root);
    TU102_CAPTURE_TRACE("open-volume",st);
    if (EFI_ERROR(st) || !root) return EFI_ERROR(st) ? st : EFI_DEVICE_ERROR;
    st = root->Open(root, &marker, L"TU102-PREFLIGHT-ONLY.TAG", EFI_FILE_MODE_READ, 0);
    TU102_CAPTURE_TRACE("open-volume-marker",st);
    if (EFI_ERROR(st) || !marker) { root->Close(root); return EFI_ACCESS_DENIED; }
    st = marker->Close(marker);
    if (EFI_ERROR(st)) { root->Close(root); return st; }
    for (UINTN i=0; i<100; i++) {
        CHAR16 path[] = L"TU102-CAPTURE-00";
        path[15] = (CHAR16)('0' + i%10);
        path[14] = (CHAR16)('0' + i/10);
        existing = 0;
        st = root->Open(root, &existing, path, EFI_FILE_MODE_READ, 0);
        TU102_CAPTURE_TRACE("existing-directory-number",i);
        TU102_CAPTURE_TRACE("existing-directory-open",st);
        if (!EFI_ERROR(st)) {
            if (!existing) { st = EFI_DEVICE_ERROR; break; }
            st = existing->Close(existing);
            if (EFI_ERROR(st)) break;
            continue;
        }
        if (st != EFI_NOT_FOUND) break;
        st = root->Open(root, directory, path,
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                        EFI_FILE_DIRECTORY);
        TU102_CAPTURE_TRACE("create-directory",st);
        if (!EFI_ERROR(st) && !*directory) st = EFI_DEVICE_ERROR;
        if (!EFI_ERROR(st)) {
            text("capture_directory=TU102-CAPTURE-"); hex(i/10,1); hex(i%10,1); text("\r\n");
        }
        break;
    }
    EFI_STATUS closed = root->Close(root);
    if (!*directory && !EFI_ERROR(st)) st = EFI_ACCESS_DENIED;
    if (!EFI_ERROR(st) && EFI_ERROR(closed)) st = closed;
    if (EFI_ERROR(st) && *directory) { (*directory)->Close(*directory); *directory=0; }
    return st;
}

static EFI_STATUS save_new(EFI_FILE_PROTOCOL *dir, CHAR16 *name,
                          const VOID *data, UINTN bytes) {
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS st = dir->Open(dir, &f, name, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(st)) { if(f)f->Close(f); return EFI_ACCESS_DENIED; }
    if (st != EFI_NOT_FOUND) return st;
    st = dir->Open(dir, &f, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(st) || !f) return EFI_ERROR(st) ? st : EFI_DEVICE_ERROR;
    UINTN written=bytes;
    st = f->Write(f, &written, (VOID *)data);
    if (!EFI_ERROR(st) && written != bytes) st = EFI_DEVICE_ERROR;
    if (!EFI_ERROR(st)) st = f->Flush(f);
    EFI_STATUS closed=f->Close(f);
    return !EFI_ERROR(st) && EFI_ERROR(closed) ? closed : st;
}

#ifdef TU102_LIVE_PROM_CAPTURE
#include "live_prom.h"
#endif

/* A protocol ROM pointer must refer to one bounded region of normal host RAM.
 * This rejects MMIO/reserved/conventional regions rather than probing them.
 */
static int rom_in_host_ram(EFI_BOOT_SERVICES *bs, const VOID *rom, UINT64 bytes) {
    EFI_MEMORY_DESCRIPTOR *map=0;
    UINTN needed=0, key=0, stride=0, capacity=0;
    UINT32 version=0;
    UINT64 start=(UINTN)rom;
    if (!start || bytes<0x1a || bytes>ROM_LIMIT || start>~(UINT64)0-bytes) return 0;
    EFI_STATUS st=bs->GetMemoryMap(&needed,0,&key,&stride,&version);
    if(st!=EFI_BUFFER_TOO_SMALL || stride<sizeof(*map) || stride>4096 || needed>MAP_LIMIT) return 0;
    for (UINTN retry=0;retry<3;retry++) {
        if(needed>MAP_LIMIT || stride>4096 || stride<sizeof(*map)) return 0;
        capacity=needed+16*stride;
        st=bs->AllocatePool(EfiLoaderData,capacity,(VOID **)&map);
        if(EFI_ERROR(st) || !map)return 0;
        needed=capacity;
        st=bs->GetMemoryMap(&needed,map,&key,&stride,&version);
        if(st!=EFI_BUFFER_TOO_SMALL)break;
        if(EFI_ERROR(bs->FreePool(map)))return 0;
        map=0;
    }
    int ok=0;
    if(!EFI_ERROR(st) && map && needed<=capacity && stride>=sizeof(*map) && stride<=4096 && !(needed%stride)) {
        for(UINTN off=0;off<needed;off+=stride) {
            EFI_MEMORY_DESCRIPTOR *d=(VOID *)((UINT8 *)map+off);
            if(d->Type!=EfiLoaderCode && d->Type!=EfiLoaderData &&
               d->Type!=EfiBootServicesCode && d->Type!=EfiBootServicesData &&
               d->Type!=EfiRuntimeServicesCode && d->Type!=EfiRuntimeServicesData)continue;
            if(d->PhysicalStart>start || d->NumberOfPages>(~(UINT64)0>>12))continue;
            UINT64 span=d->NumberOfPages<<12;
            UINT64 delta=start-d->PhysicalStart;
            if(delta<=span && bytes<=span-delta) {ok=1;break;}
        }
    }
    if(map && EFI_ERROR(bs->FreePool(map)))ok=0;
    return ok;
}

static void paths(EFI_BOOT_SERVICES *bs, EFI_HANDLE handle) {
    EFI_DEVICE_PATH_PROTOCOL *p=0;
    EFI_STATUS st=bs->HandleProtocol(handle,&PathGuid,(VOID **)&p);
    field("device_path_status",st);
    if(EFI_ERROR(st) || !p)return;
    UINTN consumed=0;
    for(UINTN node=0;node<64;node++) {
        UINTN n=(UINTN)p->Length[0]+((UINTN)p->Length[1]<<8);
        if(n<4 || n>256 || consumed+n>4096){text("device_path=refused_invalid_length\r\n");return;}
        text("path_node=");
        for(UINTN j=0;j<n;j++)hex(((UINT8 *)p)[j],2);
        text("\r\n");
        if(p->Type==0x7f && p->SubType==0xff)return;
        consumed+=n;p=(VOID *)((UINT8 *)p+n);
    }
    text("device_path=refused_too_many_nodes\r\n");
}

static EFI_STATUS collect(EFI_BOOT_SERVICES *bs, EFI_FILE_PROTOCOL *dir) {
    EFI_HANDLE *handles=0, target_handle=0;
    EFI_PCI_IO_PROTOCOL *target=0;
    UINTN count=0,matches=0;
    UINT32 config[64];
    EFI_STATUS st=bs->LocateHandleBuffer(ByProtocol,&PciGuid,0,&count,&handles);
    field("pci_enumeration_status",st);
    if(EFI_ERROR(st) || !handles)return EFI_ERROR(st)?st:EFI_DEVICE_ERROR;
    if(count>4096) {bs->FreePool(handles);return EFI_UNSUPPORTED;}
    for(UINTN i=0;i<count;i++) {
        EFI_PCI_IO_PROTOCOL *pci=0;
        UINTN seg=0,bus=0,dev=0,fn=0;
        UINT32 ids[2]={0};
        st=bs->HandleProtocol(handles[i],&PciGuid,(VOID **)&pci);
        if(EFI_ERROR(st) || !pci || !pci->GetLocation || !pci->Pci.Read)continue;
        st=pci->GetLocation(pci,&seg,&bus,&dev,&fn);
        if(EFI_ERROR(st))continue;
        st=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,1,&ids[0]);
        if(EFI_ERROR(st))continue;
        st=pci->Pci.Read(pci,EfiPciIoWidthUint32,0x2c,1,&ids[1]);
        if(EFI_ERROR(st))continue;
        text("pci=");location(seg,bus,dev,fn);text(" id=");hex(ids[0],8);text(" subsystem=");hex(ids[1],8);text("\r\n");
        tu102_pci_identity id={(uint32_t)seg,(uint32_t)bus,(uint32_t)dev,(uint32_t)fn,ids[0],ids[1]};
        if(tu102_identity_matches(&id)){target=pci;target_handle=handles[i];matches++;}
    }
    st=bs->FreePool(handles);
    field("target_matches",matches);
    if(EFI_ERROR(st))return st;
    if(matches!=1 || !target){text("target=absent_or_ambiguous\r\n");return EFI_NOT_FOUND;}
    zero_bytes(config,sizeof(config));
    st=target->Pci.Read(target,EfiPciIoWidthUint32,0,64,config);
    field("config_snapshot_status",st);
    if(EFI_ERROR(st))return st;
    /* Recheck identity after the full read rather than trusting enumeration. */
    if(config[0]!=TU102_VENDOR_DEVICE || config[11]!=TU102_SUBSYSTEM)return EFI_SECURITY_VIOLATION;
    if((config[2]>>24)!=3 || ((config[3]>>16)&0x7f))return EFI_UNSUPPORTED;
    field("pci_command",config[1]&0xffff);
    field("pci_bar0_raw",config[4]);
    field("pci_bar0_next_dword",config[5]);
    st=save_new(dir,L"PCI-CONFIG.BIN",config,sizeof(config));
    field("config_save_status",st);if(EFI_ERROR(st))return st;
    paths(bs,target_handle);
    if(LogFault)return EFI_VOLUME_FULL;

    field("firmware_cached_rom_bytes",target->RomSize);
    text("cached_rom_is_not_a_full_prom_capture=true\r\n");
    if(rom_in_host_ram(bs,target->RomImage,target->RomSize)) {
        VOID *copy=0;
        st=bs->AllocatePool(EfiLoaderData,(UINTN)target->RomSize,&copy);
        if(EFI_ERROR(st) || !copy)return EFI_ERROR(st)?st:EFI_OUT_OF_RESOURCES;
        copy_bytes(copy,target->RomImage,(UINTN)target->RomSize);
        st=save_new(dir,L"CACHED-ROM.BIN",copy,(UINTN)target->RomSize);
        EFI_STATUS freed=bs->FreePool(copy);
        field("cached_rom_save_status",st);
        if(EFI_ERROR(st))return st;
        if(EFI_ERROR(freed))return freed;
    } else {
        text("cached_rom=absent_or_not_in_bounded_host_ram; no_MMIO_fallback\r\n");
    }
#ifdef TU102_LIVE_PROM_CAPTURE
    st=capture_live_prom(bs,dir,target);
    field("live_prom_status",st);
    if(EFI_ERROR(st))return st;
#endif
    handles=0;count=0;
    st=bs->LocateHandleBuffer(ByProtocol,&GopGuid,0,&count,&handles);
    field("gop_enumeration_status",st);
    if(!EFI_ERROR(st) && handles) {
        if(count>64){bs->FreePool(handles);return EFI_UNSUPPORTED;}
        for(UINTN i=0;i<count;i++) {
            text("gop_handle_index=");hex(i,4);text("\r\n");paths(bs,handles[i]);
        }
        st=bs->FreePool(handles);if(EFI_ERROR(st))return st;
    }
    return LogFault?EFI_VOLUME_FULL:EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    if(!system || !system->BootServices)return EFI_INVALID_PARAMETER;
    EFI_BOOT_SERVICES *bs=system->BootServices;
    Used=0;LogFault=0;zero_bytes(Report,sizeof(Report));
    text("TU102 Windows port: READ-ONLY PREFLIGHT; NOT AN UNLOCK\r\n");
#ifdef TU102_LIVE_PROM_CAPTURE
    text("version=2; GPU_MMIO_reads=guarded; GPU_MMIO_writes=false\r\n");
    text("GPU_config_writes=false; DMA=false; firmware_execution=false\r\n");
#else
    text("version=1; GPU_MMIO_access=false; GPU_config_writes=false; DMA=false; firmware_execution=false\r\n");
#endif
    text("NVRAM_writes=false; boot_installation=false; Windows_changes=false\r\n");
    text("Target: 0000:03:00.0 / 10de:1e07 / subsystem 10de:12a4\r\n");
    field("uefi_revision",system->Hdr.Revision);
    EFI_FILE_PROTOCOL *dir=0;
    EFI_STATUS st=open_capture(image,bs,&dir);
    if(EFI_ERROR(st)) {
        if(system->ConOut && system->ConOut->OutputString)
            system->ConOut->OutputString(system->ConOut,L"TU102 preflight: launch from a removable FAT USB with TU102-PREFLIGHT-ONLY.TAG. No GPU accessed.\r\n");
        return st;
    }
    st=collect(bs,dir);
    field("collection_status",st);
    text("Tensor_unlock_attempted=false\r\n");
    text("Full native PROM and cold GPU state remain required; never substitute another board ROM.\r\n");
    EFI_STATUS saved=save_new(dir,L"REPORT.TXT",Report,Used);
    EFI_STATUS closed=dir->Close(dir);
    if(EFI_ERROR(saved))st=saved;
    if(EFI_ERROR(closed))st=closed;
    if(LogFault)st=EFI_VOLUME_FULL;
    if(system->ConOut && system->ConOut->OutputString)
        system->ConOut->OutputString(system->ConOut,
            EFI_ERROR(st)?L"TU102 preflight incomplete. Review the USB report. No unlock attempted.\r\n":
                          L"TU102 preflight saved to USB. No unlock attempted. Return to firmware boot menu.\r\n");
    return st;
}
