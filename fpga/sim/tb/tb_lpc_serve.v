// lpc_serve testbench: a fake MCPX issues LPC memory reads (the measured 1.6 boot pattern,
// then sequential runs and jumps incl. mirrored addresses) against top_lpc_serve with a
// SPI flash model holding a BIOS image at 0x40000. Every byte is checked against the
// image. Needs build/sim/bios.hex (one byte per line, 256 KB).
`timescale 1ns/1ps
module tb;
    reg clk12 = 0;
    reg lclk = 0, lrst_n = 0;
    reg host_frame_n = 1;
    reg [3:0] host_lad = 4'hF;
    reg       host_oe = 0;
    wire [3:0] lad;
    wire       lframe_n;
    wire       uart_tx;
    wire       f_cs, f_sck, f_mosi, f_miso;

    // LAD: pulled up, driven by host or target. LFRAME#: host low or target pull-down.
    assign lad = host_oe ? host_lad : 4'bzzzz;
    pullup (lad[0]); pullup (lad[1]); pullup (lad[2]); pullup (lad[3]);
    wire [3:0] lframe_gate;                   // target's MOSFET gate (1 = pull low)
    assign lframe_n = (!host_frame_n || lframe_gate[0]) ? 1'b0 : 1'bz;
    pullup (lframe_n);

    top_lpc_serve dut (
        .clk12(clk12), .led_red(), .led_green(), .led_blue(),
        .uart_tx(uart_tx), .uart_rx(1'b1),
        .lpc_clk(lclk), .lpc_reset_n(lrst_n), .lframe_gate(lframe_gate), .lpc_lad(lad),
        .v33_sense(1'b1), .lframe_sense(lframe_n),
        .flash_csn(f_cs), .flash_sck(f_sck), .flash_mosi(f_mosi), .flash_miso(f_miso)
    );
    spi_flash_model flash (.cs_n(f_cs), .sck(f_sck), .mosi(f_mosi), .miso(f_miso));

    always #15 lclk = ~lclk;                  // 33.3 MHz

    reg [7:0] bios [0:262143];
    integer i, reads = 0, errors = 0, timeouts = 0, waits_total = 0, wait_max = 0;
    integer lframe_low_seen = 0;

    task mem_read(input [31:0] a);
        integer k, w; reg [3:0] s; reg [7:0] d; reg done;
        begin
            @(posedge lclk); #5 host_frame_n = 0; host_oe = 1; host_lad = 4'h0;   // START
            @(posedge lclk); #5 host_frame_n = 1; host_lad = 4'b0100;            // CT: mem rd
            for (k = 7; k >= 0; k = k - 1) begin
                @(posedge lclk); #5 host_lad = a[k*4 +: 4];
            end
            @(posedge lclk); #5 host_lad = 4'hF;                                 // TAR1
            @(posedge lclk); #5 host_oe = 0;                                     // TAR2
            w = 0; done = 0;
            while (!done) begin
                @(posedge lclk); #1;
                if (lframe_n === 1'b0) lframe_low_seen = lframe_low_seen + 1;
                s = lad;
                if (s === 4'b0000) done = 1;
                else if (s === 4'b1010) begin done = 1; w = -1; end
                else w = w + 1;
                if (w > 4000) begin done = 1; w = -2; end
            end
            if (w >= 0) begin
                @(posedge lclk); #1 d[3:0] = lad;
                @(posedge lclk); #1 d[7:4] = lad;
                @(posedge lclk); #1;                                             // TAR (1111)
                if (lad !== 4'b1111) begin
                    $display("ERROR: TAR1 not 1111 (%b) after read %h", lad, a); errors = errors + 1;
                end
                @(posedge lclk);                                                 // TAR2
                reads = reads + 1;
                waits_total = waits_total + w;
                if (w > wait_max) wait_max = w;
                if (d !== bios[a[17:0]]) begin
                    errors = errors + 1;
                    if (errors < 10) $display("MISMATCH at %h: got %h want %h", a, d, bios[a[17:0]]);
                end
            end else begin
                timeouts = timeouts + 1;
                $display("TIMEOUT/ERROR (%0d) at %h", w, a);
                repeat (4) @(posedge lclk);
            end
            #6;
            if (lframe_n !== 1'b1 && !dut.holding) begin
                $display("ERROR: LFRAME# still low after cycle at %h", a); errors = errors + 1;
            end
            repeat (2) @(posedge lclk);          // inter-cycle gap
        end
    endtask

    // An I/O cycle answered by some other device (the tb plays both host and that device),
    // with 0000 nibbles in SYNC and data: the target must not drive anything or treat them
    // as a START.
    task io_other(input is_write, input [15:0] port, input [7:0] d);
        integer k;
        begin
            @(posedge lclk); #5 host_frame_n = 0; host_oe = 1; host_lad = 4'h0;   // START
            @(posedge lclk); #5 host_frame_n = 1; host_lad = is_write ? 4'b0010 : 4'b0000;
            for (k = 3; k >= 0; k = k - 1) begin @(posedge lclk); #5 host_lad = port[k*4 +: 4]; end
            if (is_write) begin
                @(posedge lclk); #5 host_lad = d[3:0];
                @(posedge lclk); #5 host_lad = d[7:4];
            end
            @(posedge lclk); #5 host_lad = 4'hF;                                 // TAR
            @(posedge lclk); #5 host_lad = 4'hF;
            @(posedge lclk); #5 host_lad = 4'b0110;                              // other: wait
            @(posedge lclk); #5 host_lad = 4'b0000;                              // other: ready
            if (!is_write) begin
                @(posedge lclk); #5 host_lad = d[3:0];
                @(posedge lclk); #5 host_lad = d[7:4];
            end
            @(posedge lclk); #5 host_lad = 4'hF;                                 // TAR
            @(posedge lclk); #5 host_oe = 0;
            repeat (2) @(posedge lclk);
        end
    endtask

    // the target must stay off the bus during other devices' cycles
    always @(posedge lclk) if (host_oe && dut.lad_oe) begin
        $display("ERROR: target drove LAD during a host/other-device phase at %0t", $time);
        errors = errors + 1;
    end

    // A memory read outside the BIOS window: nobody may answer (LAD stays 1111), and the
    // host gives up after a few clocks.
    task mem_read_ignored(input [31:0] a);
        integer k;
        begin
            @(posedge lclk); #5 host_frame_n = 0; host_oe = 1; host_lad = 4'h0;
            @(posedge lclk); #5 host_frame_n = 1; host_lad = 4'b0100;
            for (k = 7; k >= 0; k = k - 1) begin @(posedge lclk); #5 host_lad = a[k*4 +: 4]; end
            @(posedge lclk); #5 host_lad = 4'hF;
            @(posedge lclk); #5 host_oe = 0;
            for (k = 0; k < 8; k = k + 1) begin
                @(posedge lclk); #1;
                if (dut.lad_oe || lad !== 4'b1111) begin
                    $display("ERROR: answered a read outside FFxxxxxx (%h)", a); errors = errors + 1;
                end
            end
            repeat (2) @(posedge lclk);
        end
    endtask

    task run(input [31:0] a, input integer n);
        integer k; begin for (k = 0; k < n; k = k + 1) mem_read(a + k); end
    endtask

    initial begin
        $readmemh("build/sim/bios.hex", bios);
        for (i = 0; i < 262144; i = i + 1) flash.mem[20'h40000 + i] = bios[i];
        #20_000 lrst_n = 1;
        #6_000;                               // flash wake (tRES1) done
        run(32'hFF000070, 8);                 // measured 1.6 boot pattern
        run(32'hFF000000, 4);
        run(32'hFF000008, 4);
        run(32'hFF00000C, 200);               // long sequential run
        run(32'hFFFFFFF0, 16);                // reset vector, top of the mirror
        run(32'hFF03FFF8, 16);                // crosses the 256 KB mirror boundary
        run(32'hFF012345, 3);                 // random jumps
        run(32'hFF001000, 3);
        mem_read(32'hFF001002);               // short backwards jump
        run(32'hFF001005, 2);                 // short forward skip (in reach)
        io_other(0, 16'h002E, 8'h00);         // I/O read answered by another device, data 00
        mem_read(32'hFF001007);
        io_other(1, 16'h0080, 8'h00);         // I/O write (POST code style), data 00
        io_other(0, 16'h0000, 8'h00);         // port 0000, data 00: all-zero nibbles
        run(32'hFF001008, 4);
        mem_read_ignored(32'h12345678);       // outside FFxxxxxx: must not be answered
        run(32'hFF00100C, 2);
        $display("reads=%0d errors=%0d timeouts=%0d avg_wait=%0d max_wait=%0d clocks, LFRAME#-low samples=%0d",
                 reads, errors, timeouts, waits_total / (reads > 0 ? reads : 1), wait_max, lframe_low_seen);
        if (errors == 0 && timeouts == 0) $display("PASS"); else $display("FAIL");
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
