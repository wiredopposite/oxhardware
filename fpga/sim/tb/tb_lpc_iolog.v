`timescale 1ns/1ps
module tb;
    reg clk12 = 0;
    reg lclk = 0, lrst_n = 0, lframe_n = 1, v33 = 0, lclk_en = 0;
    reg [3:0] lad_h = 4'hF;
    reg [3:0] lad_t = 4'hF;
    reg       t_en = 0;
    wire [3:0] lad = t_en ? lad_t : lad_h;
    wire uart_tx;

    top_lpc_iolog dut (
        .clk12(clk12), .led_red(), .led_green(), .led_blue(), .flash_csn(),
        .uart_tx(uart_tx), .uart_rx(1'b1),
        .lpc_clk(lclk), .lpc_reset_n(lrst_n), .lpc_frame_n(lframe_n), .lpc_lad(lad),
        .v33_sense(v33)
    );
    always #15 if (lclk_en) lclk = ~lclk;

    task hdr(input [3:0] ct, input [31:0] addr, input integer nib);
        integer i;
        begin
            @(negedge lclk); lframe_n = 0; lad_h = 4'h0;
            @(negedge lclk); lframe_n = 1; lad_h = ct;
            for (i = nib - 1; i >= 0; i = i - 1) begin @(negedge lclk); lad_h = addr[i*4 +: 4]; end
        end
    endtask
    task wr(input [3:0] ct, input [31:0] addr, input integer nib, input [7:0] d);
        begin
            hdr(ct, addr, nib);
            @(negedge lclk); lad_h = d[3:0];
            @(negedge lclk); lad_h = d[7:4];
            @(negedge lclk); lad_h = 4'hF;
            @(negedge lclk);
            @(negedge lclk); t_en = 1; lad_t = 4'h0;
            @(negedge lclk); lad_t = 4'hF;
            @(negedge lclk); t_en = 0;
            repeat (3) @(negedge lclk);
        end
    endtask
    task rd(input [3:0] ct, input [31:0] addr, input integer nib, input [7:0] d,
            input integer waits, input answer);
        integer i;
        begin
            hdr(ct, addr, nib);
            @(negedge lclk); lad_h = 4'hF;
            @(negedge lclk);
            if (answer) begin
                t_en = 1;
                for (i = 0; i < waits; i = i + 1) begin @(negedge lclk); lad_t = 4'b0110; end
                @(negedge lclk); lad_t = 4'b0000;
                @(negedge lclk); lad_t = d[3:0];
                @(negedge lclk); lad_t = d[7:4];
                @(negedge lclk); lad_t = 4'hF;
                @(negedge lclk); t_en = 0;
            end else
                repeat (6) @(negedge lclk);
            repeat (3) @(negedge lclk);
        end
    endtask

    localparam real BIT = 1e9 / 921600.0;
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
        #500_000;
        v33 = 1;
        #50_000  lclk_en = 1;
        #1_200_000 lrst_n = 1;
        #5_000;
        rd(4'b0100, 32'hFF000070, 8, 8'h11, 2, 1);
        wr(4'b0010, 32'h000000EF, 4, 8'h5A);
        rd(4'b0000, 32'h000000EF, 4, 8'h3C, 3, 1);
        rd(4'b0000, 32'h000000EE, 4, 8'h00, 0, 0);
        wr(4'b0110, 32'hFFF05555, 8, 8'hAA);
        #20_000 lrst_n = 0;
        #5_000_000;
        $finish;
    end
endmodule

module upduino31_clocks #(parameter [2:0] DIVQ = 3'b100) (
    input wire clk12, output reg clk_sys = 0, output wire pll_locked, output wire rst,
    output reg clk_lf = 0);
    always #20.833 clk_sys = ~clk_sys;
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
