#
# EMUZ80_RP2040_PCB_MEZ68008_Firmware
#
# @tendai22plus さんの EMUZ80_RP2040_PCB と
# @S_Okue さんの MEZ68008 を使って、
# Z80 DIP40 フットプリントに MC68008 を組み替えて動作させるための
# RP2040 ファームウェアです。
#
# 秋月電子通商 AE-RP2040 ボード上で動作。
# RP2040 が 64KB RAM + MC6850 互換 ACIA (0xE000) をエミュレート。
#
# Author:  DragonBallEZ
# License: MIT
#

from machine import Pin, PWM
import time
import select
import sys

def load_to_memory(memory, data, offset):
    """memoryの指定したoffset位置にdataをコピーする"""
    memory[offset : offset + len(data)] = data

adrs_pins = [Pin(i, Pin.IN, Pin.PULL_DOWN) for i in range(0, 16)]
data_pins = [Pin(i, Pin.IN, Pin.PULL_DOWN) for i in range(16, 24)]
as_pin = Pin(24, Pin.IN, Pin.PULL_UP) # GP24 in,  68k AS -  Z80 20pin IORQ
rw_pin = Pin(25, Pin.IN, Pin.PULL_UP) # GP25 in,  68k RW -  Z80 21pin RD
ds_pin = Pin(26, Pin.IN, Pin.PULL_UP) # GP26 in,  68k DS -  Z80 19pin MREQ
dtack_pin = Pin(27, Pin.OUT, value=0) # GP27 out, 68k DTACK - Z80 24pin WAIT
reset_pin = Pin(28, Pin.OPEN_DRAIN, value=0) # GP28 od, 68k RESET+HALT - Z80 26pin(RESET), 16pin(INT)
print("RESET-ON")


# UART
ACIAD = 0xE000
ACIAC = 0xE001

# Simulate microcontroller memory with bytearray
MEMORY_SIZE = 65536 # 64KB memory
memory = bytearray(MEMORY_SIZE)

test2 = bytearray([
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x41, 0xFA, 0x00, 0x2C, 0x61, 0x00, 0x00, 0x1C, # 0x0000
    0x60, 0xFE, 0x48, 0x40, 0x10, 0x39, 0x00, 0x00, 0xE0, 0x01, 0xC0, 0x3C, 0x00, 0x02, 0x67, 0xF4, # 0x0010
    0x48, 0x40, 0x13, 0xC0, 0x00, 0x00, 0xE0, 0x00, 0x4E, 0x75, 0x10, 0x18, 0x67, 0x00, 0x00, 0x06, # 0x0020
    0x61, 0xE0, 0x60, 0xF6, 0x4E, 0x75, 0x0D, 0x0A, 0x20, 0x42, 0x79, 0x74, 0x65, 0x73, 0x20, 0x66, # 0x0030
    0x72, 0x65, 0x65, 0x0D, 0x0A, 0x0A, 0x45, 0x6E, 0x68, 0x61, 0x6E, 0x63, 0x65, 0x64, 0x20, 0x36, # 0x0040
    0x38, 0x6B, 0x20, 0x42, 0x41, 0x53, 0x49, 0x43, 0x20, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, # 0x0050
    0x20, 0x33, 0x2E, 0x35, 0x34, 0x0D, 0x0A, 0x00                 # 0x0060
])

# アドレス 0x0000 から 0x0015 までのメモリイメージ（計22バイト）
# 0x0000-0x0003: 初期スタックポインタ (仮に 0x00001000 と設定)
# 0x0004-0x0007: 初期PC (プログラム開始アドレス 0x00000008)
# 0x0008-0x0015: 作成したループプログラム

test1 = bytearray([
    # ---- リセットベクトル領域 (0x0000 - 0x0007) ----
    0x00, 0x00, 0x10, 0x00,  # 0x0000: 初期SSP (0x00001000)
    0x00, 0x00, 0x00, 0x08,  # 0x0004: 初期PC  (0x00000008)
    
    # ---- プログラム領域 (0x0008 - 0x0015) ----
    0x41, 0xF8, 0x80, 0x00,  # 0x0008: LEA    $8000, A0
    0x10, 0x10,              # 0x000C: MOVE.B (A0), D0
    0x52, 0x00,              # 0x000E: ADDQ.B #1, D0
    0x10, 0x80,              # 0x0010: MOVE.B D0, (A0)
    0x4E, 0xF8, 0x00, 0x08   # 0x0012: JMP    $0008
])

@micropython.native
def busemu():
    # 標準入力(sys.stdin)の監視設定
    spoll = select.poll()
    spoll.register(sys.stdin, select.POLLIN)
    p_ds = 1
    while True:
        ds = ds_pin.value()
        if (p_ds == 1 and ds == 0):
#            values = [pin.value() for pin in adrs_pins]  # GPIO0-15 読取
#            adrs = sum(v << i for i, v in enumerate(values))  # 数値に変換 

            rw = rw_pin.value()
            if (rw == 1):
                if (adrs == ACIAC):
                    data = 0x02
    #                print("#")
                elif (adrs == ACIAD):
                    data = 0x41
    #                print("@")
                else:
                    data = memory[adrs]
                for i in range(8):
                    if (data & (1 << i)):
                        data_pins[i].value(1)
                    else:
                        data_pins[i].value(0)
                    data_pins[i].init(Pin.OUT)          
            else:
                values = [pin.value() for pin in data_pins]  # GPIO16-23 読取
                data = sum(v << i for i, v in enumerate(values))  # 数値に変換
                if (adrs == ACIAD):
#                    print("[" + chr(data) + "]", end="")
                    print(chr(data), end="")
                else:
                    memory[adrs] = data

#            print("AS:" + str(as_pin.value()), end=" ")
#            print("DS:" + str(ds_pin.value()), end=" ")
#            print("RW:" + str(rw), end=" ")
#            print(f"DATA: {data:02X} ADRS: {adrs:04X}")
        if (p_ds == 0 and ds_pin.value() == 1):
            if (rw_pin.value() == 1):
                for i in range(8):
                    data_pins[i].init(Pin.IN, Pin.PULL_DOWN)
        p_ds = ds
        # 待ち時間0ミリ秒で入力イベントをチェック
        events = spoll.poll(0)
        if events:
            # 入力があれば1文字読み込む
            char = sys.stdin.read(1)
            print(f"入力された文字: {char}")
            # 'q'が入力されたらループを抜ける
            if char == 'q':
                break
        time.sleep_ms(0)
    


load_to_memory(memory, test2, 0x0000)

lin = input("hit-any-key")
print("CLK-ON")
clk_pin = PWM(Pin(29), freq=170, duty_u16=32768) # GP29 out, 68k CLK - Z80 28pin RFSH
time.sleep(2)
reset_pin.value(1)
print("RESET-OFF")
print("Start..")
busemu()

clk_pin.deinit()

