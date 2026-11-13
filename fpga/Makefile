# OpenOX FPGA build
#   make                         build DESIGN for BOARD (defaults: hello, upduino31)
#   make DESIGN=lpc_probe prog   build and write to the board's config flash with iceprog
#   make DESIGN=lpc_probe sim    run the design's iverilog testbench (sim/tb/tb_<design>.v)
#   make sim-usb                 run the USB device testbench (sim/tb/tb_usb.v)
#   make rom ROM=<file>          write a ROM image to flash offset ROM_OFFSET (default 0x40000)
#   make clean
#
# Needs OSS CAD Suite's bin and lib folders on PATH (VS Code workspace terminals
# already have them; elsewhere run oss-cad-suite\environment.ps1 first).

BOARD  ?= upduino31
DESIGN ?= hello
TOP    := top_$(DESIGN)
BUILD  := build/$(BOARD)/$(DESIGN)
PCF    := boards/$(BOARD)/$(BOARD).pcf

UART_RTL := rtl/uart/uart_tx.v rtl/uart/uart_rx.v
USB_RTL  := rtl/usb/usb_phy_rx.v rtl/usb/usb_phy_tx.v rtl/usb/usb_sie.v rtl/usb/usb_core.v

RTL_hello     := $(UART_RTL)
RTL_lpc_probe := $(UART_RTL) rtl/probe/lpc_sniff.v boards/$(BOARD)/$(BOARD)_clocks.v
RTL_lpc_iolog := $(UART_RTL) rtl/probe/lpc_sniff.v rtl/common/spram64.v boards/$(BOARD)/$(BOARD)_clocks.v
RTL_lpc_serve := $(UART_RTL) rtl/lpc/lpc_target.v rtl/mem/spi_stream.v rtl/common/word_report.v rtl/probe/trace_buf.v rtl/common/trace_dump.v boards/$(BOARD)/$(BOARD)_clocks.v
RTL_lpc_com   := $(UART_RTL) rtl/lpc/lpc_target.v rtl/lpc/lreset_filter.v rtl/lpc/lclk_detect.v rtl/lpc/com16550.v rtl/lpc/superio_cfg.v rtl/common/async_fifo.v rtl/mem/spi_stream.v rtl/link/link_slave.v rtl/link/link_regs.v rtl/lpc/bootctl_port.v rtl/boot/bank_ctrl.v rtl/common/mbx_fifo.v rtl/lpc/mailbox.v rtl/mem/flash_share.v $(USB_RTL) boards/$(BOARD)/$(BOARD)_clocks.v

RTL := $(RTL_$(DESIGN)) boards/$(BOARD)/$(TOP).v

# System clock target per design (MHz); optional extra clock constraints (nextpnr python)
FREQ_hello     := 48
FREQ_lpc_probe := 24
FREQ_lpc_iolog := 24
FREQ_lpc_serve := 24
FREQ_lpc_com   := 24
FREQ           := $(or $(FREQ_$(DESIGN)),48)
CLKPY          := $(wildcard boards/$(BOARD)/$(DESIGN)_clocks.py)
PNR_EXTRA      := $(if $(CLKPY),--pre-pack $(CLKPY))

all: $(BUILD)/$(TOP).bin

# lpc_serve head cache: first 4 KB of the ROM image as $(readmemh) data
HEAD_HEX := build/rom_head.hex
ifeq ($(DESIGN),lpc_serve)
$(BUILD)/$(TOP).json: $(HEAD_HEX)
endif
$(HEAD_HEX):
	python -c "import sys,os; os.makedirs('build',exist_ok=True); d=open(r'$(ROM)','rb').read()[:4096]; open(r'$@','w').write(chr(10).join('%02x'%b for b in d)+chr(10))"

$(BUILD)/$(TOP).json: $(RTL)
	python -c "import os; os.makedirs('$(BUILD)', exist_ok=True)"
	yosys -q -l $(BUILD)/yosys.log -p "synth_ice40 -top $(TOP) -json $@" $(RTL)

$(BUILD)/$(TOP).asc: $(BUILD)/$(TOP).json $(PCF) $(CLKPY)
	nextpnr-ice40 -q -l $(BUILD)/nextpnr.log --up5k --package sg48 --freq $(FREQ) $(PNR_EXTRA) --pcf $(PCF) --json $< --asc $@

$(BUILD)/$(TOP).bin: $(BUILD)/$(TOP).asc
	icepack $< $@

prog: $(BUILD)/$(TOP).bin
	iceprog $<

ROM_OFFSET ?= 0x40000
rom:
	iceprog -o $(ROM_OFFSET) $(ROM)

SIM_MODELS_lpc_serve := sim/models/spi_flash_model.v
SIM_MODELS_lpc_com   := sim/models/spi_flash_model.v
SIM_TB := sim/tb/tb_$(DESIGN).v $(SIM_MODELS_$(DESIGN))

sim: $(SIM_TB) $(filter-out boards/$(BOARD)/$(BOARD)_clocks.v,$(RTL))
	python -c "import os; os.makedirs('build/sim', exist_ok=True)"
	iverilog -g2012 -o build/sim/tb_$(DESIGN).vvp $^
	vvp -n build/sim/tb_$(DESIGN).vvp

sim-usb: sim/tb/tb_usb.v $(USB_RTL)
	python -c "import os; os.makedirs('build/sim', exist_ok=True)"
	iverilog -g2012 -o build/sim/tb_usb.vvp $^
	vvp -n build/sim/tb_usb.vvp

clean:
	python -c "import shutil; shutil.rmtree('build', ignore_errors=True)"

.PHONY: all prog rom sim sim-usb clean
