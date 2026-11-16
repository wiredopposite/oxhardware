`timescale 1ns/1ps
module tb;
    reg clk48 = 0, clk = 0;
    always #10.4165 clk48 = ~clk48;
    always @(posedge clk48) clk = ~clk;

    reg  h_oe = 0, h_dp = 1, h_dn = 0;
    wire d_oe, d_dp, d_dn, pu_en;
    wire dp = d_oe ? d_dp : h_oe ? h_dp : pu_en;
    wire dn = d_oe ? d_dn : h_oe ? h_dn : 1'b0;

    reg         wr = 0, b1_wr = 0;
    reg  [15:0] addr = 0;
    reg  [11:0] b1_addr = 0;
    reg  [7:0]  wdata = 0;
    wire [7:0]  rdata, b1_rdata;
    wire        hit, irq_pulse, irq_level;
    reg         rst = 1;
    usb_core dut (
        .clk48(clk48), .clk(clk), .rst(rst),
        .dp_in(dp), .dn_in(dn), .dp_out(d_dp), .dn_out(d_dn), .oe(d_oe), .pu_en(pu_en),
        .wr(wr), .addr(addr), .wdata(wdata), .rdata(rdata), .hit(hit),
        .b1_wr(b1_wr), .b1_addr(b1_addr), .b1_rdata(b1_rdata),
        .irq_pulse(irq_pulse), .irq_level(irq_level)
    );

    integer errors = 0;
    integer irqs = 0;
    always @(posedge clk) if (irq_pulse) irqs = irqs + 1;

    task rw(input [15:0] a, input [7:0] v);
        begin
            @(posedge clk); addr <= a; wdata <= v; wr <= 1;
            @(posedge clk); wr <= 0;
        end
    endtask
    task rr(input [15:0] a, output [7:0] v);
        begin
            @(posedge clk); addr <= a;
            @(posedge clk); @(posedge clk); v = rdata;
        end
    endtask
    task bw(input [11:0] a, input [7:0] v);
        begin
            @(posedge clk); b1_addr <= a; wdata <= v; b1_wr <= 1;
            @(posedge clk); b1_wr <= 0;
        end
    endtask
    task br(input [11:0] a, output [7:0] v);
        begin
            @(posedge clk); b1_addr <= a;
            @(posedge clk); @(posedge clk); v = b1_rdata;
        end
    endtask
    task expect_reg(input [15:0] a, input [7:0] want, input [8*24-1:0] what);
        reg [7:0] v;
        begin
            rr(a, v);
            if (v !== want) begin
                $display("ERROR: %0s: reg %h = %h, want %h", what, a, v, want);
                errors = errors + 1;
            end
        end
    endtask

    function [4:0] crc5(input [10:0] v);
        integer i; reg [4:0] c;
        begin
            c = 5'h1F;
            for (i = 0; i < 11; i = i + 1)
                c = ((c[0] ^ v[i]) ? ((c >> 1) ^ 5'h14) : (c >> 1));
            crc5 = ~c;
        end
    endfunction
    reg [7:0] pkt [0:79];
    function [15:0] crc16(input integer from, input integer n);
        integer i, b; reg [15:0] c;
        begin
            c = 16'hFFFF;
            for (i = 0; i < n; i = i + 1) begin
                c = c ^ pkt[from + i];
                for (b = 0; b < 8; b = b + 1) c = c[0] ? ((c >> 1) ^ 16'hA001) : (c >> 1);
            end
            crc16 = ~c;
        end
    endfunction

    real bt = 83.333;
    reg  hj;
    integer hones;
    time eop_end;
    task hbit(input b);
        begin
            if (!b) hj = !hj;
            h_dp = hj; h_dn = !hj; #(bt);
            if (b) begin
                hones = hones + 1;
                if (hones == 6) begin hj = !hj; h_dp = hj; h_dn = !hj; #(bt); hones = 0; end
            end else hones = 0;
        end
    endtask
    task hsend(input integer n);
        integer i, b;
        begin
            #(bt * 3);
            hj = 1; hones = 0; h_oe = 1; h_dp = 1; h_dn = 0; #(bt);
            for (i = 0; i < 7; i = i + 1) hbit(0);
            hbit(1); hones = 1;
            for (i = 0; i < n; i = i + 1)
                for (b = 0; b < 8; b = b + 1) hbit(pkt[i][b]);
            h_dp = 0; h_dn = 0; #(bt * 2);
            h_dp = 1; h_dn = 0; #(bt);
            h_oe = 0;
            eop_end = $time;
        end
    endtask
    task token(input [3:0] pid, input [6:0] a, input [3:0] ep);
        reg [4:0] c;
        begin
            c = crc5({ep, a});
            pkt[0] = {~pid, pid};
            pkt[1] = {ep[0], a};
            pkt[2] = {c, ep[3:1]};
            hsend(3);
        end
    endtask
    task hdata(input [3:0] pid, input integer n, input bad);
        reg [15:0] c;
        begin
            pkt[0] = {~pid, pid};
            c = crc16(1, n);
            pkt[n + 1] = c[7:0] ^ {7'd0, bad};
            pkt[n + 2] = c[15:8];
            hsend(n + 3);
        end
    endtask
    task hshake(input [3:0] pid);
        begin pkt[0] = {~pid, pid}; hsend(1); end
    endtask

    reg  [7:0] rx [0:79];
    integer    rx_n;
    time       rx_t0;
    task hrecv(input integer timeout_ns, output ok);
        integer nb, ones, k;
        reg     last, cur, bitv, done;
        reg [7:0] sh;
        time t;
        begin
            ok = 0; rx_n = 0;
            t = $time;
            while (!(d_oe && dp == 0) && ($time - t) < timeout_ns) #1;
            if (d_oe && dp == 0) begin
                ok = 1;
                rx_t0 = $time;
                #(83.333 / 2);
                last = 1; ones = 0; nb = 0; done = 0; k = 0;
                while (!done) begin
                    if (dp == 0 && dn == 0) done = 1;
                    else begin
                        cur = dp;
                        bitv = (cur == last);
                        last = cur;
                        if (k < 8) begin
                            k = k + 1;
                            if (k == 8) ones = 1;
                        end
                        else if (ones == 6) begin
                            if (bitv) begin $display("ERROR: device stuff error"); errors = errors + 1; end
                            ones = 0;
                        end else begin
                            ones = bitv ? ones + 1 : 0;
                            sh = {bitv, sh[7:1]};
                            nb = nb + 1;
                            if (nb == 8) begin rx[rx_n] = sh; rx_n = rx_n + 1; nb = 0; end
                        end
                        #(83.333);
                    end
                end
                if (k == 8 && nb != 0) begin $display("ERROR: device EOP not byte aligned"); errors = errors + 1; end
                while (d_oe) #1;
            end
        end
    endtask

    task expect_hs(input [3:0] pid, input [8*24-1:0] what);
        reg ok; reg [7:0] want;
        begin
            hrecv(2000, ok);
            want = {~pid, pid};
            if (pid == 4'h0) begin
                if (ok) begin $display("ERROR: %0s: unexpected reply %h", what, rx[0]); errors = errors + 1; end
            end else if (!ok || rx_n != 1 || rx[0] !== want) begin
                $display("ERROR: %0s: got %0d bytes (%h), want %h", what, ok ? rx_n : 0, rx[0], want);
                errors = errors + 1;
            end
            if (ok) check_turn(what);
        end
    endtask
    integer turn_max = 0;
    task check_turn(input [8*24-1:0] what);
        integer bits10;
        begin
            bits10 = (rx_t0 - eop_end) * 10 / 83.333;
            if (bits10 > turn_max) turn_max = bits10;
            if (bits10 < 20 || bits10 > 75) begin
                $display("ERROR: %0s: turnaround %0d.%0d bit times", what, bits10 / 10, bits10 % 10);
                errors = errors + 1;
            end
        end
    endtask
    reg [7:0] exp [0:63];
    task expect_data(input [3:0] pid, input integer n, input [8*24-1:0] what);
        reg ok; integer i; reg [15:0] c;
        begin
            hrecv(2000, ok);
            if (!ok) begin $display("ERROR: %0s: no reply", what); errors = errors + 1; end
            else begin
                check_turn(what);
                if (rx[0] !== {~pid, pid} || rx_n != n + 3) begin
                    $display("ERROR: %0s: PID %h, %0d bytes, want %h, %0d", what, rx[0], rx_n - 3,
                             {~pid, pid}, n);
                    errors = errors + 1;
                end else begin
                    for (i = 0; i < n; i = i + 1) begin
                        pkt[1 + i] = rx[1 + i];
                        if (rx[1 + i] !== exp[i]) begin
                            $display("ERROR: %0s: byte %0d = %h, want %h", what, i, rx[1 + i], exp[i]);
                            errors = errors + 1;
                        end
                    end
                    c = crc16(1, n);
                    if ({rx[n + 2], rx[n + 1]} !== c) begin
                        $display("ERROR: %0s: CRC %h%h, want %h", what, rx[n + 2], rx[n + 1], c);
                        errors = errors + 1;
                    end
                end
            end
        end
    endtask

    task setup8(input [6:0] a, input [63:0] req);
        integer i;
        begin
            token(4'hD, a, 0);
            for (i = 0; i < 8; i = i + 1) pkt[1 + i] = req[i*8 +: 8];
            hdata(4'h3, 8, 0);
        end
    endtask
    task map(input [6:0] a, input [7:0] v);
        begin rw(16'h0210, a); rw(16'h0211, v); end
    endtask
    task arm_in(input [3:0] k, input integer n, input [7:0] seed);
        integer i;
        begin
            for (i = 0; i < n; i = i + 1) begin
                exp[i] = seed + i * 8'h1D;
                bw({k, 6'd0} + i, exp[i]);
            end
            bw(12'hD00 + k, n);
        end
    endtask

    reg [7:0] v;
    integer i, irq0;
    initial begin
        #200 rst = 0;
        rw(16'h0200, 8'h03);
        rw(16'h0205, 8'hEF);
        if (pu_en !== 1'b1) begin $display("ERROR: no pull-up"); errors = errors + 1; end

        h_oe = 1; h_dp = 0; h_dn = 0; #20000; h_dp = 1; #100; h_oe = 0;
        expect_reg(16'h0204, 8'h01, "bus reset irq");
        rw(16'h0204, 8'h01);
        expect_reg(16'h0204, 8'h00, "bus reset cleared");
        map(0, 8'h80);

        token(4'h5, 7'h3A, 4'h5);
        #500;
        expect_reg(16'h0202, 8'hBA, "frame lo");
        expect_reg(16'h0203, 8'h02, "frame hi");
        rw(16'h0204, 8'h08);

        bt = 83.54;
        setup8(0, 64'h0040_0000_0100_0680);
        expect_hs(4'h2, "setup ack");
        expect_reg(16'h0206, 8'h01, "setup dev0");
        br(12'h800, v); if (v !== 8'h80) begin $display("ERROR: setup[0] %h", v); errors = errors + 1; end
        br(12'h801, v); if (v !== 8'h06) begin $display("ERROR: setup[1] %h", v); errors = errors + 1; end
        br(12'h807, v); if (v !== 8'h00) begin $display("ERROR: setup[7] %h", v); errors = errors + 1; end
        br(12'h806, v); if (v !== 8'h40) begin $display("ERROR: setup[6] %h", v); errors = errors + 1; end
        rw(16'h0206, 8'h01);
        token(4'h9, 0, 0);
        expect_hs(4'hA, "IN unarmed: NAK");
        arm_in(0, 18, 8'h12);
        exp[3] = 8'hFF; bw(12'h003, 8'hFF);
        exp[4] = 8'hFF; bw(12'h004, 8'hFF);
        bt = 83.12;
        token(4'h9, 0, 0);
        expect_data(4'hB, 18, "descriptor DATA1");
        hshake(4'h2);
        #300;
        expect_reg(16'h0208, 8'h01, "IN_DONE slot 0");
        rw(16'h0208, 8'h01);
        token(4'h9, 0, 0);
        expect_hs(4'hA, "IN again: NAK");
        token(4'h1, 0, 0);
        hdata(4'hB, 0, 0);
        expect_hs(4'h2, "status OUT ack");
        expect_reg(16'h020A, 8'h01, "OUT_DONE slot 16");
        br(12'h840, v); if (v !== 8'h00) begin $display("ERROR: status len %h", v); errors = errors + 1; end
        rw(16'h020A, 8'h01);
        rw(16'h0250, 8'h40);
        bt = 83.333;

        setup8(0, 64'h0000_0000_0005_0500);
        expect_hs(4'h2, "set address ack");
        rw(16'h0206, 8'h01);
        rw(16'h0212, 8'h00);
        rw(16'h0213, 8'h05);
        arm_in(0, 0, 0);
        token(4'h9, 5, 0);
        expect_hs(4'h0, "addr 5 before status");
        token(4'h9, 0, 0);
        expect_data(4'hB, 0, "status ZLP");
        hshake(4'h2);
        #300;
        rw(16'h0208, 8'h01);
        map(0, 8'h00);
        token(4'h9, 0, 0);
        expect_hs(4'h0, "addr 0 after address");
        token(4'h9, 5, 0);
        expect_hs(4'hA, "addr 5 NAK");

        map(0, 8'h81);
        setup8(0, 64'h0012_0000_0100_0680);
        expect_hs(4'h2, "dev1 setup ack");
        expect_reg(16'h0206, 8'h02, "setup dev1");
        br(12'h808, v); if (v !== 8'h80) begin $display("ERROR: dev1 setup[0] %h", v); errors = errors + 1; end
        rw(16'h0206, 8'h02);
        map(6, 8'h81);
        map(0, 8'h00);
        token(4'h9, 6, 0);
        expect_hs(4'hA, "dev1 at 6 NAK");

        bw(12'hC00 + {3'd0, 4'd1, 1'b1}, 8'h88);
        token(4'h9, 5, 1);
        expect_hs(4'h0, "EP1 not enabled");
        rw(16'h0248, 8'h11);
        token(4'h9, 5, 1);
        expect_hs(4'hA, "EP1 NAK");
        arm_in(8, 20, 8'h40);
        token(4'h9, 5, 1);
        expect_data(4'h3, 20, "EP1 DATA0");
        hshake(4'h2);
        #300;
        expect_reg(16'h0209, 8'h01, "IN_DONE slot 8");
        rw(16'h0209, 8'h01);
        arm_in(8, 64, 8'h07);
        token(4'h9, 5, 1);
        expect_data(4'hB, 64, "EP1 DATA1 64");
        #3000;
        token(4'h9, 5, 1);
        expect_data(4'hB, 64, "EP1 DATA1 retry");
        hshake(4'h2);
        #300;
        rw(16'h0209, 8'h01);
        rw(16'h0248, 8'h04);
        token(4'h9, 5, 1);
        expect_hs(4'hE, "EP1 STALL");
        rw(16'h0248, 8'h08);
        token(4'h9, 6, 1);
        expect_hs(4'h0, "dev1 EP1 unmapped");

        bw(12'hC00 + {3'd0, 4'd2, 1'b0}, 8'h89);
        rw(16'h0259, 8'h11);
        token(4'h1, 5, 2);
        for (i = 0; i < 64; i = i + 1) pkt[1 + i] = (i == 10 || i == 11) ? 8'hFF : i * 3;
        hdata(4'h3, 64, 0);
        expect_hs(4'h2, "bulk OUT ack");
        expect_reg(16'h020B, 8'h02, "OUT_DONE slot 25");
        br(12'h849, v); if (v !== 8'd64) begin $display("ERROR: bulk len %0d", v); errors = errors + 1; end
        for (i = 0; i < 64; i = i + 1) begin
            br(12'h400 + 9 * 64 + i, v);
            if (v !== ((i == 10 || i == 11) ? 8'hFF : i * 3)) begin
                $display("ERROR: bulk byte %0d = %h", i, v); errors = errors + 1;
            end
        end
        br(12'h400 + 10 * 64, v);
        rw(16'h020B, 8'h02);
        token(4'h1, 5, 2);
        hdata(4'hB, 4, 0);
        expect_hs(4'hA, "bulk OUT full: NAK");
        rw(16'h0259, 8'h40);
        token(4'h1, 5, 2);
        pkt[1] = 8'hA1; pkt[2] = 8'hA2; pkt[3] = 8'hA3; pkt[4] = 8'hA4;
        hdata(4'hB, 4, 1);
        expect_hs(4'h0, "bad CRC: no reply");
        token(4'h1, 5, 2);
        pkt[1] = 8'hA1; pkt[2] = 8'hA2; pkt[3] = 8'hA3; pkt[4] = 8'hA4;
        hdata(4'hB, 4, 0);
        expect_hs(4'h2, "bulk OUT DATA1 ack");
        expect_reg(16'h020B, 8'h02, "OUT_DONE again");
        br(12'h849, v); if (v !== 8'd4) begin $display("ERROR: len %0d", v); errors = errors + 1; end
        rw(16'h020B, 8'h02);
        rw(16'h0259, 8'h40);
        token(4'h1, 5, 2);
        pkt[1] = 8'hB1; pkt[2] = 8'hB2; pkt[3] = 8'hB3; pkt[4] = 8'hB4;
        hdata(4'hB, 4, 0);
        expect_hs(4'h2, "duplicate ack");
        expect_reg(16'h020B, 8'h00, "duplicate dropped");
        token(4'h1, 5, 2);
        hdata(4'h3, 4, 0);
        expect_hs(4'h2, "bulk OUT DATA0 after duplicate");
        expect_reg(16'h020B, 8'h02, "OUT_DONE after duplicate");
        rw(16'h020B, 8'h02);
        rw(16'h0259, 8'h40);

        rw(16'h0240, 8'h04);
        token(4'h9, 5, 0);
        expect_hs(4'hE, "EP0 STALL");
        setup8(5, 64'h0000_0000_0000_0880);
        expect_hs(4'h2, "setup after stall");
        token(4'h9, 5, 0);
        expect_hs(4'hA, "EP0 NAK after setup");
        rw(16'h0206, 8'h01);

        token(4'h9, 9, 0);
        expect_hs(4'h0, "unknown address");

        irq0 = irqs;
        h_oe = 1; h_dp = 0; h_dn = 0; #20000; h_dp = 1; #100; h_oe = 0;
        expect_reg(16'h0204, 8'h01, "bus reset 2");
        rw(16'h0204, 8'h01);
        if (irqs == irq0) begin $display("ERROR: no irq pulse"); errors = errors + 1; end
        token(4'h9, 5, 0);
        expect_hs(4'h0, "addr 5 gone");
        map(5, 8'h80);
        arm_in(8, 4, 8'h01);
        token(4'h9, 5, 1);
        expect_hs(4'h0, "EP1 disabled by reset");
        token(4'h9, 5, 0);
        expect_hs(4'hA, "EP0 still answers");

        $display("turnaround max %0d.%0d bit times", turn_max / 10, turn_max % 10);
        if (errors == 0) $display("PASS");
        else $display("FAIL: %0d errors", errors);
        $finish;
    end
    initial begin #20_000_000; $display("TIMEOUT"); $finish; end
endmodule
