import subprocess
import time
import os

print("Starting samosa-maple...")
proc = subprocess.Popen(["build/samosa-maple", "--prompt", "Hello world!"], env={**os.environ, "METAL_PATH": "build/mlx-build/mlx/backend/metal/kernels"})

time.sleep(2)  # wait for model load
print("Running vmmap...")
vmmap_out = subprocess.check_output(f"vmmap -summary {proc.pid}", shell=True, text=True)

with open("tests/samosa_vmmap.txt", "w") as f:
    f.write(vmmap_out)

proc.wait()
print("Done!")
