/* Local GOP topology validation. Reads protocols only; never sets a mode.
 * A pathless GOP may be a console alias, not another physical GPU. Unknown
 * Active aliases need an identical valid framebuffer. A zero-framebuffer
 * console with SizeOfInfo=0 needs positive console-protocol evidence.
 */
#ifndef TU102_DISPLAY_H
#define TU102_DISPLAY_H
static int td_known_path(const EFI_DEVICE_PATH_PROTOCOL *path) {
    /* Exact 5070 Ti route recorded in the local preflight, then ACPI ADR.
     * Compare node lengths before reading each complete node. */
    static const UINT8 expected[]={
        2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,
        1,1,6,0,1,1, 1,1,6,0,0,0,
        2,3,8,0,0x40,3,1,0x80, 0x7f,0xff,4,0};
    UINTN at=0;
    while(at<sizeof(expected)) {
        const UINT8 *b=(const UINT8 *)path+at;
        UINTN bytes=b[2]+((UINTN)b[3]<<8);
        UINTN want=expected[at+2]+((UINTN)expected[at+3]<<8);
        if(bytes!=want)return 0;
        for(UINTN j=0;j<bytes;j++)if(b[j]!=expected[at+j])return 0;
        at+=bytes;
    }
    return 1;
}
static int td_empty_path(const EFI_DEVICE_PATH_PROTOCOL *path) {
    return path && path->Type==0x7f && path->SubType==0xff &&
        path->Length[0]==4 && path->Length[1]==0;
}
static int td_framebuffer(EFI_GRAPHICS_OUTPUT_PROTOCOL *g) {
    if(!g || !g->Mode || !g->Mode->Info ||
       g->Mode->SizeOfInfo<sizeof(*g->Mode->Info))return 0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m=g->Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i=m->Info;
    if(!m->FrameBufferBase || !m->FrameBufferSize ||
       i->PixelFormat>=PixelBltOnly || !i->HorizontalResolution ||
       !i->VerticalResolution || i->PixelsPerScanLine<i->HorizontalResolution ||
       i->HorizontalResolution>32768 || i->VerticalResolution>32768 ||
       i->PixelsPerScanLine>65536)return 0;
    UINT64 minimum=(UINT64)i->PixelsPerScanLine*i->VerticalResolution*4;
    return minimum<=m->FrameBufferSize &&
        m->FrameBufferBase<=~(UINT64)0-m->FrameBufferSize;
}
static int td_alias(EFI_GRAPHICS_OUTPUT_PROTOCOL *a,EFI_GRAPHICS_OUTPUT_PROTOCOL *b) {
    if(!td_framebuffer(a) || !td_framebuffer(b))return 0;
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *x=a->Mode->Info,*y=b->Mode->Info;
    return a->Mode->FrameBufferBase==b->Mode->FrameBufferBase &&
        a->Mode->FrameBufferSize==b->Mode->FrameBufferSize &&
        x->Version==y->Version && x->HorizontalResolution==y->HorizontalResolution &&
        x->VerticalResolution==y->VerticalResolution && x->PixelFormat==y->PixelFormat &&
        x->PixelsPerScanLine==y->PixelsPerScanLine &&
        x->PixelInformation.RedMask==y->PixelInformation.RedMask &&
        x->PixelInformation.GreenMask==y->PixelInformation.GreenMask &&
        x->PixelInformation.BlueMask==y->PixelInformation.BlueMask &&
        x->PixelInformation.ReservedMask==y->PixelInformation.ReservedMask;
}
static EFI_GUID TdTextOutGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static int td_console_link(TuEfi *p,EFI_HANDLE physical,EFI_HANDLE alias) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out=0;
    EFI_STATUS st=p->bs->HandleProtocol(alias,&TdTextOutGuid,(VOID **)&out);
    field("alias_text_output_status",st);
    if(!EFI_ERROR(st) && out && (out==TxSystem->ConOut || out==TxSystem->StdErr)) {
        text("console_identity=current_text_output_protocol\r\n");return 1;
    }
    if(EFI_ERROR(st) && st!=EFI_NOT_FOUND && st!=EFI_UNSUPPORTED)return -1;
    /* A console splitter has a virtual child handle on a physical console's
     * protocol. Verify BY_CHILD_CONTROLLER and its matching BY_DRIVER owner,
     * without opening, disconnecting or binding any driver ourselves. */
    if(!p->bs->ProtocolsPerHandle || !p->bs->OpenProtocolInformation)return 0;
    EFI_GUID **protocols=0;UINTN count=0;int found=0;
    st=p->bs->ProtocolsPerHandle(physical,&protocols,&count);
    field("console_parent_protocol_status",st);field("console_parent_protocol_count",count);
    if(EFI_ERROR(st) || !protocols || !count || count>128) {
        if(protocols)p->bs->FreePool(protocols);
        return -1;
    }
    for(UINTN i=0;i<count && !found;i++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *r=0;UINTN n=0;
        st=p->bs->OpenProtocolInformation(physical,protocols[i],&r,&n);
        field("console_parent_open_info_status",st);
        if(EFI_ERROR(st) || n>256 || (n && !r))found=-1;
        for(UINTN j=0;!found && j<n;j++) {
            if(r[j].Attributes!=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER ||
               r[j].ControllerHandle!=alias || !r[j].AgentHandle)continue;
            for(UINTN k=0;k<n;k++) {
                UINT32 a=r[k].Attributes;
                if(r[k].AgentHandle==r[j].AgentHandle &&
                   (a==EFI_OPEN_PROTOCOL_BY_DRIVER ||
                    a==(EFI_OPEN_PROTOCOL_BY_DRIVER|EFI_OPEN_PROTOCOL_EXCLUSIVE))) {
                    found=1;text("console_identity=physical_console_driver_child\r\n");break;
                }
            }
        }
        if(r && EFI_ERROR(p->bs->FreePool(r)))found=-1;
    }
    if(EFI_ERROR(p->bs->FreePool(protocols)))found=-1;
    return found;
}
static EFI_STATUS experiment_display(TuEfi *p) {
    EFI_HANDLE *handles=0;UINTN count=0,physical=0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *actual=0,*gops[16]={0};
    EFI_HANDLE physical_handle=0;
    EFI_HANDLE aliases[16]={0};UINTN alias_count=0;
    EFI_STATUS st=p->bs->LocateHandleBuffer(ByProtocol,&GopGuid,0,&count,&handles);
    field("gop_enumeration_status",st);field("gop_count",count);
    if(EFI_ERROR(st)){if(handles)p->bs->FreePool(handles);return st;}
    if(!handles || !count || count>16){if(handles)p->bs->FreePool(handles);return EFI_UNSUPPORTED;}
    for(UINTN i=0;i<count;i++) {
        field("gop_index",i);field("gop_is_ConsoleOutHandle",handles[i]==TxSystem->ConsoleOutHandle);
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=0;
        EFI_STATUS gs=p->bs->HandleProtocol(handles[i],&GopGuid,(VOID **)&gop);
        field("gop_protocol_status",gs);
        if(!EFI_ERROR(gs) && gop && gop->Mode) {
            field("gop_max_mode",gop->Mode->MaxMode);
            field("gop_current_mode",gop->Mode->Mode);
            field("gop_info_present",gop->Mode->Info!=0);
            field("gop_info_bytes",gop->Mode->SizeOfInfo);
            field("gop_framebuffer_base",gop->Mode->FrameBufferBase);
            field("gop_framebuffer_bytes",gop->Mode->FrameBufferSize);
            if(gop->Mode->Info && gop->Mode->SizeOfInfo>=sizeof(*gop->Mode->Info)) {
                field("gop_width",gop->Mode->Info->HorizontalResolution);
                field("gop_height",gop->Mode->Info->VerticalResolution);
                field("gop_pixel_format",gop->Mode->Info->PixelFormat);
            }
        }
        EFI_DEVICE_PATH_PROTOCOL *path=0;
        st=p->bs->HandleProtocol(handles[i],&PathGuid,(VOID **)&path);
        field("gop_path_status",st);
        if(st==EFI_UNSUPPORTED || st==EFI_NOT_FOUND || (!EFI_ERROR(st) && (!path || td_empty_path(path)))) {
            aliases[alias_count]=handles[i];gops[alias_count++]=EFI_ERROR(gs)?0:gop;
            text("gop_kind=pathless_console_candidate\r\n");st=EFI_SUCCESS;continue;
        }
        if(EFI_ERROR(st))break;
        paths(p->bs,handles[i]);
        if(!td_known_path(path)) {
            text("display_refusal=unrecognized_physical_path\r\n");st=EFI_UNSUPPORTED;break;
        }
        physical++;actual=EFI_ERROR(gs)?0:gop;physical_handle=handles[i];
        text("gop_kind=recorded_5070Ti_physical_path\r\n");
    }
    if(!EFI_ERROR(st) && physical!=1) {
        text("display_refusal=physical_display_count\r\n");st=EFI_UNSUPPORTED;
    }
    for(UINTN i=0;!EFI_ERROR(st) && i<alias_count;i++) {
        /* Preserve the original recognized system-console allowance. An
         * additional pathless handle needs independent framebuffer evidence. */
        if(aliases[i]==TxSystem->ConsoleOutHandle) {
            text("gop_alias=system_console\r\n");continue;
        }
        if(!td_alias(actual,gops[i])) {
            /* Capture09 has non-NULL Info but SizeOfInfo=0. Never dereference
             * that Info pointer. These numeric fields alone do NOT prove a
             * console: require an actual text-output identity or driver-child
             * relationship, plus no PCI_IO and a valid known physical display. */
            EFI_GRAPHICS_OUTPUT_PROTOCOL *g=gops[i];
            if(count==2 && alias_count==1 && td_framebuffer(actual) && g && g->Mode &&
               !g->Mode->FrameBufferBase && !g->Mode->FrameBufferSize && !g->Mode->SizeOfInfo) {
                EFI_PCI_IO_PROTOCOL *pci=0;
                EFI_STATUS ps=p->bs->HandleProtocol(aliases[i],&PciGuid,(VOID **)&pci);
                field("inactive_gop_pci_protocol_status",ps);
                if(!pci && (ps==EFI_NOT_FOUND || ps==EFI_UNSUPPORTED)) {
                    int linked=td_console_link(p,physical_handle,aliases[i]);
                    field("console_link_result",linked==1);
                    if(linked==1){text("gop_kind=verified_virtual_console\r\n");continue;}
                    text("display_refusal=unproven_virtual_console_relationship\r\n");
                    st=EFI_UNSUPPORTED;break;
                }
                text("display_refusal=inactive_GOP_has_PCI_protocol_or_query_error\r\n");
                st=EFI_UNSUPPORTED;break;
            }
            text("display_refusal=pathless_GOP_without_verified_framebuffer_alias\r\n");
            st=EFI_UNSUPPORTED;break;
        }
        text("gop_alias=identical_5070Ti_framebuffer\r\n");
    }
    field("physical_gop_count",physical);field("pathless_gop_count",alias_count);
    EFI_STATUS freed=p->bs->FreePool(handles);
    if(EFI_ERROR(freed))return freed;
    return LogFault?EFI_VOLUME_FULL:st;
}
#endif
