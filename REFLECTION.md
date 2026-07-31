# Final Reflection

EEE 4775 Real-Time Systems, Final Integration Capstone
Aaron Ways, Summer 2026


## What I would do differently

Fix the instrumentation instead of writing around it. I already knew in App 3 that the idle gap was misleading, since both tasks run at the same priority and share the same timer variable. The notification task prints its log line before the semaphore task is able to get to the CPU, which means the semaphore's number actually includes that print time. Essentially it reflects the run order, not true speed.

That part was right. The problem is that I explained it instead of fixing it. Two separate timestamps and moving the ESP_LOGI outside the timed region would have taken a few minutes, and instead I carried the broken number into the capstone and defended it again. Re-running it, hits #49 and #50 flipped the order and notification became the path reading 2246 us. Real wake latency is 20 to 30 us on both paths.

## What was harder than expected

Trusting arithmetic over an explanation that already sounded right. I wrote that the increase comes from Task A, since the bottom-half tasks run at priority 12 while task A runs at priority 15 making it higher, so when the button ISR is fired while Task A is mid execution, the task has to wait for A to finish before it can actually run. I said that directly explains the 79x factor increase for notif max.

The priority reasoning holds. The conclusion does not, because I never measured how long A runs. For the capstone I printed the WCET values MEASURE_WCET had been collecting all along, and Task A is 346 us. It cannot cause a 2692 us delay, since it is short by a factor of about eight. Nothing about that explanation felt weak, and it was the part I was most confident in.

## The most valuable thing I learned

Know which two events your number sits between, and know which core the code is running on. In App 3 I said the prediction and the observation did not match because at priority 12 with an idle core, the next tick reschedules to the ready task regardless, so IDF triggers the reschedule anyway. I also expected the error would show up under load with WITH_LOAD 1, where waiting on a higher priority task turns into real missed deadline latency.

The observation was right and the mechanism was wrong. gpio_install_isr_service() is called from app_main on Core 0 while both bottom halves are pinned to Core 1, and a yield request only affects the core the ISR is on. The line I removed was never in the path, and under load it still did not appear. Load was not the variable. Core placement was.

Same lesson in the timing numbers. The logic analyzer reads 15.35 us and the serial log reads 20 to 30 us for what I was calling the same thing, and both are correct. One spans button edge to ISR pulse, the other spans ISR entry to the bottom half running.

## Next steps

1. Separate timestamps per path, with the log moved outside the timed region.
2. Install the ISR service on Core 1 so the removed yield is load bearing and the failure reproduces.
3. Correct the period comments, which say 10 / 20 / 50 / 100 ms while the code runs 100 / 200 / 500 / 1000.
