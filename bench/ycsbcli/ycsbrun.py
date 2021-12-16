#!/usr/bin/env python3

import sys
import os
import glob
import subprocess


WORKLOADS = ("a", "b", "c", "d", "e", "f")
VALUE_SIZES = (10, 100, 1000, 10000, 100000)
NUM_ITERS = 5


def gen_cmd(ycsbcli: str, dbdir: str, workload: str,
            value_size: int, load: bool = False) -> list:
    return [
        ycsbcli,
        "-f", f"ycsb-traces/{workload}-{'load' if load else 'run'}.txt",
        "-v", f"{value_size}",
        "-d", dbdir
    ]

def exec_cmd(cmd: list, envs: dict) -> tuple:
    """Returns (num_reqeusts, microseconds) tuple."""
    p = subprocess.run(cmd,
        capture_output=True,
        env=envs
    )
    
    if p.returncode != 0:
        print(f"exit code: {p.returncode}")
        print(f"\nstdout:\n{p.stdout.decode()}")
        print(f"\nstderr:\n{p.stderr.decode()}")
        raise RuntimeError("ycsbcli returned non-zero rc")

    out = p.stdout.decode()
    num_reqs = int(out[out.index("Finished ")+9:out.index(" requests")])
    time_us = float(out[out.index("Time elapsed: ")+14:out.index(" us")])
    return (num_reqs, time_us)


def bench_fs(ycsbcli: str, dbdir: str, libulayfs: str, results: dict):
    envs = os.environ.copy()
    fs_type = "ext4"
    if libulayfs is not None:
        envs["LD_PRELOAD"] = libulayfs
        fs_type = "uLayFS"

    for workload in WORKLOADS:
        for value_size in VALUE_SIZES:
            load_cmd = gen_cmd(ycsbcli, dbdir, workload, value_size, load=True)
            exec_cmd(load_cmd, envs)

            kops_per_sec = 0.0
            for exper_iter in range(NUM_ITERS):
                # drop everything in page cache
                os.system("echo 3 > sudo tee /proc/sys/vm/drop_caches")

                run_cmd = gen_cmd(ycsbcli, dbdir, workload, value_size, load=False)
                num_reqs, run_us = exec_cmd(run_cmd, envs)
                print(f" {workload:1s} {value_size:6d}x{num_reqs:<5d} {run_us:.3f}")
                kops_per_sec += (float(num_reqs * 1000) / run_us) / NUM_ITERS

            if value_size not in results:
                results[value_size] = dict()
            if workload not in results[value_size]:
                results[value_size][workload] = dict()
            results[value_size][workload][fs_type] = kops_per_sec

            # clean up dbdir
            dbfiles = glob.glob(f"{dbdir}/*")
            for f in dbfiles:
                os.remove(f)

def bench(ycsbcli: str, dbdir: str, libulayfs: str):
    results = dict()

    # bench ext4
    print("\nExt4 --")
    bench_fs(ycsbcli, dbdir, None, results)

    # bench ulayfs
    print("\nuLayFS --")
    bench_fs(ycsbcli, dbdir, libulayfs, results)

    # print sheet-form tables
    print("\nResult tables--")
    for value_size in results:
        print(f"\n Throughput (kops/sec) - Value size {value_size}")
        print(f" {'Workload':>8s}  {'uLayFS':>10s}  {'ext4':>10s}")
        for workload in results[value_size]:
            kops_per_sec = results[value_size][workload]
            print(f" {workload:>8s}  {kops_per_sec['uLayFS']:10.3f}  {kops_per_sec['ext4']:10.3f}")
    print()


if __name__ == "__main__":
    assert len(sys.argv) == 3
    dbdir = sys.argv[1]
    libulayfs = sys.argv[2]
    ycsbcli = "./ycsbcli"

    assert os.path.isdir(dbdir)
    assert os.path.exists(ycsbcli)
    assert os.path.exists(libulayfs)

    print("\nBenchmarking YCSB-LevelDB...")
    bench(ycsbcli, dbdir, libulayfs)
