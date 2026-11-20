"""Predicted battery life from measured current draw.

Measure the USB current in each mode (USB power meter or multimeter in series),
fill in the numbers, and run:  python tools/battery_life.py
The defaults below are a planning budget, NOT measurements; replace them.
"""
import argparse

# mode: (current mA, hours per day in that mode)
BUDGET = {
    "idle unlocked (Wi-Fi, OLED on)": (110.0, 15.0),
    "locked, waiting (Wi-Fi, OLED on)": (115.0, 8.5),
    "flip attempt (buzzer chirps)": (130.0, 0.1),
    "servo move": (350.0, 0.01),
    "alarm ringing": (160.0, 0.05),
}

ap = argparse.ArgumentParser()
ap.add_argument("--capacity-mah", type=float, default=10000, help="power bank capacity")
ap.add_argument("--efficiency", type=float, default=0.85, help="usable fraction (boost converter losses)")
args = ap.parse_args()

total_h = sum(h for _, h in BUDGET.values())
mah_per_day = sum(ma * h for ma, h in BUDGET.values())
avg_ma = mah_per_day / total_h
usable = args.capacity_mah * args.efficiency
print(f"{'mode':38s} {'mA':>7s} {'h/day':>6s} {'mAh/day':>8s}")
for name, (ma, h) in BUDGET.items():
    print(f"{name:38s} {ma:7.1f} {h:6.2f} {ma * h:8.1f}")
print(f"\naverage current: {avg_ma:.1f} mA  ({mah_per_day:.0f} mAh per {total_h:.1f} h)")
print(f"{args.capacity_mah:.0f} mAh pack x {args.efficiency:.0%} usable = {usable:.0f} mAh")
print(f"predicted runtime: {usable / avg_ma:.1f} h  ({usable / avg_ma / 24:.1f} days)")
