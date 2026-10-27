/* Windows CPU-only integration of the real local core and EFI DMA adapter.
 * Protocol callbacks below are mocks. No real GPU or UEFI service is used.
 */
#include <Uefi.h>
#define main included_core_suite_main
#include "test_core.c"
#undef main
#include "efi_dma.h"

static EFI_BOOT_SERVICES services;
static EFI_PCI_IO_PROTOCOL pci;
static TuEfi port;
static _Alignas(4096) uint8_t backing[TX_POOL_SIZE];
static UINT64 attrs,attrs_supported,map_address;
static UINT16 command;
static unsigned opens,closes,allocs,frees,maps,unmaps,enable_master,disable_master,flushes;
static int wrong_id,wrong_sub,wrong_bdf,wrong_bar,deny_owner,different_interface;
static int fail_alloc,fail_map,map_error_token,short_map,no_map_token,fail_unmap,fail_free;
static int fail_dac,fail_enable,fail_disable,fail_close,fail_config,change_config;
static int wrong_attrs,stuck_bme,partial_enable,dirty_dac,misalign_pool;
static int active_owner,owner_api_fail,tpl_raised;
static unsigned dac_enable_calls;
static int image_token,device_token;

static EFI_TPL EFIAPI raise_mock(EFI_TPL t){assert(t==TPL_NOTIFY && !tpl_raised);tpl_raised=1;return TPL_APPLICATION;}
static VOID EFIAPI restore_mock(EFI_TPL t){assert(t==TPL_APPLICATION && tpl_raised);tpl_raised=0;}
static EFI_STATUS EFIAPI free_pool_mock(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI protocols_mock(EFI_HANDLE h,EFI_GUID ***ids,UINTN *count) {
    assert(h==&device_token && tpl_raised);
    if(owner_api_fail)return EFI_DEVICE_ERROR;
    *count=1;*ids=malloc(sizeof(**ids));assert(*ids);(*ids)[0]=&port.pci_guid;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI records_mock(EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *n) {
    assert(h==&device_token && g==&port.pci_guid && tpl_raised);
    *n=active_owner?1:0;*out=active_owner?calloc(1,sizeof(**out)):0;
    if(active_owner)(*out)[0].Attributes=(UINT32)active_owner;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI open_mock(EFI_HANDLE h,EFI_GUID *guid,VOID **p,EFI_HANDLE agent,EFI_HANDLE controller,UINT32 flags) {
    assert(h==&device_token && guid==&port.pci_guid && agent==&image_token && !controller);
    assert(flags==EFI_OPEN_PROTOCOL_EXCLUSIVE);opens++;
    assert(tpl_raised && !active_owner);
    if(deny_owner)return EFI_ACCESS_DENIED;
    *p=different_interface?0:&pci;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_mock(EFI_HANDLE h,EFI_GUID *guid,EFI_HANDLE agent,EFI_HANDLE c) {
    assert(h==&device_token && guid==&port.pci_guid && agent==&image_token && !c);
    closes++;return fail_close?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI location_mock(EFI_PCI_IO_PROTOCOL *p,UINTN *s,UINTN *b,UINTN *d,UINTN *f) {
    assert(p==&pci);*s=*d=*f=0;*b=wrong_bdf?1:3;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI config_mock(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN count,VOID *out) {
    assert(p==&pci && w==EfiPciIoWidthUint32 && count==1);
    if(fail_config)return EFI_DEVICE_ERROR;
    switch(off) {
    case 0:*(UINT32 *)out=wrong_id?0x2c0510de:TU102_VENDOR_DEVICE;break;
    case 0x2c:*(UINT32 *)out=wrong_sub?0x866a1043:TU102_SUBSYSTEM;break;
    case 4:*(UINT32 *)out=command|(change_config?8:0);break;
    case 0x10:*(UINT32 *)out=wrong_bar?0xd0000000:0xdd000000;break;
    default:abort();
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI attrs_mock(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 mask,UINT64 *out) {
    assert(p==&pci);
    switch(op) {
    case EfiPciIoAttributeOperationSupported:assert(!mask && out);*out=attrs_supported;break;
    case EfiPciIoAttributeOperationGet:assert(!mask && out);*out=attrs|(wrong_attrs?0x100:0);break;
    case EfiPciIoAttributeOperationEnable:
        assert(!out && (mask==EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE || mask==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER));
        if(mask==EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE) {
            dac_enable_calls++;attrs|=mask;if(fail_dac)return EFI_DEVICE_ERROR;
        } else {
            enable_master++;
            if(!fail_enable || partial_enable){attrs|=mask;command|=4;}
            if(fail_enable)return EFI_DEVICE_ERROR;
        }
        break;
    case EfiPciIoAttributeOperationDisable:
        assert(!out && (mask==EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE || mask==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER));
        if(mask==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) {
            disable_master++;
            if(fail_disable)return EFI_DEVICE_ERROR;
            if(!stuck_bme){command&=(UINT16)~4u;attrs&=~mask;}
        } else if(!dirty_dac)attrs&=~mask;
        break;
    default:abort();
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI alloc_mock(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE type,EFI_MEMORY_TYPE mem,UINTN pages,VOID **cpu,UINT64 flags) {
    assert(p==&pci && type==AllocateAnyPages && mem==EfiBootServicesData && pages==TX_POOL_SIZE/4096);
    /* Match EDK2's PciIoAllocateBuffer behavior: a DAC argument is rejected.
     * The supported/enabled device attribute selects64-bit root mapping. */
    if(flags&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE)return EFI_UNSUPPORTED;
    assert(!flags && (attrs&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE) && !(command&4));allocs++;
    if(fail_alloc){*cpu=0;return EFI_OUT_OF_RESOURCES;}
    *cpu=backing+(misalign_pool?1:0);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_mock(EFI_PCI_IO_PROTOCOL *p,UINTN pages,VOID *cpu) {
    assert(p==&pci && pages==TX_POOL_SIZE/4096 && cpu==(VOID *)(backing+(misalign_pool?1:0)));
    assert(!(command&4));frees++;return fail_free?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI map_mock(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION op,VOID *cpu,UINTN *bytes,EFI_PHYSICAL_ADDRESS *dev,VOID **mapping) {
    assert(p==&pci && op==EfiPciIoOperationBusMasterCommonBuffer && cpu==backing && *bytes==TX_POOL_SIZE);
    assert((attrs&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE) && !(command&4));maps++;
    if(fail_map){*mapping=map_error_token?(VOID *)&maps:0;return EFI_DEVICE_ERROR;}
    *dev=map_address;*mapping=no_map_token?0:(VOID *)&maps;
    if(short_map)*bytes/=2;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unmap_mock(EFI_PCI_IO_PROTOCOL *p,VOID *m) {
    assert(p==&pci && (m==&maps || (no_map_token && !m)) && !(command&4));unmaps++;
    return fail_unmap?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI read_mock(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT8 bar,UINT64 off,UINTN count,VOID *out) {
    assert(p==&pci && width==EfiPciIoWidthUint32 && bar==0 && count==1);
    return model_read(&model,(uint32_t)off,out)?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write_mock(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT8 bar,UINT64 off,UINTN count,VOID *in) {
    assert(p==&pci && width==EfiPciIoWidthUint32 && bar==0 && count==1);
    assert(command==7 && port.owned && port.mapped && port.profile_verified);
    return model_write(&model,(uint32_t)off,*(UINT32 *)in)?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI stall_mock(UINTN us){return model_delay(&model,(uint32_t)us)?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI flush_mock(EFI_PCI_IO_PROTOCOL *p){assert(p==&pci);flushes++;return EFI_SUCCESS;}
static int guard_mock(void *ctx,uint32_t off,uint32_t want,uint32_t actual,uint32_t mask) {
    assert(ctx==&port);return model_guard_note(&model,off,want,actual,mask);
}

static void fixture(void) {
    memset(&port,0,sizeof(port));memset(&pci,0,sizeof(pci));memset(&services,0,sizeof(services));
    tx.pool=backing;setup();
    command=3;attrs=EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_IO;
    attrs_supported=attrs|EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
    map_address=0x800000000ULL;
    opens=closes=allocs=frees=maps=unmaps=enable_master=disable_master=flushes=0;
    wrong_id=wrong_sub=wrong_bdf=wrong_bar=deny_owner=different_interface=0;
    fail_alloc=fail_map=map_error_token=short_map=no_map_token=fail_unmap=fail_free=0;
    fail_dac=fail_enable=fail_disable=fail_close=fail_config=change_config=0;
    wrong_attrs=stuck_bme=partial_enable=dirty_dac=misalign_pool=0;
    active_owner=owner_api_fail=tpl_raised=0;dac_enable_calls=0;
    services.OpenProtocol=open_mock;services.CloseProtocol=close_mock;services.Stall=stall_mock;
    services.RaiseTPL=raise_mock;services.RestoreTPL=restore_mock;services.FreePool=free_pool_mock;
    services.ProtocolsPerHandle=protocols_mock;services.OpenProtocolInformation=records_mock;
    pci.GetLocation=location_mock;pci.Pci.Read=config_mock;pci.Attributes=attrs_mock;
    pci.AllocateBuffer=alloc_mock;pci.FreeBuffer=free_mock;pci.Map=map_mock;pci.Unmap=unmap_mock;
    pci.Mem.Read=read_mock;pci.Mem.Write=write_mock;pci.Flush=flush_mock;
    port.bs=&services;port.pci=&pci;port.handle=&device_token;port.image=&image_token;
    port.profile_verified=TRUE;port.tx=tx;port.tx.pool=0;port.tx.device_address=0;
    port.log_stage=model_event;port.log_context=&model;
    te_bind(&port);port.tx.guard_note=guard_mock;model.tx=&port.tx;
}
static void prepared(void) {
    fixture();assert(te_claim(&port)==EFI_SUCCESS && te_allocate(&port)==EFI_SUCCESS);
    assert(tx_prepare(&port.tx,booter,signature,boot)==TX_OK);
}
static void check_no_mutation(void) {
    assert(!model.writes && !model.fw_starts && !model.booter_starts);
}
#ifndef TU102_DMA_TEST_MAIN
#define TU102_DMA_TEST_MAIN main
#endif
int TU102_DMA_TEST_MAIN(void) {
    load("captures/capture-04/NATIVE-FWSEC.BIN",native,sizeof(native));
    load("assets/upstream-cmp50hx-efi/bl_gsp_tu102.bin",generic,sizeof(generic));
    load("assets/upstream-cmp50hx-efi/booter_ucode_prod.bin",booter,sizeof(booter));
    load("assets/upstream-cmp50hx-efi/v67_payload.bin",signature,sizeof(signature));
    load("assets/upstream-cmp50hx-efi/gsp_rm_boot_dbg.bin",boot,sizeof(boot));
    unsigned tests=0;
    prepared();assert(tx_guards(&port.tx)==TX_OK);assert(te_enable_master(&port)==EFI_SUCCESS);
    assert(tx_run(&port.tx)==TX_OK && port.tx.unlocked && port.tx.quiescent);tests++;
    assert(te_release(&port)==EFI_SUCCESS && command==3 && !port.owned && !port.mapped);
    assert(frees==1 && unmaps==1 && closes==1 && !port.retained);tests++;
    assert(te_release(&port)==EFI_INVALID_PARAMETER);tests++;
    for(unsigned bad=0;bad<8;bad++) {
        fixture();
        switch(bad){case 0:wrong_id=1;break;case 1:wrong_sub=1;break;case 2:wrong_bdf=1;break;
            case 3:wrong_bar=1;break;case 4:command=7;break;case 5:deny_owner=1;break;
            case 6:fail_config=1;break;case 7:port.profile_verified=FALSE;break;}
        assert(EFI_ERROR(te_claim(&port)) && !port.owned && !allocs);check_no_mutation();tests++;
    }
    fixture();different_interface=1;assert(EFI_ERROR(te_claim(&port)) && port.owned && port.fault);
    assert(te_release(&port)==EFI_SUCCESS);check_no_mutation();tests++;
    for(unsigned bad=0;bad<11;bad++) {
        fixture();assert(te_claim(&port)==EFI_SUCCESS);
        switch(bad) {
        case 0:attrs_supported&=~EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE;break;
        case 1:fail_dac=1;break;case 2:fail_alloc=1;break;case 3:misalign_pool=1;break;
        case 4:fail_map=1;break;case 5:short_map=1;break;case 6:map_address=0xff000000;break;
        case 7:map_address++;break;case 8:map_address=0x10000000000ULL;break;
        case 9:fail_map=map_error_token=1;break;case 10:attrs_supported&=~EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;break;
        }
        assert(EFI_ERROR(te_allocate(&port)));assert(te_release(&port)==EFI_SUCCESS);
        assert(!port.pool && !port.mapped && !port.owned);check_no_mutation();tests++;
    }
    fixture();assert(te_claim(&port)==EFI_SUCCESS);no_map_token=1;
    assert(te_allocate(&port)==EFI_SUCCESS);assert(te_release(&port)==EFI_SUCCESS && !port.retained && frees==1 && unmaps==1);tests++;
    for(unsigned fail=0;fail<6;fail++) {
        prepared();assert(te_enable_master(&port)==EFI_SUCCESS);
        port.tx.mutated=port.tx.quiescent=1;
        switch(fail){case 0:fail_unmap=1;break;case 1:fail_free=1;break;case 2:fail_disable=1;break;
            case 3:stuck_bme=1;break;case 4:dirty_dac=1;break;case 5:fail_close=1;break;}
        assert(EFI_ERROR(te_release(&port)) && port.retained);tests++;
        if(fail==0)assert(!frees);
        if(fail==2 || fail==3)assert(!unmaps && !frees);
    }
    prepared();assert(te_enable_master(&port)==EFI_SUCCESS);port.tx.mutated=1;port.tx.quiescent=0;
    assert(te_release(&port)==EFI_ACCESS_DENIED && port.retained && !unmaps && !frees && !closes);tests++;
    assert(command==3); /* containment without freeing uncertain DMA backing */
    for(unsigned partial=0;partial<2;partial++) {
        prepared();fail_enable=1;partial_enable=(int)partial;
        assert(EFI_ERROR(te_enable_master(&port)));
        assert(te_release(&port)==EFI_SUCCESS && command==3 && frees==1);tests++;
    }
    prepared();assert(te_enable_master(&port)==EFI_SUCCESS);
    model.fail_write=1;assert(tx_run(&port.tx)==TX_IO && port.tx.mutated && port.fault);
    assert(te_release(&port)==EFI_ACCESS_DENIED && port.retained && !frees && command==3);tests++;
    prepared();assert(te_write(&port,0x409664,0x88888888)!=0 && !model.writes);tests++;
    prepared();assert(te_enable_master(&port)==EFI_SUCCESS);
    assert(te_write(&port,0x409664,0x88888888)!=0 && !model.writes);tests++;
    assert(te_release(&port)==EFI_SUCCESS);
    prepared();assert(te_enable_master(&port)==EFI_SUCCESS);
    port.fault=1;assert(te_write(&port,0x1103c0,1)!=0 && !model.writes);tests++;
    assert(te_release(&port)==EFI_SUCCESS);
    const UINT32 disallowed[]={EFI_OPEN_PROTOCOL_BY_DRIVER,EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER,
        EFI_OPEN_PROTOCOL_EXCLUSIVE,EFI_OPEN_PROTOCOL_BY_DRIVER|EFI_OPEN_PROTOCOL_EXCLUSIVE};
    for(unsigned i=0;i<sizeof(disallowed)/sizeof(disallowed[0]);i++) {
        fixture();active_owner=(int)disallowed[i];
        assert(te_claim(&port)==EFI_ACCESS_DENIED && !opens && !port.owned && !tpl_raised);tests++;
        check_no_mutation();
    }
    fixture();owner_api_fail=1;assert(te_claim(&port)==EFI_DEVICE_ERROR && !opens && !tpl_raised);tests++;
    fixture();assert(te_claim(&port)==EFI_SUCCESS);attrs_supported&=~EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
    assert(te_allocate(&port)==EFI_UNSUPPORTED && !strcmp(port.dma_step,"supported-attributes") && !allocs);
    assert(te_release(&port)==EFI_SUCCESS);tests++;
    fixture();assert(te_claim(&port)==EFI_SUCCESS);no_map_token=1;map_address=0x61d8e000;
    assert(te_allocate(&port)==EFI_UNSUPPORTED);
    assert(te_release(&port)==EFI_SUCCESS && frees==1 && unmaps==1 && !port.retained);tests++;
    fixture();assert(te_claim(&port)==EFI_SUCCESS);no_map_token=1;fail_unmap=1;
    assert(te_allocate(&port)==EFI_SUCCESS);assert(EFI_ERROR(te_release(&port)) && !frees && port.retained);tests++;
    fixture();port.allocation_probe=TRUE;port.original_command=3;attrs|=EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
    assert(te_allocate(&port)==EFI_SUCCESS && !opens);
    assert(te_enable_master(&port)==EFI_ACCESS_DENIED && te_write(&port,0x1103c0,1)!=0 && !model.writes);
    assert(te_release(&port)==EFI_SUCCESS && !closes);tests++;
    fixture();port.allocation_probe=TRUE;port.original_command=3;
    assert(te_allocate(&port)==EFI_UNSUPPORTED && !allocs && !dac_enable_calls);
    assert(te_release(&port)==EFI_SUCCESS);tests++;
    fixture();assert(te_claim(&port)==EFI_SUCCESS);fail_alloc=1;
    assert(te_allocate(&port)==EFI_OUT_OF_RESOURCES && !strcmp(port.dma_step,"AllocateBuffer") && port.dma_allocate_status==EFI_OUT_OF_RESOURCES);
    assert(te_release(&port)==EFI_SUCCESS);tests++;
    fixture();assert(te_claim(&port)==EFI_SUCCESS);short_map=1;
    assert(te_allocate(&port)==EFI_UNSUPPORTED && !strcmp(port.dma_step,"validate-returned-mapping") && port.dma_mapped_bytes==TX_POOL_SIZE/2);
    assert(te_release(&port)==EFI_SUCCESS);tests++;
    printf("PASS %u Windows EFI/core/DMA mock integration cases; no hardware\n",tests);
    return 0;
}
