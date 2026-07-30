/*
 * Application 3 — Interrupts & bottom-half pattern
 *
 * Scaffold level: ~70% complete.
 *
 * Scaffold Code - AI useage:
 *   Addition of the WITH_LOAD compile-time switch and the four background-load
 *     task skeletons (carried over from App 2's pure-compute stand-ins)
 *   Logic to allow for switching between idle mode and under-load mode
 *   Commenting of code including human readable summaries
 *
 * What this scaffold gives you:
 *   - A button on GPIO 18 with internal pull-up (active-low).
 *   - A complete IRAM_ATTR ISR that signals via BOTH a binary semaphore AND
 *     a direct task notification (so you can compare the two paths).
 *   - Two bottom-half tasks (one per signaling mechanism), each pinned to Core 1.
 *   - A GPIO pulse on GPIO 19 inside the ISR so you can scope/logic-analyzer
 *     the entry-to-exit latency.
 *   - A debounce timer (200 us gate, you can tune).
 *   - An OPTIONAL background load (App 2's four tasks) behind one #define, so
 *     "measure latency under load" is a flag flip instead of a copy-paste.
 *
 * What you do:
 *   1. Theme-rename (YOURTHEME) and customize the bottom-half log messages.
 *   2. Pick which signaling path your "real" handler uses (or both).
 *   3. Run >= 50 button presses idle (WITH_LOAD 0); measure the
 *      GPIO-19-to-task-start latency for each path.
 *   4. Flip WITH_LOAD to 1 to bring App 2's four tasks online, then re-run the
 *      >= 50 presses and re-measure. Report idle vs loaded.
 *   5. Induce a failure: comment out portYIELD_FROM_ISR(), document what changes.
 *   6. Defend in README.
 *
 * What you DON'T need to change:
 *   - The ISR body, the debounce gate, or the GPIO-19 scope pulse.
 *   - The xTaskCreatePinnedToCore plumbing for the bottom-half tasks.
 *   - The load tasks themselves — they are a fixed fixture, not the object of
 *     study. Leave them as deterministic compute; you are timing YOUR ISR path,
 *     not their bodies.
 *
 * ============================================================
 *  RUN MODE  (idle vs. under load)
 * ============================================================
 *
 * WITH_LOAD selects whether the four App 2 background tasks run alongside your
 * two bottom-half tasks. Both modes report the SAME latency fields on every
 * press; only the contention on Core 1 differs.
 *
 *   WITH_LOAD = 0  -> Idle. Only the two bottom-half tasks exist on Core 1.
 *                     This is your baseline latency-max measurement.
 *   WITH_LOAD = 1  -> Loaded. App 2's four periodic tasks (10/20/50/100 ms) run
 *                     on Core 1 at the rate-monotonic ladder (15/10/5/2). Your
 *                     bottom-half tasks stay at priority 12, so they sit BETWEEN
 *                     load Task A (15) and load Task B (10). Task A can therefore
 *                     delay a wake; B, C, and D cannot preempt your bottom half.
 *                     That asymmetry is the under-load story you defend.
 *
 * Wokwi diagram: button on GPIO 18 to GND (pull-up active in code).
 *
 * ============================================================
 * Theme: Avionics
 * ============================================================
 */

#ifndef WITH_LOAD
#define WITH_LOAD 1
#endif

#ifndef BREAK_YIELD
#define BREAK_YIELD 0
#endif

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#define BUTTON_GPIO   GPIO_NUM_18      /* input — button to GND */
#define ISR_PULSE_GPIO GPIO_NUM_19     /* output — scope this for latency */
#define DEBOUNCE_US   200

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "app3";

/* Signaling primitives */
static SemaphoreHandle_t btn_sem;            /* binary semaphore path */
static TaskHandle_t      task_notif_handle;  /* direct notification path */

/* Latency telemetry */
static volatile int64_t isr_entry_time_us;
static volatile uint32_t presses_observed;
static volatile uint64_t latency_max_sem_us;
static volatile uint64_t latency_max_notif_us;

/* Debounce — track time of last accepted edge */
static volatile int64_t last_edge_us;

/* ============================================================
 *  ISR — runs in interrupt context. IRAM_ATTR avoids the
 *  first-execution cache-fill penalty from flash.
 * ============================================================ */
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();

    /* Debounce: drop edges within DEBOUNCE_US of last accepted one. */
    if (now - last_edge_us < DEBOUNCE_US) return;
    last_edge_us = now;

    /* 1. Toggle the scope output HIGH so the logic analyzer can see ISR entry. */
    gpio_set_level(ISR_PULSE_GPIO, 1);

    isr_entry_time_us = now;
    presses_observed++;

    BaseType_t higher_woken = pdFALSE;

    /* 2. Signal via binary semaphore.
     *    Multiple presses while taken can be LOST — binary sem has no count. */
    xSemaphoreGiveFromISR(btn_sem, &higher_woken);

    /* 3. Signal via direct task notification.
     *    Faster than the semaphore on most ports; one-to-one. */
    vTaskNotifyGiveFromISR(task_notif_handle, &higher_woken);

    /* 4. Toggle scope output LOW — ISR is about to return. */
    gpio_set_level(ISR_PULSE_GPIO, 0);

    /* 5. Request a context switch on ISR exit if a higher-priority task is ready. */
#if !BREAK_YIELD
    portYIELD_FROM_ISR(higher_woken);
#endif
}

/* ============================================================
 *  Bottom-half task: binary-semaphore path
 * ============================================================ */
static void btn_task_sem(void *arg)
{
    for (;;) {
        if (xSemaphoreTake(btn_sem, portMAX_DELAY) == pdTRUE) {
            int64_t wake = esp_timer_get_time();
            int64_t lat = wake - isr_entry_time_us;
            if ((uint64_t)lat > latency_max_sem_us) latency_max_sem_us = (uint64_t)lat;

            /* TODO(YOU): Do the actual themed work here.
             *
             * Avionics:   "RADAR pulse received -> increment hit counter"
             * Medical:    "patient call button —> set alarm bit"
             * Industrial: "E-STOP pressed —> assert safe state"
             * Space:      "ground command —> log + acknowledge"
             * Security:   "tamper sensor —> record event, raise integrity flag"
             *
             * For the scaffold we just log:
             */
            ESP_LOGI(TAG, "[sem] RADAR pulse received -> hit #%lu  latency=%lld us (max=%llu)",
                     (unsigned long)presses_observed,
                     (long long)lat,
                     (unsigned long long)latency_max_sem_us);
        }
    }
}

/* ============================================================
 *  Bottom-half task: direct-notification path
 * ============================================================ */
static void btn_task_notif(void *arg)
{
    for (;;) {
        uint32_t count = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (count == 0) continue;

        int64_t wake = esp_timer_get_time();
        int64_t lat = wake - isr_entry_time_us;
        if ((uint64_t)lat > latency_max_notif_us) latency_max_notif_us = (uint64_t)lat;

        /* TODO(YOU): same theme-appropriate work as the sem task.
         * (For your final design, you'd use ONE path — but the scaffold gives
         * you both so you can compare wake latency in your README.) */
        ESP_LOGI(TAG, "[notif] RADAR pulse received -> hit #%lu  latency=%lld us (max=%llu) (notif count=%lu)",
                  
                 (unsigned long)presses_observed,
                 (long long)lat,
                 (unsigned long long)latency_max_notif_us,
                 (unsigned long)count);
    }
}

#if WITH_LOAD
/* ============================================================
 *  BACKGROUND LOAD  (WITH_LOAD = 1)
 * ============================================================
 *
 * These four tasks are based on App 2's scheduler demo: four
 * periodic tasks pinned to Core 1 on the rate-monotonic ladder. 
 * Here they exist to put Core 1 under realistic contention while you measure ISR
 * response latency. 
 * You are not studying these bodies in App 3 — you are studing what
 * their presence does to your wake latency.
 *
 * Why these default code segments (the same rules App 2 fixed on):
 *   (1) DEAD-CODE ELIMINATION. Each kernel ends by writing a `volatile` sink
 *       and seeds itself from that sink, so -O2/-Os cannot delete the work.
 *   (2) INITIALIZE BUFFERS ONCE. Large buffers are static and filled a single
 *       time in load_init_buffers(), never inside the period.
 *   (3) float, NOT double. The ESP32 FPU is single-precision; double is
 *       software-emulated and runs with data-dependent timing.
 *   (4) WOKWI != SILICON. The *_ITERS / *_N / *_LEN knobs are 240 MHz ballpark.
 *       Tune them if you want a specific load level; the defaults give a light,
 *       comfortably schedulable set (~15-20% utilization).
 *
 * Per-task heartbeat counters and a WCET-max helper are included so you can
 * confirm the load is actually running (heartbeats climbing) and, if you want,
 * report the load's own WCET. Single 32-bit reads are atomic on Xtensa, so the
 * heartbeats need no mutex yet (App 6 changes that).
 */
static volatile uint32_t hb_a, hb_b, hb_c, hb_d;
static uint64_t wcet_a_max_us, wcet_b_max_us, wcet_c_max_us, wcet_d_max_us;

#define MEASURE_WCET(_max_var, _body) do {                       \
    int64_t _t0 = esp_timer_get_time();                          \
    _body;                                                        \
    int64_t _dt = esp_timer_get_time() - _t0;                    \
    if ((uint64_t)_dt > (_max_var)) (_max_var) = (uint64_t)_dt;  \
} while (0)

/* ---- Load Task A  priority 15  period 10 ms : xorshift32 churn (integer) ---- */
#define A_ITERS 300
static volatile uint32_t a_sink;
static void load_task_a(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(100);
    for (;;) {
        MEASURE_WCET(wcet_a_max_us, {
            uint32_t x = a_sink ? a_sink : 0xACE1u;   /* seed from sink (observable) */
            for (int i = 0; i < A_ITERS; i++) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            }
            a_sink = x;
        });
        hb_a++;
        vTaskDelayUntil(&last, period);
    }
}

/* ---- Load Task B  priority 10  period 20 ms : single-precision FIR ---- */
#define B_SAMP 256                       /* power of two for the index mask */
#define B_TAPS 32                       /* <= B_SAMP */
static float b_buf[B_SAMP];
static float b_coef[B_TAPS];
static volatile float b_sink;
static void load_task_b(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(200);
    for (;;) {
        MEASURE_WCET(wcet_b_max_us, {
            float acc = b_sink;          /* seed from sink (observable) */
            for (int n = 0; n < B_SAMP; n++)
                for (int k = 0; k < B_TAPS; k++)
                    acc += b_buf[(n + B_SAMP - k) & (B_SAMP - 1)] * b_coef[k];
            b_sink = acc;
        });
        hb_b++;
        vTaskDelayUntil(&last, period);
    }
}

/* ---- Load Task C  priority 5  period 50 ms : CRC-32 over a buffer ---- */
#define C_LEN 4096                      /* bytes; raise toward 49152 to lengthen */
static uint8_t c_pkt[C_LEN];
static volatile uint32_t c_sink;
static void load_task_c(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(500);
    for (;;) {
        MEASURE_WCET(wcet_c_max_us, {
            uint32_t crc = 0xFFFFFFFFu ^ c_sink;     /* seed from sink */
            for (int n = 0; n < C_LEN; n++) {
                crc ^= c_pkt[n];
                for (int b = 0; b < 8; b++)
                    crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
            }
            c_sink = crc ^ 0xFFFFFFFFu;
        });
        hb_c++;
        vTaskDelayUntil(&last, period);
    }
}

/* ---- Load Task D  priority 2  period 100 ms : insertion sort, forced worst case ---- */
#define D_N 400
static int d_arr[D_N];
static volatile int d_sink;
static void load_task_d(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000);
    for (;;) {
        MEASURE_WCET(wcet_d_max_us, {
            for (int i = 0; i < D_N; i++) d_arr[i] = D_N - i + (d_sink & 1);
            for (int i = 1; i < D_N; i++) {          /* insertion sort */
                int key = d_arr[i];                  /* split decls: a top-level */
                int j = i - 1;                       /* comma would break the macro arg */
                while (j >= 0 && d_arr[j] > key) { d_arr[j+1] = d_arr[j]; j--; }
                d_arr[j+1] = key;
            }
            d_sink = d_arr[D_N/2];
        });
        hb_d++;
        ESP_LOGI(TAG, "[wcet] A=%lluus B=%lluus C=%lluus D=%lluus  hb=%lu/%lu/%lu/%lu",
                 (unsigned long long)wcet_a_max_us, (unsigned long long)wcet_b_max_us,
                 (unsigned long long)wcet_c_max_us, (unsigned long long)wcet_d_max_us,
                 (unsigned long)hb_a, (unsigned long)hb_b,
                 (unsigned long)hb_c, (unsigned long)hb_d);
        vTaskDelayUntil(&last, period);
    }
}

/* Fill the load buffers exactly once, off the periodic path. */
static void load_init_buffers(void)
{
    for (int i = 0; i < B_SAMP; i++) b_buf[i]  = (float)((i * 2654435761u) & 0xFFFF) / 65536.0f;
    for (int k = 0; k < B_TAPS; k++) b_coef[k] = 1.0f / (float)B_TAPS;   /* boxcar */
    for (int n = 0; n < C_LEN;  n++) c_pkt[n]  = (uint8_t)(n * 31 + 7);
}

static void start_background_load(void)
{
    load_init_buffers();
    /* Rate-monotonic ladder, all on Core 1, mirroring App 2. These priorities
     * are FIXED here (this is a load fixture). Note A=15 outranks your
     * bottom-half tasks (12); B/C/D do not. */
    xTaskCreatePinnedToCore(load_task_a, "load_a", 2048, NULL, 15, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(load_task_b, "load_b", 2048, NULL, 10, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(load_task_c, "load_c", 2048, NULL,  5, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(load_task_d, "load_d", 2048, NULL,  2, NULL, APP_CPU_NUM);
}
#endif /* WITH_LOAD */

/* ============================================================
 *  app_main — wire everything up
 * ============================================================ */
void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== App 3 [Avionics] starting — ISR + bottom-half ====");

#if WITH_LOAD
    ESP_LOGI(TAG, "Run mode: UNDER LOAD (WITH_LOAD=1) — App 2's 4 tasks on Core 1");
#else
    ESP_LOGI(TAG, "Run mode: IDLE (WITH_LOAD=0) — baseline latency, no background tasks");
#endif

    /* Create signaling primitives. */
    btn_sem = xSemaphoreCreateBinary();

    /* Bottom-half tasks. Both pinned to Core 1, both high priority because
     * they're the "real-time response" path. */
    xTaskCreatePinnedToCore(btn_task_sem,  "btn_sem",   4096, NULL, 12, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(btn_task_notif,"btn_notif", 4096, NULL, 12,
                            &task_notif_handle, APP_CPU_NUM);

#if WITH_LOAD
    /* Bring App 2's periodic tasks online as a Core-1 load fixture. */
    start_background_load();
#endif

    /* Configure GPIOs. */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,    /* button pulls low when pressed */
    };
    gpio_config(&btn_cfg);

    gpio_config_t pulse_cfg = {
        .pin_bit_mask = 1ULL << ISR_PULSE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pulse_cfg);
    gpio_set_level(ISR_PULSE_GPIO, 0);

    /* Install GPIO ISR service. Flags = 0 means default (low) priority. */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

    ESP_LOGI(TAG, "Press the button on GPIO %d. Scope GPIO %d to time the ISR.",
             BUTTON_GPIO, ISR_PULSE_GPIO);

    /* app_main returns; tasks continue. */
}
