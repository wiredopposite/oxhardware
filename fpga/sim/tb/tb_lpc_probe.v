// Quick iverilog smoke test for top_lpc_probe: fake Xbox power-on, LCLK, LRESET#,
// a few LPC cycles, power-off, then decode the UART report to stdout.
`timescale 1ns/1ps
module tb;
    reg clk12 = 0;
    reg lclk = 0, lrst_n = 0, lframe_n = 1, v33 = 0;
    reg [3:0] lad = 4'hF;
    wire uart_tx;
    reg  uart_rx = 1;
    reg  lclk_en = 0;

    top_lpc_probe dut (
        .clk12(clk12), .led_red(), .led_green(), .led_blue(), .flash_csn(),
        .uart_tx(uart_tx), .uart_rx(uart_rx),
        .lpc_clk(lclk), .lpc_reset_n(lrst_n), .lpc_frame_n(lframe_n), .lpc_lad(lad),
        .v33_sense(v33)
    );

    always #15 if (lclk_en) lclk = ~lclk;   // 33.3 MHz

    task lpc_cycle(input [3:0] ct, input [31:0] addr, input integer nib);
        integer i;
        begin
            @(negedge lclk); lframe_n = 0; lad = 4'h0;          // START
            @(negedge lclk); lframe_n = 1; lad = ct;             // CYCTYPE
            for (i = nib - 1; i >= 0; i = i - 1) begin
                @(negedge lclk); lad = addr[i*4 +: 4];
            end
            repeat (10) @(negedge lclk); lad = 4'hF;             // TAR/SYNC/data (ignored)
        end
    endtask

    // UART decoder (115200)
    localparam real BIT = 1e9 / 115200.0;
    initial begin : uart_mon
        reg [7:0] c; integer k;
        forever begin
            @(negedge uart_tx);
            #(BIT * 1.5);
            for (k = 0; k < 8; k = k + 1) begin c[k] = uart_tx; #(BIT); end
            if (c != 8'h0D) $write("%c", c);
        end
    end

    initial begin
        #1_000_000;                         // let PLL stub/reset settle
        repeat (50) begin #7_000 v33 = 1; #3_000 v33 = 0; end   // noise: must be ignored
        #500_000;
        v33 = 1;                            // t = 0
        #100_000  lclk_en = 1;              // LCLK at ~100 us
        #200_000  lrst_n = 1;               // LRESET# rises at ~300 us
        #5_000;
        lpc_cycle(4'b0100, 32'hFFFF_FFF0, 8);   // mem read reset vector
        lpc_cycle(4'b0100, 32'hFFFF_FFF1, 8);
        lpc_cycle(4'b0000, 32'h0000_002E, 4);   // I/O read 0x2E
        lpc_cycle(4'b0010, 32'h0000_80C0, 4);   // I/O write
        lpc_cycle(4'b0110, 32'hFFF0_0000, 8);   // mem write
        #20_000 lrst_n = 0;
        #10_000 lrst_n = 1;                     // second reset pulse
        lpc_cycle(4'b0100, 32'hFFFF_FFF0, 8);
        #2_000_000 v33 = 0;                     // power off -> report (after 1 ms filter)
        #60_000_000;
        $finish;
    end
endmodule

// Simulation stand-ins for the iCE40 hard blocks used by the top.
module upduino31_clocks #(parameter [2:0] DIVQ = 3'b100) (
    input wire clk12, output reg clk_sys = 0, output wire pll_locked, output wire rst,
    output reg clk_lf = 0);
    always #20.833 clk_sys = ~clk_sys;      // 24 MHz
    always #50000  clk_lf  = ~clk_lf;
    assign pll_locked = 1'b1;
    reg [3:0] r = 0;
    always @(posedge clk_sys) if (!r[3]) r <= r + 1;
    assign rst = !r[3];
endmodule

module SB_RGBA_DRV #(parameter CURRENT_MODE = "0b0", RGB0_CURRENT = "0b0",
                     RGB1_CURRENT = "0b0", RGB2_CURRENT = "0b0") (
    input CURREN, RGBLEDEN, RGB0PWM, RGB1PWM, RGB2PWM, output RGB0, RGB1, RGB2);
    assign RGB0 = !RGB0PWM, RGB1 = !RGB1PWM, RGB2 = !RGB2PWM;
endmodule
