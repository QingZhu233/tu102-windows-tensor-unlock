/* CPU-only tests of the actual UEFI entry with mocked protocol services.
 * All PCI/MMIO write, BAR read, DMA and boot-mutation callbacks abort.
 */
#ifndef TU102_PREFLIGHT_SOURCE_INCLUDED
#include "preflight.c"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EFI_BOOT_SERVICES Services;
static EFI_SYSTEM_TABLE System;
static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static EFI_BLOCK_IO_PROTOCOL Block;
static EFI_BLOCK_IO_MEDIA Media;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_FILE_PROTOCOL Root, Tag, Dir, Existing, Files[5];
static EFI_PCI_IO_PROTOCOL Devices[2];
static EFI_DEVICE_PATH_PROTOCOL EndPath={0x7f,0xff,{4,0}};
static UINT8 Rom[512];
static UINT8 Disk[5][0x80000];
static UINTN DiskSize[5];
static UINT32 Config[2][64];
static int ImageToken, VolumeToken;
static UINTN PciCount, PciReads, Writes, Cases, ExistingDirs, MapCalls;
static int HasTag, IsRemovable, WrongLocation, Duplicate, FailConfig, ChangeConfig;
static int RomType, BadMap, ShortWrite, FailFlush, FailClose, ExistingOutput, BadGop;
static int CreatedDir, AbsentRom, WrongSub, WrongDevice, WrongClass, MapFail;

static int eq16(const CHAR16 *a,const CHAR16 *b) {
    while(*a && *a==*b){a++;b++;}return *a==*b;
}
static int guid_eq(const EFI_GUID *a,const EFI_GUID *b){return !memcmp(a,b,sizeof(*a));}
static EFI_STATUS EFIAPI alloc(EFI_MEMORY_TYPE t,UINTN n,VOID **p) {
    (void)t;*p=calloc(1,(size_t)n);return *p?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI release(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI get_map(UINTN *n,EFI_MEMORY_DESCRIPTOR *p,UINTN *key,UINTN *stride,UINT32 *ver) {
    MapCalls++;*key=1;*stride=sizeof(*p);*ver=1;
    if(MapFail)return EFI_DEVICE_ERROR;
    if(!p || *n<sizeof(*p)){*n=sizeof(*p);return EFI_BUFFER_TOO_SMALL;}
    memset(p,0,sizeof(*p));p->Type=(UINT32)RomType;
    p->PhysicalStart=(UINTN)Rom&~4095ULL;p->NumberOfPages=2;
    *n=sizeof(*p)+(BadMap?1:0);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE kind,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **h) {
    (void)kind;(void)key;
    if(guid_eq(guid,&PciGuid)) {
        *count=PciCount;*h=malloc((size_t)(PciCount?PciCount:1)*sizeof(**h));
        assert(*h);for(UINTN i=0;i<PciCount;i++)(*h)[i]=&Devices[i%2];return EFI_SUCCESS;
    }
    assert(guid_eq(guid,&GopGuid));
    *count=BadGop?65:0;*h=BadGop?calloc(65,sizeof(**h)):0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
    if(h==&ImageToken && guid_eq(g,&ImageGuid)){*p=&Loaded;return EFI_SUCCESS;}
    if(h==&VolumeToken && guid_eq(g,&BlockGuid)){Media.RemovableMedia=(BOOLEAN)IsRemovable;*p=&Block;return EFI_SUCCESS;}
    if(h==&VolumeToken && guid_eq(g,&FsGuid)){*p=&Fs;return EFI_SUCCESS;}
    if((h==&Devices[0] || h==&Devices[1]) && guid_eq(g,&PciGuid)){*p=h;return EFI_SUCCESS;}
    if((h==&Devices[0] || h==&Devices[1]) && guid_eq(g,&PathGuid)){*p=&EndPath;return EFI_SUCCESS;}
    *p=0;return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI get_location(EFI_PCI_IO_PROTOCOL *p,UINTN *s,UINTN *b,UINTN *d,UINTN *f) {
    assert(p==&Devices[0] || p==&Devices[1]);
    *s=*d=*f=0;*b=(p==&Devices[0])?(WrongLocation?4:3):(Duplicate?3:1);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI read_config(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN count,VOID *out) {
    assert(w==EfiPciIoWidthUint32 && off+count*4<=256);PciReads++;
    UINTN i=p==&Devices[0]?0:1;
    if(count==64 && FailConfig)return EFI_DEVICE_ERROR;
    memcpy(out,(UINT8 *)Config[i]+off,(size_t)(count*4));
    if(count==64 && ChangeConfig)((UINT32 *)out)[11]^=1;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI forbidden_config(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 o,UINTN n,VOID *v) {
    (void)p;(void)w;(void)o;(void)n;(void)v;assert(!"GPU configuration write forbidden");abort();
}
static EFI_STATUS EFIAPI forbidden_bar(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT8 b,UINT64 o,UINTN n,VOID *v) {
    (void)p;(void)w;(void)b;(void)o;(void)n;(void)v;assert(!"GPU BAR access forbidden");abort();
}
static EFI_STATUS EFIAPI open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *f,EFI_FILE_PROTOCOL **p){assert(f==&Fs);*p=&Root;return EFI_SUCCESS;}
static int slot(CHAR16 *name) {
    if(eq16(name,L"REPORT.TXT"))return 0;
    if(eq16(name,L"PCI-CONFIG.BIN"))return 1;
    if(eq16(name,L"CACHED-ROM.BIN"))return 2;
    if(eq16(name,L"LIVE-TRACE.TXT"))return 3;
    if(eq16(name,L"TRANSACTION.TXT"))return 3;
    if(eq16(name,L"LIVE-PROM.BIN"))return 4;
    assert(!"unexpected output filename");return -1;
}
static EFI_STATUS EFIAPI file_open(EFI_FILE_PROTOCOL *self,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 mode,UINT64 attr) {
    *out=0;
    if(self==&Root && eq16(name,L"TU102-PREFLIGHT-ONLY.TAG")) {
        assert(mode==EFI_FILE_MODE_READ && !attr);
        if(!HasTag)return EFI_NOT_FOUND;
        *out=&Tag;return EFI_SUCCESS;
    }
    if(self==&Root) {
        const CHAR16 prefix[]=L"TU102-CAPTURE-";
        for(UINTN i=0;i<13;i++)assert(name[i]==prefix[i]);
        assert(name[13]=='-' && name[14]>='0' && name[14]<='9' && name[15]>='0' && name[15]<='9' && !name[16]);
        UINTN n=(name[14]-'0')*10+name[15]-'0';
        if(mode==EFI_FILE_MODE_READ){if(n<ExistingDirs){*out=&Existing;return EFI_SUCCESS;}return EFI_NOT_FOUND;}
        assert(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE) && attr==EFI_FILE_DIRECTORY);
        assert(n==ExistingDirs);CreatedDir=1;*out=&Dir;return EFI_SUCCESS;
    }
    assert(self==&Dir && !attr);int i=slot(name);
    if(mode==EFI_FILE_MODE_READ){if(ExistingOutput && i==1){*out=&Existing;return EFI_SUCCESS;}return EFI_NOT_FOUND;}
    assert(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE));*out=&Files[i];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI file_close(EFI_FILE_PROTOCOL *f){return FailClose && f==&Files[1]?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI file_write(EFI_FILE_PROTOCOL *f,UINTN *n,VOID *p) {
    int i=(int)(f-Files);assert(i>=0 && i<5 && *n<=sizeof(Disk[i])-DiskSize[i]);Writes++;
    if(ShortWrite && i==1)*n/=2;
    memcpy(Disk[i]+DiskSize[i],p,(size_t)*n);DiskSize[i]+=*n;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI file_flush(EFI_FILE_PROTOCOL *f){return FailFlush && f==&Files[1]?EFI_DEVICE_ERROR:EFI_SUCCESS;}

static void setup(void) {
    memset(&Services,0,sizeof(Services));memset(&System,0,sizeof(System));
    memset(&Root,0,sizeof(Root));memset(&Tag,0,sizeof(Tag));memset(&Dir,0,sizeof(Dir));
    memset(&Existing,0,sizeof(Existing));memset(Files,0,sizeof(Files));
    memset(Devices,0,sizeof(Devices));memset(Config,0,sizeof(Config));
    memset(Disk,0,sizeof(Disk));memset(DiskSize,0,sizeof(DiskSize));memset(Rom,0x55,sizeof(Rom));
    memset(&Loaded,0,sizeof(Loaded));memset(&Media,0,sizeof(Media));
    Services.AllocatePool=alloc;Services.FreePool=release;Services.GetMemoryMap=get_map;
    Services.LocateHandleBuffer=locate;Services.HandleProtocol=handle;
    System.BootServices=&Services;System.Hdr.Revision=0x2001f;
    Loaded.DeviceHandle=&VolumeToken;Fs.OpenVolume=open_volume;Block.Media=&Media;
    Media.MediaPresent=TRUE;HasTag=IsRemovable=1;
    Root.Open=Dir.Open=file_open;
    Root.Close=Dir.Close=Tag.Close=Existing.Close=file_close;
    for(unsigned i=0;i<5;i++){Files[i].Close=file_close;Files[i].Write=file_write;Files[i].Flush=file_flush;}
    for(unsigned i=0;i<2;i++) {
        Devices[i].GetLocation=get_location;Devices[i].Pci.Read=read_config;Devices[i].Pci.Write=forbidden_config;
        Devices[i].Mem.Read=Devices[i].Mem.Write=forbidden_bar;Devices[i].Io.Read=Devices[i].Io.Write=forbidden_bar;
        Config[i][2]=0x030000a1;Config[i][4]=0xd0000000;
    }
    Config[0][0]=TU102_VENDOR_DEVICE;Config[0][11]=TU102_SUBSYSTEM;
    Config[1][0]=0x2c0510de;Config[1][11]=0x41831458;
    Devices[0].RomImage=Rom;Devices[0].RomSize=sizeof(Rom);
    PciCount=2;PciReads=Writes=MapCalls=ExistingDirs=0;
    RomType=EfiBootServicesData;
    WrongLocation=Duplicate=FailConfig=ChangeConfig=BadMap=ShortWrite=FailFlush=FailClose=ExistingOutput=BadGop=CreatedDir=AbsentRom=WrongSub=WrongDevice=WrongClass=MapFail=0;
}
static EFI_STATUS run(void){Cases++;return efi_main(&ImageToken,&System);}

int main(void) {
    setup();assert(run()==EFI_SUCCESS);assert(DiskSize[1]==256 && DiskSize[2]==sizeof(Rom));
    assert(!memcmp(Disk[1],Config[0],256) && !memcmp(Disk[2],Rom,sizeof(Rom)));
    assert(strstr((char *)Disk[0],"Tensor_unlock_attempted=false"));
    assert(strstr((char *)Disk[0],"target_matches=0x0000000000000001"));
    setup();IsRemovable=0;assert(run()==EFI_UNSUPPORTED && !PciReads && !Writes);
    setup();HasTag=0;assert(run()==EFI_ACCESS_DENIED && !PciReads && !Writes);
    setup();WrongLocation=1;assert(run()==EFI_NOT_FOUND && !DiskSize[1] && !DiskSize[2]);
    setup();Config[0][11]^=1;assert(run()==EFI_NOT_FOUND && !DiskSize[1] && !DiskSize[2]);
    setup();Config[0][0]=0x2c0510de;assert(run()==EFI_NOT_FOUND && !DiskSize[1] && !DiskSize[2]);
    setup();Duplicate=1;memcpy(Config[1],Config[0],sizeof(Config[0]));assert(run()==EFI_NOT_FOUND && !DiskSize[1]);
    setup();PciCount=0;assert(run()==EFI_NOT_FOUND && !DiskSize[1]);
    setup();FailConfig=1;assert(run()==EFI_DEVICE_ERROR && !DiskSize[1]);
    setup();ChangeConfig=1;assert(run()==EFI_SECURITY_VIOLATION && !DiskSize[1]);
    setup();Config[0][2]=0x02000000;assert(run()==EFI_UNSUPPORTED && !DiskSize[1]);
    setup();Config[0][3]=0x10000;assert(run()==EFI_UNSUPPORTED && !DiskSize[1]);
    setup();RomType=EfiMemoryMappedIO;assert(run()==EFI_SUCCESS && DiskSize[1] && !DiskSize[2]);
    setup();Devices[0].RomSize=ROM_LIMIT+1;assert(run()==EFI_SUCCESS && !DiskSize[2] && !MapCalls);
    setup();Devices[0].RomImage=0;assert(run()==EFI_SUCCESS && !DiskSize[2] && !MapCalls);
    setup();BadMap=1;assert(run()==EFI_SUCCESS && !DiskSize[2]);
    setup();MapFail=1;assert(run()==EFI_SUCCESS && !DiskSize[2]);
    setup();ShortWrite=1;assert(run()==EFI_DEVICE_ERROR && !DiskSize[2]);
    setup();FailFlush=1;assert(run()==EFI_DEVICE_ERROR && !DiskSize[2]);
    setup();FailClose=1;assert(run()==EFI_DEVICE_ERROR && !DiskSize[2]);
    setup();ExistingOutput=1;assert(run()==EFI_ACCESS_DENIED && !DiskSize[1]);
    setup();ExistingDirs=99;assert(run()==EFI_SUCCESS && CreatedDir);
    setup();ExistingDirs=100;assert(run()==EFI_ACCESS_DENIED && !PciReads && !CreatedDir);
    setup();BadGop=1;assert(run()==EFI_UNSUPPORTED);
    setup();assert(efi_main(0,0)==EFI_INVALID_PARAMETER);Cases++;
    printf("PASS %llu actual UEFI-entry host simulations; no hardware access\n",(unsigned long long)Cases);
    return 0;
}
