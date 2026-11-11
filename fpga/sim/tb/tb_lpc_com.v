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
    reg        l_sck = 0, l_cs = 1, l_mosi = 0;
    wire       l_miso;
    wire       irq_n;

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
        .flash_csn(f_cs), .flash_sck(f_sck), .flash_mosi(f_mosi), .flash_miso(f_miso),
        .link_sck(l_sck), .link_cs_n(l_cs), .link_mosi(l_mosi), .link_miso(l_miso),
        .fpga_irq_n(irq_n)
    );
    // NOR engine: shorter idle wait for simulation (100 us)
    defparam dut.u_nor.QUIET_CLKS = 2400;
    spi_flash_model flash (.cs_n(f_cs), .sck(f_sck), .mosi(f_mosi), .miso(f_miso));

    always #15 lclk = ~lclk;                  // 33.3 MHz

    reg [7:0] bios [0:262143];
    integer i, reads = 0, errors = 0, timeouts = 0, waits_total = 0, wait_max = 0;
    integer lframe_low_seen = 0;
    reg [7:0] exp_x = 8'h00;                  // expected = image byte ^ exp_x (bank contents)

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
                if (d !== (bios[a[17:0]] ^ exp_x)) begin
                    errors = errors + 1;
                    if (errors < 10) $display("MISMATCH at %h: got %h want %h", a, d, bios[a[17:0]] ^ exp_x);
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

    // ESP32 link master, SPI mode 0 (26.67 MHz unless changed)
    real LHALF = 18.75;
    reg [7:0] lbuf [0:63];
    reg [7:0] lsts;
    task link_xfer(input [7:0] cmd, input [15:0] a, input integer n);   // n data bytes
        integer k, b, tot;
        reg [7:0] o, r;
        begin
            tot = 4 + (cmd[7] ? 1 : 0) + n;
            l_cs = 0; #(LHALF);
            for (k = 0; k < tot; k = k + 1) begin
                o = (k == 0) ? cmd : (k == 1) ? 8'h00 : (k == 2) ? a[15:8] : (k == 3) ? a[7:0]
                  : (!cmd[7]) ? lbuf[k - 4] : 8'h00;
                for (b = 7; b >= 0; b = b - 1) begin
                    l_mosi = o[b]; #(LHALF);
                    l_sck = 1; r[b] = l_miso; #(LHALF);
                    l_sck = 0;
                end
                if (k == 0) lsts = r;
                if (cmd[7] && k >= 5) lbuf[k - 5] = r;
            end
            #(LHALF); l_cs = 1; #100;
        end
    endtask
    task link_expect(input [7:0] cmd, input [15:0] a, input integer n, input [31:0] want);
        integer k;
        begin
            link_xfer(cmd, a, n);
            for (k = 0; k < n; k = k + 1)
                if (lbuf[k] !== want[8*k +: 8]) begin
                    $display("ERROR: link rd %h+%0d = %h, expected %h", a, k, lbuf[k], want[8*k +: 8]);
                    errors = errors + 1;
                end
        end
    endtask

    // ---------------------------------------------------------------- banks
    task lw(input [15:0] a, input [7:0] v);
        begin lbuf[0] = v; link_xfer(8'h00, a, 1); end
    endtask
    task lr(input [15:0] a, input [7:0] v);
        link_expect(8'h80, a, 1, {24'd0, v});
    endtask
    task console_reset;
        begin lrst_n = 0; #2_000; lrst_n = 1; #6_000; run(32'hFF000070, 8); end
    endtask
    // ---------------------------------------------------------------- mailbox
    // SPACE 3 port 1 with NOINC: 0x38 write, 0xB8 read
    task mbx_push(input integer n);             // ESP32 -> console, lbuf[0..n-1]
        link_xfer(8'h38, 16'h0001, n);
    endtask
    task mbx_pull(input integer n, input [63:0] want);   // console -> ESP32
        integer k;
        begin
            link_xfer(8'hB8, 16'h0001, n);
            for (k = 0; k < n; k = k + 1)
                if (lbuf[k] !== want[8*k +: 8]) begin
                    $display("ERROR: mailbox pull +%0d = %h, expected %h", k, lbuf[k], want[8*k +: 8]);
                    errors = errors + 1;
                end
        end
    endtask
    task mbx_tests;
        integer k;
        begin
            // Programmer not up: chip present, Id 'R', counts 0
            lr(16'h0100, 8'h01);
            expect_rd(16'h0B03, 1, "R");
            expect_rd(16'h0B01, 1, 8'h00);
            expect_rd(16'h0B02, 1, 8'h00);
            expect_rd(16'h0B04, 0, 8'h00);
            expect_wr(16'h0B00, 8'h99, 1);              // dropped while not ready
            lr(16'h0103, 8'h00);

            lw(16'h0100, 8'h05);                        // enable | READY
            expect_rd(16'h0B03, 1, "O");
            expect_rd(16'h0B02, 1, 8'h7F);
            expect_rd(16'h0B01, 1, 8'h00);

            // Console -> ESP32: five bytes, IRQ MBX_RX and STS bit 2
            lw(16'h0010, 8'hFF); lw(16'h0011, 8'hFF);
            for (k = 0; k < 5; k = k + 1) expect_wr(16'h0B00, 8'hC0 + k, 1);
            #500;
            lr(16'h0103, 8'h05);
            if (lsts[2] !== 1'b1) begin $display("ERROR: STS %h: no mailbox bit", lsts); errors = errors + 1; end
            lr(16'h0010, 8'h20);
            mbx_pull(2, 64'hC1C0);                      // the link fetches ahead: nothing lost
            lr(16'h0103, 8'h03);
            mbx_pull(3, 64'hC4C3C2);
            lr(16'h0103, 8'h00);
            mbx_pull(2, 64'h0000);                      // empty
            expect_wr(16'h0B00, 8'h7E, 1);
            #500;
            mbx_pull(1, 64'h7E);
            lr(16'h0103, 8'h00);
            expect_rd(16'h0B02, 1, 8'h7F);
            LHALF = 2000;                               // 250 kHz link
            for (k = 0; k < 3; k = k + 1) expect_wr(16'h0B00, 8'h30 + k, 1);
            mbx_pull(2, 64'h3130);
            mbx_pull(1, 64'h32);
            LHALF = 18.75;
            lr(16'h0103, 8'h00);

            // ESP32 -> console
            for (k = 0; k < 4; k = k + 1) lbuf[k] = 8'hA0 + k;
            mbx_push(4);
            link_expect(8'h80, 16'h0105, 2, 32'h01FC);  // TX_FREE 508
            #500;
            expect_rd(16'h0B01, 1, 8'h04);
            for (k = 0; k < 4; k = k + 1) expect_rd(16'h0B00, 1, 8'hA0 + k);
            expect_rd(16'h0B01, 1, 8'h00);
            expect_rd(16'h0B00, 1, 8'h00);              // empty reads 0, takes nothing
            lbuf[0] = 8'h55; mbx_push(1); #500;
            expect_rd(16'h0B00, 1, 8'h55);
            link_expect(8'h80, 16'h0105, 2, 32'h0200);

            // Counts saturate at 0x7F
            for (k = 0; k < 50; k = k + 1) lbuf[k] = k;
            mbx_push(50); mbx_push(50); mbx_push(50);
            #500;
            expect_rd(16'h0B01, 1, 8'h7F);

            // Console reset: both FIFOs dropped, 'R' until the ESP32 clears RESET_REQ
            expect_wr(16'h0B00, 8'h11, 1);
            expect_wr(16'h0B03, 8'h01, 1);
            expect_rd(16'h0B03, 1, "R");
            expect_rd(16'h0B01, 1, 8'h00);
            lr(16'h0100, 8'h07);
            lr(16'h0103, 8'h00);
            lbuf[0] = 8'hEE; mbx_push(1);               // stale reply bytes from before
            lw(16'h0100, 8'h07);                        // clear RESET_REQ (W1C)
            #500;
            lr(16'h0100, 8'h05);
            expect_rd(16'h0B03, 1, "O");
            expect_rd(16'h0B01, 1, 8'h00);
            link_expect(8'h80, 16'h0105, 2, 32'h0200);
            expect_wr(16'h0B00, 8'h22, 1); #500;
            mbx_pull(1, 64'h22);

            // Moving the ports, disabling them
            lw(16'h0101, 8'h00); lw(16'h0102, 8'h0D);
            expect_rd(16'h0B03, 0, 8'h00);
            expect_rd(16'h0D03, 1, "O");
            lw(16'h0101, 8'h00); lw(16'h0102, 8'h0B);
            lw(16'h0100, 8'h04);
            expect_rd(16'h0B03, 0, 8'h00);
            lw(16'h0100, 8'h05);
            expect_rd(16'h0B03, 1, "O");
            run(32'hFF000300, 8);
        end
    endtask

    // ---------------------------------------------------------------- NOR engine
    task nor_wait;                              // until NOR_STATUS busy clears
        begin
            lbuf[0] = 8'h04;
            while (lbuf[0] & 8'h04) begin #5_000; link_xfer(8'h80, 16'h00C0, 1); end
        end
    endtask
    task nor_op(input [23:0] a, input [7:0] len_m1, input [7:0] op);
        begin
            lw(16'h00C8, a[7:0]); lw(16'h00C9, a[15:8]); lw(16'h00CA, a[23:16]);
            lw(16'h00CC, len_m1);
            lw(16'h00CB, op);
        end
    endtask
    task nor_check(input [23:0] a, input integer n, input [7:0] xr);   // buffer vs mem ^ xr
        integer k;
        begin
            nor_op(a, n - 1, 8'd5); nor_wait;
            link_xfer(8'hB8, 16'h0003, n);
            for (k = 0; k < n; k = k + 1)
                if (lbuf[k] !== (flash.mem[a + k] ^ xr)) begin
                    $display("ERROR: NOR read %h = %h, expected %h", a + k, lbuf[k], flash.mem[a + k] ^ xr);
                    errors = errors + 1;
                end
        end
    endtask
    function [31:0] crc_mem(input [19:0] a, input integer n);
        integer k, b; reg [31:0] c; reg [7:0] v;
        begin
            c = 32'hFFFFFFFF;
            for (k = 0; k < n; k = k + 1) begin
                v = flash.mem[a + k];
                for (b = 0; b < 8; b = b + 1) begin
                    c = (c[0] ^ v[b]) ? ((c >> 1) ^ 32'hEDB88320) : (c >> 1);
                end
            end
            crc_mem = ~c;
        end
    endfunction
    task crc_check(input [23:0] a, input [23:0] n);
        reg [31:0] want;
        begin
            want = crc_mem(a[19:0], n);
            lw(16'h00D0, a[7:0]); lw(16'h00D1, a[15:8]); lw(16'h00D2, a[23:16]);
            lw(16'h00D3, n[7:0]); lw(16'h00D4, n[15:8]); lw(16'h00D5, n[23:16]);
            lw(16'h00D6, 8'h01);
        end
    endtask
    task crc_expect(input [23:0] a, input [23:0] n);
        reg [31:0] want;
        begin
            want = crc_mem(a[19:0], n);
            nor_wait;
            link_expect(8'h80, 16'h00D8, 4, want);
        end
    endtask
    task nor_tests;
        integer k, t0, wmax0;
        begin
            // Idle after boot: the engine has woken the flash and read its ID
            #200_000;
            link_expect(8'h80, 16'h00C0, 1, 32'h01);            // init done, idle
            link_expect(8'h80, 16'h00C2, 3, 32'h1640EF);

            // Read into the buffer
            nor_check(24'h040000, 16, 8'h00);
            nor_check(24'h080010, 40, 8'h00);

            // Erase 4 K, program 16 bytes, then 100 (chunked) across a page's start
            nor_op(24'h0F0000, 0, 8'd1); nor_wait;
            nor_check(24'h0F0000, 16, 8'h00);
            if (flash.mem[20'hF0000] !== 8'hFF || flash.mem[20'hF0FFF] !== 8'hFF) begin
                $display("ERROR: erase 4 K"); errors = errors + 1;
            end
            for (k = 0; k < 16; k = k + 1) lbuf[k] = 8'h10 + k;
            link_xfer(8'h38, 16'h0003, 16);
            nor_op(24'h0F0010, 15, 8'd3); nor_wait;
            for (k = 0; k < 16; k = k + 1)
                if (flash.mem[20'hF0010 + k] !== 8'h10 + k) begin
                    $display("ERROR: program %h = %h", 20'hF0010 + k, flash.mem[20'hF0010 + k]);
                    errors = errors + 1;
                end
            for (k = 0; k < 63; k = k + 1) lbuf[k] = 8'h80 ^ k;
            link_xfer(8'h38, 16'h0003, 63);
            for (k = 0; k < 37; k = k + 1) lbuf[k] = 8'hC0 ^ k;
            link_xfer(8'h38, 16'h0003, 37);              // a new transaction starts at 0
            nor_op(24'h0F0100, 36, 8'd3); nor_wait;
            for (k = 0; k < 37; k = k + 1)
                if (flash.mem[20'hF0100 + k] !== (8'hC0 ^ k)) begin
                    $display("ERROR: program %h = %h", 20'hF0100 + k, flash.mem[20'hF0100 + k]);
                    errors = errors + 1;
                end
            if (flash.mem[20'hF0125] !== 8'hFF) begin $display("ERROR: programmed too far"); errors = errors + 1; end
            nor_op(24'h0F00F0, 31, 8'd3);                // crosses a page: refused
            link_expect(8'h80, 16'h00C0, 1, 32'h21);

            // Bitstream area: refused unless unlocked, for one operation
            nor_op(24'h010000, 0, 8'd1);
            link_expect(8'h80, 16'h00C0, 1, 32'h21);
            lw(16'h00CD, 8'h5A);
            nor_op(24'h010000, 0, 8'd1); nor_wait;
            link_expect(8'h80, 16'h00C0, 1, 32'h01);
            nor_op(24'h011000, 0, 8'd1);
            link_expect(8'h80, 16'h00C0, 1, 32'h21);

            // CRC of slot 0, interrupted by BIOS reads: they wait for the engine, it
            // continues afterwards, the result is right
            lw(16'h0010, 8'hFF); lw(16'h0011, 8'hFF);
            crc_check(24'h040000, 24'h001000);
            #150_000;                                   // the engine holds the flash now
            wmax0 = wait_max; wait_max = 0;
            run(32'hFF000400, 16);
            $display("NOR: BIOS reads during CRC, longest wait %0d clocks", wait_max);
            crc_expect(24'h040000, 24'h001000);
            link_expect(8'h80, 16'h0011, 1, 32'h01);    // CRC_DONE

            // Erase 64 K, interrupted: suspended for the BIOS reads, resumed after
            nor_op(24'h0E0000, 0, 8'd2);
            #150_000;
            wait_max = 0;
            run(32'hFF000500, 16);
            $display("NOR: BIOS reads during erase, longest wait %0d clocks, suspends %0d",
                     wait_max, flash.suspends);
            if (flash.suspends == 0) begin $display("ERROR: erase was not suspended"); errors = errors + 1; end
            nor_wait;
            if (flash.mem[20'hE0000] !== 8'hFF || flash.mem[20'hEFFFF] !== 8'hFF) begin
                $display("ERROR: erase 64 K"); errors = errors + 1;
            end
            if (wmax0 > wait_max) wait_max = wmax0;

            // Console in reset: the engine works without waiting for BIOS reads to stop
            lrst_n = 0; #150_000;
            nor_check(24'h040100, 8, 8'h00);
            console_reset;
            run(32'hFF000600, 8);

            if (flash.violations != 0) begin
                $display("ERROR: %0d flash access violations", flash.violations);
                errors = errors + 1;
            end
        end
    endtask

    task bank_tests;
        begin
            // defaults
            lr(16'h0020, 8'h15); lr(16'h0022, 8'h00); lr(16'h0027, 8'h00);
            lr(16'h0038, 8'hE0); lr(16'h0039, 8'h00); lr(16'h003A, 8'h00);
            expect_rd(16'h00E0, 1, 8'h4F);
            expect_rd(16'h00E2, 1, 8'hFF);                        // nothing armed
            expect_rd(16'h00E7, 1, 8'h00);
            expect_rd(16'h00E8, 0, 8'h00);                        // not ours
            lw(16'h003C, 8'h01);                                  // SYSTEM_BANK
            expect_rd(16'h00E5, 1, 8'h01);

            // the Loader arms bank 1 and resets: one boot of bank 1, then the default
            lw(16'h0010, 8'hFF); lw(16'h0011, 8'hFF);             // clear what earlier resets raised
            lw(16'h0012, 8'h01);                                  // IRQ: LRST_ASSERT
            lw(16'h0013, 8'h08);                                  //      BOOTCTL
            expect_wr(16'h00E2, 8'h01, 1);
            expect_rd(16'h00E2, 1, 8'h01);
            expect_rd(16'h00E1, 1, 8'h01);                        // armed
            lr(16'h0011, 8'h08);                                  // BOOTCTL raised
            if (irq_n !== 1'b0) begin $display("ERROR: IRQ_N not low"); errors = errors + 1; end
            lw(16'h0011, 8'h08);                                  // W1C
            if (irq_n !== 1'b1) begin $display("ERROR: IRQ_N not released"); errors = errors + 1; end
            lr(16'h0021, 8'h0D);                                  // swap armed
            exp_x = 8'h5A; console_reset;
            lr(16'h0010, 8'h03);                                  // LRST assert + deassert
            if (irq_n !== 1'b0) begin $display("ERROR: IRQ_N not low after reset"); errors = errors + 1; end
            lw(16'h0010, 8'h03);
            lr(16'h0022, 8'h01);
            lr(16'h003A, 8'h02);                                  // ARMED_BOOT
            expect_rd(16'h00E7, 1, 8'h01);
            run(32'hFF000100, 20); run(32'hFF040100, 4);
            expect_wr(16'h00E3, 8'hA5, 1);                        // confirm
            expect_rd(16'h00E1, 1, 8'h12);
            exp_x = 8'h00; console_reset;
            lr(16'h0022, 8'h00); lr(16'h003A, 8'h00);
            run(32'hFF000100, 20);

            // ESP32 arms bank 1 on trial
            lw(16'h0023, 8'h01); lw(16'h003A, 8'h04); lw(16'h0020, 8'h1D);
            lr(16'h003A, 8'h21);                                  // armed, trial pending
            exp_x = 8'h5A; console_reset;
            lr(16'h003A, 8'h06);                                  // ARMED_BOOT, TRIAL
            run(32'hFF000200, 8);
            exp_x = 8'h00; console_reset;
            lr(16'h003A, 8'h00);
            run(32'hFF000200, 8);

            // ESP32 disarms again before the reset
            expect_wr(16'h00E2, 8'h02, 1); lw(16'h0020, 8'h15);
            lr(16'h0020, 8'h15); expect_rd(16'h00E2, 1, 8'hFF);

            // LAST_FAILED: set by the ESP32, cleared by the Loader
            lw(16'h003A, 8'h08);
            expect_rd(16'h00E1, 1, 8'h08);
            expect_wr(16'h00E1, 8'h08, 1);
            lr(16'h003A, 8'h00);

            // 512 K bank at slot 1 (slots 1-2), switched immediately
            lw(16'h0022, 8'h41);
            #1_000;
            exp_x = 8'h5A; run(32'hFF000300, 8); run(32'hFF080300, 4);
            exp_x = 8'hA5; run(32'hFF040300, 8); run(32'hFFFC0300, 4);
            lr(16'h002A, 8'h0C);                                  // LAST_MEM_ADDR in slot 2
            exp_x = 8'h00; lw(16'h0022, 8'h00);
            #1_000; run(32'hFF000300, 8);

            // the port moves with BOOTCTL_BASE
            lw(16'h0038, 8'h40); lw(16'h0039, 8'h0C);
            expect_rd(16'h00E0, 0, 8'h00);
            expect_rd(16'h0C40, 1, 8'h4F);
        end
    endtask

    initial begin
        $readmemh("build/sim/bios.hex", bios);
        for (i = 0; i < 262144; i = i + 1) begin
            flash.mem[20'h40000 + i] = bios[i];               // slot 0
            flash.mem[20'h80000 + i] = bios[i] ^ 8'h5A;       // slot 1
            flash.mem[20'hC0000 + i] = bios[i] ^ 8'hA5;       // slot 2
        end
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

        // ESP32 link: identity, scratch, NOINC, status byte
        link_expect(8'h80, 16'h0000, 4, "GFXO");                // 'O','X','F','G'
        lbuf[0] = 8'h11; lbuf[1] = 8'h22; lbuf[2] = 8'h33; lbuf[3] = 8'h44;
        link_xfer(8'h00, 16'h000C, 4);
        link_expect(8'h80, 16'h000C, 4, 32'h44332211);
        link_expect(8'h88, 16'h000D, 3, 32'h00222222);          // NOINC
        if (lsts !== 8'h60) begin
            $display("ERROR: link STS %h, expected 60", lsts); errors = errors + 1;
        end
        link_expect(8'h80, 16'h0021, 1, 32'h05);                 // LRESET# high, served
        link_expect(8'h80, 16'h0007, 3, 32'h006F01);             // BOARD_ID, FEATURES
        LHALF = 2000;                                            // 250 kHz
        link_expect(8'h80, 16'h0000, 4, "GFXO");
        link_expect(8'h80, 16'h000C, 4, 32'h44332211);
        LHALF = 18.75;

        // A 300 ns LRESET# glitch is filtered: COM stays where the guest put it
        lrst_n = 0; #300; lrst_n = 1; #3_000;
        expect_rd(16'h02FD, 1, 8'h60);
        run(32'hFF000200, 8);

        // LRESET#: Super I/O and COM back to defaults (COM at 0x3F8)
        lrst_n = 0; #2_000; lrst_n = 1; #6_000;
        run(32'hFF000070, 8);
        expect_rd(16'h02FD, 0, 8'h00);
        expect_rd(16'h03FD, 1, 8'h60);
        run(32'hFF000100, 20);

        bank_tests;
        mbx_tests;
        nor_tests;

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
