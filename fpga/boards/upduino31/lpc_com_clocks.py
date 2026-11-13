# nextpnr pre-pack timing constraints for DESIGN=lpc_com
# (--freq sets the 24 MHz system clock; USB line side 48 MHz; the Xbox LPC clock is
# 33.33 MHz, the ESP32 link clock at most 20 MHz)
ctx.addClock("lpc_clk", 34)
ctx.addClock("link_sck", 27)
ctx.addClock("clk48", 48)
