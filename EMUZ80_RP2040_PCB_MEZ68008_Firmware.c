/*
 * EMUZ80_RP2040_PCB_MEZ68008_Firmware
 *
 * @tendai22plus さんの EMUZ80_RP2040_PCB と
 * @S_Okue さんの MEZ68008 を使って、
 * Z80 DIP40 フットプリントに MC68008 を組み替えて動作させるための
 * RP2040 ファームウェアです。
 *
 * 秋月電子通商 AE-RP2040 ボード上で動作。
 * RP2040 が 64KB RAM + MC6850 互換 ACIA (0xE000) をエミュレート。
 *
 * Author:  DragonBallEZ
 * License: MIT
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"
#include "pico/platform.h" 
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "tusb.h" // TinyUSB

#define ACIAD 0xE000
#define ACIAC 0xE001
#define MEMORY_SIZE 65536

// === Easy Overclock Configuration ===
// Change this one value to switch clock speed (under 300MHz recommended)
// Common good values: 200000, 250000, 266667, 280000
#define TARGET_SYS_CLK_KHZ 288000

// Pin assignments (EMUZ80_AE_RP2040 + MEZ68008 style)
#define ADDR_BASE 0
#define DATA_BASE 16
#define AS_PIN    24
#define RW_PIN    25
#define DS_PIN    26
#define DTACK_PIN 27
#define RESET_PIN 28
#define CLK_PIN   29

// Direct SIO masks for fast GPIO
#define DATA_MASK   (0xFFu << 16)   // GPIO16-23
#define DATA_SHIFT  16
#define DS_MASK     (1u << DS_PIN)
#define RW_MASK     (1u << RW_PIN)

#define DTACK_MASK (1u << DTACK_PIN)

// 68000 memory emulation area (64KB) aligned to 64KB boundary for performance
static uint8_t __attribute__((aligned(65536))) __not_in_flash("bus_memory")  memory[MEMORY_SIZE];

// Flag to indicate if the bus emulation is running (shared between cores)
static volatile bool    __attribute__((section(".scratch_x"))) emu_flg = false;  

#include "ehbasic.h"  // EhBASIC program image
// #include "ehbasic030.h"  // EhBASIC program image

static void load_to_memory(uint8_t *mem, const uint8_t *data, size_t len, uint16_t offset) {
    memcpy(mem + offset, data, len);
}

// Fast direct SIO register access (bypassing SDK for speed)
static inline void drive_data_fast(uint8_t data) {
    uint32_t bits = (uint32_t)data << DATA_SHIFT;
    sio_hw->gpio_out = (sio_hw->gpio_out & ~DATA_MASK) | bits;
    sio_hw->gpio_oe_set = DATA_MASK;
}

static inline void release_data_bus_fast(void) {
    sio_hw->gpio_oe_clr = DATA_MASK;
}

static inline uint32_t read_gpio_all(void) {
    return sio_hw->gpio_in;
}

// DTACK direct SIO control (for speed in busemu)
static inline void dtack_low(void)  { sio_hw->gpio_out = sio_hw->gpio_out & ~DTACK_MASK; }  // assert (active low)
static inline void dtack_high(void) { sio_hw->gpio_out = sio_hw->gpio_out |  DTACK_MASK; }  // deassert (high)

// 自動で最適な wrap / clkdiv を計算して PWM 周波数を設定する関数
static void pwm_set_frequency(uint slice_num, uint chan, float freq_hz) {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    
    // 1周期あたりのシステムクロック数
    float period = (float)sys_hz / freq_hz;
    
    uint16_t wrap;
    float div;
    
    if (period > 65536.0f) {
        // 低周波数 → wrap最大 + 分周器を使う
        wrap = 0xFFFF;
        div = period / 65536.0f;
    } else {
        // 高周波数 → 分周器=1.0 で wrap を小さくする
        div = 1.0f;
        wrap = (uint16_t)(period - 1.0f);
        // 安全のため上限
        if (wrap > 0xFFFF) wrap = 0xFFFF;
    }
    
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, div);
    pwm_config_set_wrap(&cfg, wrap);
    pwm_init(slice_num, &cfg, true);
    pwm_set_chan_level(slice_num, chan, wrap / 2);
    
    // 実際に出た周波数を表示（デバッグ用）
    float actual_freq = (float)sys_hz / ((wrap + 1ULL) * div);
    printf("PWM: target=%0.3fMHz  actual=%0.3fMHz  wrap=%u  div=%.4f\n",
           freq_hz / 1000000.0f, actual_freq / 1000000.0f, wrap, div);
}


// 通信専用のコア0,1で共有するUART通信のためのグローバル変数
static volatile uint8_t __attribute__((section(".scratch_x"))) uart_tx_data; // 送信データバッファ(Z80からPicoへ: 1バイト)
static volatile uint8_t __attribute__((section(".scratch_x"))) uart_rx_data; // 受信データバッファ(PicoからZ80へ: 1バイト)
static volatile bool __attribute__((section(".scratch_x"))) uart_tx_ready; // 送信可フラグ (true=Ready, false=Busy)
static volatile bool __attribute__((section(".scratch_x"))) uart_rx_ready; // 受信完了フラグ (true=Ready, false=Empty)

static volatile bool __attribute__((section(".scratch_x"))) exit_flag = false;


//
// コア1 のエントリポイント　バスエミュレーション
//
void __time_critical_func(busemu)(void) {
    uint8_t * const mem = memory;   // ベースアドレスを一度だけレジスタにロード
    uart_tx_data = 0;      // 送信データ初期化
    uart_rx_data = 0;      // 受信データ初期化
    uart_tx_ready = true;  // 送信準備完了
    uart_rx_ready = false; // 受信バッファは空
    exit_flag = false;     // 終了フラグ初期化

    while (!(read_gpio_all() & DS_MASK)) {  // DS low(active) → wait for high
        tight_loop_contents(); 
    }
    while (true) {
        while (read_gpio_all() & DS_MASK) { // DS high (inactive) → wait for falling edge
            tight_loop_contents(); 
        }
        uint32_t gpio = read_gpio_all();
        bool ds = (gpio & DS_MASK);   // ds = pin level (true when high/inactive)
        bool rw = (gpio & RW_MASK);
        uint16_t adrs = gpio & 0xFFFFu;
        uint8_t data;
        if (rw) {       // Read cycle: CPU is reading from us → drive data bus
            if (!((adrs & 0xFF00u) == 0xE000u)) {
                data = mem[adrs];
//                printf("A:%04X D:%02X\n", adrs, data);
            } else {
                if (adrs == ACIAC) {    // Status: bit0 (RDRF) = 1 if input char available
                    data = uart_rx_ready ? 0x01 : 0x00;  // RDRF
                    if (uart_tx_ready) {
                        data |= 0x02;   // TDRE set
                    }
                } else if (adrs == ACIAD) {
                    // Data: return char if available, else 0
                    data = uart_rx_ready ? uart_rx_data : 0;
                    uart_rx_ready = false;  // consume the char
                } else {
                    data = 0;
                }
            }
            drive_data_fast(data);
//            printf("RW=%d ADRS=%04X DATA=%02X\n", rw, adrs, data);
            
            dtack_low();  // DTACK low to acknowledge immediately
        } else {        // Write cycle
            data = (gpio >> DATA_SHIFT) & 0xFFu;
            if (adrs != ACIAD) {
                mem[adrs] = data;
            } else {
                if (uart_tx_ready) {       // 送信可能な場合のみ送信データを格納
                    uart_tx_data = data;
                    uart_tx_ready = false; // mark as busy
                } else {               // 送信中の場合は無視する（バッファオーバーフロー防止）
                }
            }
//            printf("RW=%d ADRS=%04X DATA=%02X\n", rw, adrs, data);
            dtack_low();  // DTACK low to acknowledge immediately
        }
        while (!(read_gpio_all() & DS_MASK)) {  // DS  low (active) → wait for rising edge
            tight_loop_contents();
        }
        if (rw) {            // RWを再サンプリングせず、立下り時にキャプチャしたフラグを使用
            release_data_bus_fast();
        }
        dtack_high();     // DTACK high to acknowledge immediately
        if (exit_flag) {
            break;  // exit on Ctrl-\ from UART
        }
    }
    emu_flg = false;  // signal to main that we are exiting
}

//
// --- UART Task (Core 0) ---
//
void uart_task(void) {
  printf("task UART start..\n\n");
  while (true) {
    // 送信処理: (Z80 -> USB) Z80がデータを書き込んで Busy になったら実行
    if (!uart_tx_ready) {
      if (tud_cdc_connected() && tud_cdc_write_available() > 0) {
        putchar(uart_tx_data);
        uart_tx_ready = true; // 送信完了（readyに戻す）
      }
    }
    // 受信処理(US->Z80) RX Readyがfalseの場合のみ入力をチェック
    if (!uart_rx_ready) {
      int c = getchar_timeout_us(0);
      if (c != PICO_ERROR_TIMEOUT) {
        if (c == 0x1C) { // Ctrl-\で終了
          exit_flag = true;
          break;
        }
        uart_rx_data = (uint8_t)c;
        uart_rx_ready = true;
      }
    }
    sleep_us(500); // 500us待機（CPU負荷を下げるため）
  }
}



int main() {
    // === Overclock setup (change TARGET_SYS_CLK_KHZ above to switch easily) ===
    uint32_t target_khz = TARGET_SYS_CLK_KHZ;

    // Select safe voltage based on target frequency
    if (target_khz <= 200000) {
        vreg_set_voltage(VREG_VOLTAGE_1_15);
    } else if (target_khz <= 250000) {
        vreg_set_voltage(VREG_VOLTAGE_1_25);
    } else {
        vreg_set_voltage(VREG_VOLTAGE_1_30);
    }

    sleep_ms(5);  // wait for voltage to stabilize

    bool clock_ok = set_sys_clock_khz(target_khz, true);

    stdio_init_all();
    sleep_ms(2000);

    printf("EMUZ80_RP2040 MEZ68008 - 0.01\n");
    if (clock_ok) {
        printf("sysclk %0.3fMHz\n", target_khz / 1000.0f);
    } else {
        printf("Failed to set %0.3fMHz clock\n", target_khz / 1000.0f);
    }

    // Address bus: GP0-15 inputs with pull-down
    for (int i = 0; i < 16; i++) {
        gpio_init(ADDR_BASE + i);
        gpio_pull_down(ADDR_BASE + i);
        gpio_set_dir(ADDR_BASE + i, GPIO_IN);
    }

    // Data bus: GP16-23 inputs with pull-down (bidirectional)
    for (int i = 0; i < 8; i++) {
        gpio_init(DATA_BASE + i);
        gpio_pull_down(DATA_BASE + i);
        gpio_set_dir(DATA_BASE + i, GPIO_IN);
    }
    // Make sure data bus is input from SIO perspective
    sio_hw->gpio_oe_clr = DATA_MASK;

    gpio_init(AS_PIN);
    gpio_pull_up(AS_PIN);
    gpio_set_dir(AS_PIN, GPIO_IN);

    gpio_init(RW_PIN);
    gpio_pull_up(RW_PIN);
    gpio_set_dir(RW_PIN, GPIO_IN);

    gpio_init(DS_PIN);
    gpio_pull_up(DS_PIN);
    gpio_set_dir(DS_PIN, GPIO_IN);

    gpio_init(DTACK_PIN);
    gpio_put(DTACK_PIN, 1);  // DTACK初期値はHIGH
    gpio_set_dir(DTACK_PIN, GPIO_OUT);

    // RESET (open-drain style): start asserted
    gpio_init(RESET_PIN);
    gpio_put(RESET_PIN, 0);
    gpio_set_dir(RESET_PIN, GPIO_OUT);
    printf("RESET-ON\n");

  
    printf("hit-any-key\n");
    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT) {
        tight_loop_contents();  // yield a bit
    }
    // Start CLK via PWM on GP29 (自動計算で周波数設定)
    printf("CLK-ON\n");
    gpio_set_function(CLK_PIN, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(CLK_PIN);
    uint chan = pwm_gpio_to_channel(CLK_PIN);

   float desired_freq = 10000000.0f;    // 10MHz
//    float desired_freq = 8000000.0f;    // 8MHz

   pwm_set_frequency(slice_num, chan, desired_freq);
    sleep_ms(200);

    printf("ROMエミュレータ起動 - core1\n");
    multicore_launch_core1(busemu);
    emu_flg = true;

    // Load program into simulated memory EhBASIC
    load_to_memory(memory, EhBASIC, ehbasic_size, 0x0000);
    printf("EhBASIC loaded to memory (size=%d bytes)\n", ehbasic_size);
    // Wait for any keypress to proceed

    printf("RESET-OFF\n");
    printf("Start..\n");
    // Release reset
    gpio_set_dir(RESET_PIN, GPIO_IN);   // RESET-OFF (open-drain style)
    uart_task();  // Start UART task on core 0

    pwm_set_enabled(slice_num, false);

    printf("\nStopped.\n");

    while (true) {
        tight_loop_contents();
    }
}
