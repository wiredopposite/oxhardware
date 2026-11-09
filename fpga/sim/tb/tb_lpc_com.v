// lpc_com testbench: BIOS reads as in tb_lpc_serve, plus the Super I/O sequence a debug
// kernel uses to find and place its serial port, and 16550 traffic in both directions
// through the real UART pins (115200 8N1). Needs build/sim/bios.hex (256 KB, one byte
// per line).
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
    reg        uart_rx = 1'b1;
    wire       f_cs, f_sck, f_mosi, f_miso;

    // LAD: pulled up, driven by host or target. LFRAME#: host low or target pull-down.
    assign lad = host_oe ? host_lad : 4'bzzzz;
    pullup (lad[0]); pullup (lad[1]); pullup (lad[2]); pullup (lad[3]);
    wire [3:0] lframe_gate;                   // target's MOSFET gate (1 = pull low)
    assign lframe_n = (!host_frame_n || lframe_gate[0]) ? 1'b0 : 1'bz;
    pullup (lframe_n);

    top_lpc_com dut (
        .clk12(clk12), .led_red(), .led_green(), .led_blue(),
        .uart_tx(uart_tx), .uart_rx(uart_rx),
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

    // ---------------------------------------------------------------- I/O cycles
    // Host I/O read; answered=0 if nobody drives SYNC within 4 clocks.
    task io_read(input [15:0] port, output [7:0] d, output answered);
        integer k; reg [3:0] s; reg done;
        begin
            @(posedge lclk); #5 host_frame_n = 0; host_oe = 1; host_lad = 4'h0;
            @(posedge lclk); #5 host_frame_n = 1; host_lad = 4'b0000;
            for (k = 3; k >= 0; k = k - 1) begin @(posedge lclk); #5 host_lad = port[k*4 +: 4]; end
            @(posedge lclk); #5 host_lad = 4'hF;
            @(posedge lclk); #5 host_oe = 0;
            answered = 0; done = 0; k = 0; d = 8'hFF;
            while (!done) begin
                @(posedge lclk); #1 s = lad;
                if (s === 4'b0000) begin answered = 1; done = 1; end
                else if (k == 3) done = 1;
                if (k == 0 && s !== 4'b0000 && dut.lad_oe) begin
                    $display("ERROR: I/O read %h: SYNC not ready on the first clock (%b)", port, s);
                    errors = errors + 1;
                end
                k = k + 1;
            end
            if (answered) begin
                @(posedge lclk); #1 d[3:0] = lad;
                @(posedge lclk); #1 d[7:4] = lad;
                @(posedge lclk); #1;
                if (lad !== 4'b1111) begin $display("ERROR: I/O TAR not 1111"); errors = errors + 1; end
                @(posedge lclk);
            end
            repeat (2) @(posedge lclk);
        end
    endtask

    task io_write(input [15:0] port, input [7:0] d, output answered);
        integer k; reg [3:0] s; reg done;
        begin
            @(posedge lclk); #5 host_frame_n = 0; host_oe = 1; host_lad = 4'h0;
            @(posedge lclk); #5 host_frame_n = 1; host_lad = 4'b0010;
            for (k = 3; k >= 0; k = k - 1) begin @(posedge lclk); #5 host_lad = port[k*4 +: 4]; end
            @(posedge lclk); #5 host_lad = d[3:0];
            @(posedge lclk); #5 host_lad = d[7:4];
            @(posedge lclk); #5 host_lad = 4'hF;
            @(posedge lclk); #5 host_oe = 0;
            answered = 0; done = 0; k = 0;
            while (!done) begin
                @(posedge lclk); #1 s = lad;
                if (s === 4'b0000) begin answered = 1; done = 1; end
                else if (k == 3) done = 1;
                k = k + 1;
            end
            if (answered) begin
                @(posedge lclk); #1;
                if (lad !== 4'b1111) begin $display("ERROR: I/O TAR not 1111"); errors = errors + 1; end
                @(posedge lclk);
            end
            repeat (2) @(posedge lclk);
        end
    endtask

    reg [7:0] rd_d; reg ans;
    task expect_rd(input [15:0] port, input exp_ans, input [7:0] exp_d);
        begin
            io_read(port, rd_d, ans);
            if (ans !== exp_ans || (exp_ans && rd_d !== exp_d)) begin
                $display("ERROR: read %h -> answered=%0d data=%h, expected answered=%0d data=%h",
                         port, ans, rd_d, exp_ans, exp_d);
                errors = errors + 1;
            end
        end
    endtask
    task expect_wr(input [15:0] port, input [7:0] d, input exp_ans);
        begin
            io_write(port, d, ans);
            if (ans !== exp_ans) begin
                $display("ERROR: write %h <- %h answered=%0d, expected %0d", port, d, ans, exp_ans);
                errors = errors + 1;
            end
        end
    endtask

    // ---------------------------------------------------------------- UART (115200 8N1)
    localparam real BIT_NS = 1_000_000_000.0 / 115200.0;
    reg [7:0] got [0:15];
    integer   ngot = 0;
    initial begin : uart_mon
        integer b; reg [7:0] v;
        forever begin
            @(negedge uart_tx);
            #(BIT_NS * 1.5);
            for (b = 0; b < 8; b = b + 1) begin v[b] = uart_tx; #(BIT_NS); end
            if (ngot < 16) got[ngot] = v;
            ngot = ngot + 1;
        end
    end
    task uart_send(input [7:0] v);
        integer b;
        begin
            uart_rx = 0; #(BIT_NS);
            for (b = 0; b < 8; b = b + 1) begin uart_rx = v[b]; #(BIT_NS); end
            uart_rx = 1; #(BIT_NS);
        end
    endtask

    initial begin
        $readmemh("build/sim/bios.hex", bios);
        for (i = 0; i < 262144; i = i + 1) flash.mem[20'h40000 + i] = bios[i];
        #20_000 lrst_n = 1;
        #6_000;
        run(32'hFF000070, 8);                 // measured 1.6 boot pattern (ends the boot hold)
        run(32'hFF000000, 4);
        run(32'hFF000008, 40);

        // Super I/O: data port silent until the enter key
        expect_rd(16'h002F, 0, 8'h00);
        expect_wr(16'h002E, 8'h55, 1);        // enter configuration
        expect_wr(16'h002E, 8'h26, 1);
        expect_rd(16'h002F, 1, 8'h2E);        // the kernel's probe: config port low byte
        expect_wr(16'h002E, 8'h27, 1);
        expect_rd(16'h002F, 1, 8'h00);
        expect_wr(16'h002E, 8'h07, 1); expect_wr(16'h002F, 8'h04, 1);   // LDN 4
        expect_wr(16'h002E, 8'h60, 1); expect_wr(16'h002F, 8'h02, 1);
        expect_wr(16'h002E, 8'h61, 1); expect_wr(16'h002F, 8'hF8, 1);
        expect_wr(16'h002E, 8'h30, 1); expect_wr(16'h002F, 8'h01, 1);
        expect_wr(16'h002E, 8'h60, 1); expect_rd(16'h002F, 1, 8'h02);
        expect_wr(16'h002E, 8'hAA, 1);        // exit
        expect_rd(16'h002F, 0, 8'h00);
        expect_rd(16'h002E, 0, 8'h00);

        // COM moved to 0x2F8; 0x3F8 is no longer ours
        expect_rd(16'h03FD, 0, 8'h00);
        expect_rd(16'h02FD, 1, 8'h60);        // LSR: THRE|TEMT, no data
        expect_wr(16'h02FB, 8'h80, 1);        // DLAB
        expect_wr(16'h02F8, 8'h01, 1);
        expect_wr(16'h02F9, 8'h00, 1);
        expect_rd(16'h02F8, 1, 8'h01);        // DLL reads back
        expect_wr(16'h02FB, 8'h03, 1);
        expect_rd(16'h02FB, 1, 8'h03);
        expect_rd(16'h02FA, 1, 8'h01);        // IIR: no interrupt
        expect_rd(16'h02FE, 1, 8'hB0);        // MSR
        expect_wr(16'h02FF, 8'hA5, 1); expect_rd(16'h02FF, 1, 8'hA5);
        expect_wr(16'h02F8, 8'h4F, 1);        // "OK"
        expect_wr(16'h02F8, 8'h4B, 1);
        run(32'hFF000030, 20);                // serving continues alongside
        io_other(1, 16'h0080, 8'h00);         // unclaimed port still followed correctly
        mem_read_ignored(32'h12345678);

        #300_000;                             // two bytes at 115200
        if (ngot != 2 || got[0] !== 8'h4F || got[1] !== 8'h4B) begin
            $display("ERROR: UART out: %0d bytes (%h %h), expected 4F 4B", ngot, got[0], got[1]);
            errors = errors + 1;
        end

        uart_send(8'h5A);                     // PC -> console
        #20_000;
        expect_rd(16'h02FD, 1, 8'h61);        // data ready
        expect_rd(16'h02F8, 1, 8'h5A);
        expect_rd(16'h02FD, 1, 8'h60);        // popped

        // LRESET#: Super I/O and COM back to defaults (COM at 0x3F8)
        lrst_n = 0; #2_000; lrst_n = 1; #6_000;
        run(32'hFF000070, 8);
        expect_rd(16'h02FD, 0, 8'h00);
        expect_rd(16'h03FD, 1, 8'h60);
        run(32'hFF000100, 20);

        $display("reads=%0d errors=%0d timeouts=%0d max_wait=%0d uart_out=%0d",
                 reads, errors, timeouts, wait_max, ngot);
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
