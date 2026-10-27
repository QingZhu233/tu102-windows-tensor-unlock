"""Compare identical-mode Windows Tensor microbenchmarks; files only."""
import argparse
import json
import statistics
from pathlib import Path


def compare(before, after):
    a=json.loads(Path(before).read_text());b=json.loads(Path(after).read_text())
    for key in ("gpu","bdf","compute_capability","sms","driver_api_version","ptx_sha256","rounds","threads_per_block"):
        if a[key]!=b[key]:raise ValueError("Noncomparable benchmark metadata: "+key)
    rows=[]
    for mode in ("FP16-input/FP32-accumulate","FP16-input/FP16-accumulate"):
        values=[]
        for data in (a,b):
            s=[x for x in data["samples"] if x["mode"]==mode]
            if len(s)!=7 or any(x["exact_result_errors"] or x["unique_sms"]!=68 for x in s):
                raise ValueError("Missing samples/correctness/SM coverage")
            values.append(dict(median_tflops=statistics.median(x["tflops"] for x in s),
                               median_flops_sm_cycle=statistics.median(x["flops_per_sm_cycle"] for x in s)))
        rows.append(dict(mode=mode,before=values[0],after=values[1],
                         per_sm_cycle_ratio=values[1]["median_flops_sm_cycle"]/values[0]["median_flops_sm_cycle"]))
    target,control=rows
    rate_gain=(1.8<=target["per_sm_cycle_ratio"]<=2.2 and
               0.9<=control["per_sm_cycle_ratio"]<=1.1)
    return dict(rows=rows,observed_tensor_issue_rate_gain=rate_gain,
                interpretation="Approximately doubled FP16->FP32 issue rate; FP16 accumulation control stable" if rate_gain else
                "No confirmed doubling under this test; inspect transaction logs and measured results",
                limitation="Microbenchmark with exact simple arithmetic, not exhaustive stability or application-speed certification")


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("before");p.add_argument("after");a=p.parse_args()
    print(json.dumps(compare(a.before,a.after),indent=2))
