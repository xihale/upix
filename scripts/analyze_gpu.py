#!/usr/bin/env python3
import pandas as pd
import sys

csv_file = sys.argv[1] if len(sys.argv) > 1 else "gpu_batch_1.csv"
df = pd.read_csv(csv_file)

print(f"=== GPU Analysis: {csv_file} ===")
print(f"Samples: {len(df)}")
print(f"Avg Util: {df['gpu_util(%)'].mean():.2f}%")
print(f"Max Util: {df['gpu_util(%)'].max():.2f}%")
print(f"Avg Mem: {df['mem_used(MB)'].mean():.0f}MB")
print(f"Peak Mem: {df['mem_used(MB)'].max():.0f}MB")
print(f"Avg Temp: {df['temp(C)'].mean():.1f}°C")
