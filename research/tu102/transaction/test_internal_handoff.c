#define TU102_AUTOMATIC_INTERNAL
#define main retained_handoff_tests
#include "test_handoff.c"
#undef main
int main(void) {
    TuWindows w;setup();memset(&w,0,sizeof(w));self_image.DeviceHandle=&volume_tokens[0];
    assert(tw_prepare(&parent_token,&bs,&w)==EFI_SUCCESS && w.prepared && loads==1);unsigned cases=1;
    setup();memset(&w,0,sizeof(w));self_image.DeviceHandle=&volume_tokens[0];count=2;found_mask=3;
    assert(tw_prepare(&parent_token,&bs,&w)==EFI_ACCESS_DENIED && !loads);cases++;
    printf("PASS %u same-ESP Windows handoff/ambiguity cases; no firmware\n",cases);return 0;
}
