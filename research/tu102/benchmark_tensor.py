"""Windows-only bounded SM75 Tensor issue-rate check using NVIDIA Driver API.

No CUDA toolkit installation is required. Only a normal CUDA context/kernel is
used; no clock/power settings, firmware, PCI, registry or boot state is changed.
Method follows the 68-SM/1024-thread/2048-round ASUS research microbenchmark:
four independent accumulators; 8*(4 positive + 4 negative) MMA per round, then
four nonzero exact-result checks. This is NOT a GEMM/application benchmark or
an exhaustive numerical-stability test.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
import statistics
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def make_ptx():
    lines=[".version 6.5", ".target sm_75", ".address_size 64"]
    for mode in (32,16):
        lines += [f".visible .entry rate{mode}(",
                  ".param .u64 output, .param .u64 ticks, .param .u64 smids,",
                  ".param .u32 rounds, .param .u32 one, .param .u32 negone)",
                  ".maxntid 1024,1,1", "{",
                  ".reg .b32 %r<16>; .reg .b64 %q<16>; .reg .pred %p<4>;",
                  ".reg .f32 %f<16>; .reg .b32 %h<8>;",
                  "ld.param.u64 %q0,[output]; ld.param.u64 %q1,[ticks]; ld.param.u64 %q2,[smids];",
                  "ld.param.u32 %r1,[rounds]; ld.param.u32 %r6,[one]; ld.param.u32 %r7,[negone];",
                  "mov.u32 %r2,%tid.x; mov.u32 %r3,%ctaid.x; mov.u32 %r0,0;"]
        for i in range(16): lines.append(f"mov.f32 %f{i},0f00000000;")
        for i in range(8): lines.append(f"mov.b32 %h{i},0;")

        def mma(group, negative=False):
            accum = ",".join(f"%f{4*group+k}" for k in range(4)) if mode==32 else ",".join(f"%h{2*group+k}" for k in range(2))
            suffix="f32.f16.f16.f32" if mode==32 else "f16.f16.f16.f16"
            return f"mma.sync.aligned.m16n8k8.row.col.{suffix} {{{accum}}},{{%r6,%r6}},{{%r{7 if negative else 6}}},{{{accum}}};"

        lines += ["bar.sync 0; mov.u64 %q3,%clock64;", f"LOOP{mode}:"]
        for _ in range(8):
            for negative in (False,True):
                for j in range(4): lines.append(mma(j,negative))
        lines += ["add.u32 %r0,%r0,1; setp.lt.u32 %p0,%r0,%r1;", f"@%p0 bra LOOP{mode};"]
        lines += [mma(j) for j in range(4)]
        lines += ["bar.sync 0; mov.u64 %q4,%clock64; sub.u64 %q4,%q4,%q3;",
                  "mov.u32 %r8,0;"]
        for i in range(16 if mode==32 else 8):
            lines.append(f"setp.neu.f32 %p1,%f{i},0f41000000;" if mode==32 else f"setp.ne.u32 %p1,%h{i},0x48004800;")
            lines.append("selp.u32 %r9,1,0,%p1; or.b32 %r8,%r8,%r9;")
        lines += ["mad.lo.u32 %r4,%r3,1024,%r2; mul.wide.u32 %q5,%r4,4; add.u64 %q5,%q0,%q5; st.global.u32 [%q5],%r8;",
                  "setp.ne.u32 %p2,%r2,0;", f"@%p2 bra DONE{mode};",
                  "mul.wide.u32 %q6,%r3,8; add.u64 %q6,%q1,%q6; st.global.u64 [%q6],%q4;",
                  "mov.u32 %r10,%smid; mul.wide.u32 %q7,%r3,4; add.u64 %q7,%q2,%q7; st.global.u32 [%q7],%r10;",
                  f"DONE{mode}: ret;", "}"]
    return "\n".join(lines)+"\n"


def run():
    os.environ["CUDA_CACHE_DISABLE"]="1"
    os.environ["CUDA_CACHE_PATH"]=str(ROOT/"build/transaction/cuda-cache")
    cuda=C.WinDLL("nvcuda.dll")
    ptr=C.c_void_p
    u64=C.c_uint64
    funcs={}

    def api(name, args):
        f=getattr(cuda,name);f.restype=C.c_int;f.argtypes=args;funcs[name]=f
        return f

    def check(name,*args):
        rc=funcs[name](*args)
        if rc: raise RuntimeError(f"{name}: CUDA error {rc}")

    api("cuInit",[C.c_uint]);api("cuDriverGetVersion",[C.POINTER(C.c_int)])
    api("cuDeviceGetByPCIBusId",[C.POINTER(C.c_int),C.c_char_p])
    api("cuDeviceGetPCIBusId",[C.c_char_p,C.c_int,C.c_int])
    api("cuDeviceGetName",[C.c_char_p,C.c_int,C.c_int])
    api("cuDeviceGetAttribute",[C.POINTER(C.c_int),C.c_int,C.c_int])
    api("cuDeviceTotalMem_v2",[C.POINTER(C.c_size_t),C.c_int])
    api("cuCtxCreate_v2",[C.POINTER(ptr),C.c_uint,C.c_int]);api("cuCtxDestroy_v2",[ptr])
    api("cuModuleLoadData",[C.POINTER(ptr),ptr]);api("cuModuleUnload",[ptr])
    api("cuModuleGetFunction",[C.POINTER(ptr),ptr,C.c_char_p])
    api("cuMemAlloc_v2",[C.POINTER(u64),C.c_size_t]);api("cuMemFree_v2",[u64])
    api("cuMemcpyDtoH_v2",[ptr,u64,C.c_size_t])
    api("cuEventCreate",[C.POINTER(ptr),C.c_uint]);api("cuEventRecord",[ptr,ptr])
    api("cuEventSynchronize",[ptr]);api("cuEventElapsedTime",[C.POINTER(C.c_float),ptr,ptr])
    api("cuEventDestroy_v2",[ptr])
    api("cuLaunchKernel",[ptr,C.c_uint,C.c_uint,C.c_uint,C.c_uint,C.c_uint,C.c_uint,C.c_uint,ptr,C.POINTER(ptr),C.POINTER(ptr)])
    check("cuInit",0)
    device=C.c_int();check("cuDeviceGetByPCIBusId",C.byref(device),b"0000:03:00.0")
    name=C.create_string_buffer(128);bus=C.create_string_buffer(32)
    check("cuDeviceGetName",name,len(name),device.value)
    check("cuDeviceGetPCIBusId",bus,len(bus),device.value)
    attrs=[]
    for attr in (75,76,16):
        v=C.c_int();check("cuDeviceGetAttribute",C.byref(v),attr,device.value);attrs.append(v.value)
    total=C.c_size_t();check("cuDeviceTotalMem_v2",C.byref(total),device.value)
    if attrs != [7,5,68] or "2080 Ti" not in name.value.decode() or not (21<<30) < total.value <= (22<<30):
        raise RuntimeError("Refusing unexpected GPU architecture, SM count, model or VRAM")
    driver=C.c_int();check("cuDriverGetVersion",C.byref(driver))
    ctx=ptr();module=ptr();buffers=[];events=[]
    ptx=make_ptx().encode()+b"\0"
    result=dict(gpu=name.value.decode(),bdf=bus.value.decode(),compute_capability="7.5",sms=68,
                vram_bytes=total.value,driver_api_version=driver.value,
                threads_per_block=1024,rounds=2048,method="four accumulators; 64 MMA per round; exact final value=8",
                ptx_sha256=hashlib.sha256(ptx).hexdigest(),unix_timestamp=time.time(),
                firmware_or_settings_changed=False,samples=[])
    try:
        check("cuCtxCreate_v2",C.byref(ctx),0,device.value)
        image=C.create_string_buffer(ptx);check("cuModuleLoadData",C.byref(module),C.cast(image,ptr))
        for size in (68*1024*4,68*8,68*4):
            b=u64();check("cuMemAlloc_v2",C.byref(b),size);buffers.append(b)
        for _ in range(2):
            e=ptr();check("cuEventCreate",C.byref(e),0);events.append(e)
        out=(C.c_uint32*(68*1024))();cycles=(u64*68)();ids=(C.c_uint32*68)()
        rounds=C.c_uint32(2048);one=C.c_uint32(0x3c003c00);neg=C.c_uint32(0xbc00bc00)
        arguments=(ptr*6)(*[C.cast(C.byref(p),ptr) for p in (*buffers,rounds,one,neg)])
        for mode in (32,16):
            function=ptr();check("cuModuleGetFunction",C.byref(function),module,f"rate{mode}".encode())
            for sample in range(-2,7):
                check("cuEventRecord",events[0],None)
                check("cuLaunchKernel",function,68,1,1,1024,1,1,0,None,arguments,None)
                check("cuEventRecord",events[1],None);check("cuEventSynchronize",events[1])
                ms=C.c_float();check("cuEventElapsedTime",C.byref(ms),*events)
                check("cuMemcpyDtoH_v2",out,buffers[0],C.sizeof(out))
                check("cuMemcpyDtoH_v2",cycles,buffers[1],C.sizeof(cycles))
                check("cuMemcpyDtoH_v2",ids,buffers[2],C.sizeof(ids))
                if any(out):raise RuntimeError(f"FP{mode} MMA exact-result correctness failure")
                unique=len(set(ids));median=statistics.median(cycles)
                if unique!=68 or not median or ms.value<=0:raise RuntimeError("Unexpected SM coverage/timing; no comparable result")
                flops_per_sm=(2048*64+4)*32*2048
                if sample>=0:
                    record=dict(mode=f"FP16-input/FP{mode}-accumulate",sample=sample,milliseconds=ms.value,
                                tflops=flops_per_sm*68/(ms.value*1e9),median_sm_cycles=median,
                                flops_per_sm_cycle=flops_per_sm/median,unique_sms=unique,
                                exact_result_errors=0,checked_threads=68*1024)
                    result["samples"].append(record)
            group=[s for s in result["samples"] if s["mode"].endswith(f"FP{mode}-accumulate")]
            print(json.dumps(dict(mode=f"FP{mode}",median_tflops=statistics.median(s["tflops"] for s in group),
                                  median_flops_sm_cycle=statistics.median(s["flops_per_sm_cycle"] for s in group),
                                  correctness_passed=True)))
    finally:
        for e in events:funcs["cuEventDestroy_v2"](e)
        for b in buffers:funcs["cuMemFree_v2"](b)
        if module:funcs["cuModuleUnload"](module)
        if ctx:funcs["cuCtxDestroy_v2"](ctx)
    return result


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    output=args.output.resolve()
    if not output.is_relative_to(ROOT) or output.exists():
        raise SystemExit("Output must be a NEW file within this E:\\unlock project")
    result=run()
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print("Saved benchmark result to",output)
