// USB full-speed transmitter at 48MHz. The SIE hands over one byte at a time with a
// toggle, and the first byte (the PID) starts the packet: SYNC, the bytes, CRC16 if
// b_crc, then EOP. done_tog flips once the line is released.
//
// Every register's next value is worked out a clock before the bit boundary (p2) and
// loaded at it (bnd), which keeps the boundary's logic to one LUT.
`default_nettype none

module usb_phy_tx (
    input  wire       clk,
    input  wire       b_tog,
    input  wire [7:0] b_data,
    input  wire       b_last,
    input  wire       b_crc,
    output reg        b_taken = 1'b0,
    output reg        done_tog = 1'b0,
    output reg        oe = 1'b0,
    output reg        dp = 1'b1,
    output reg        dn = 1'b0
);
    reg [1:0] tog_s = 2'b00;
    reg       seen = 1'b0;
    always @(posedge clk) tog_s <= {tog_s[0], b_tog};
    wire pend = (tog_s[1] != seen);

    localparam [2:0] S_SYNC = 3'd1, S_DATA = 3'd2, S_CRC = 3'd3, S_EOP = 3'd4;
    reg [2:0]  st = S_SYNC;
    reg        idle = 1'b1;
    reg [1:0]  ph = 2'd0;
    reg        start = 1'b0, p2 = 1'b0, bnd = 1'b0;
    reg [3:0]  n = 4'd0;
    reg [7:0]  sh = 8'd0;
    reg        last = 1'b0, crc_en = 1'b0, pid = 1'b0;
    reg [5:0]  ones = 6'd0;
    reg        j = 1'b1;
    reg [15:0] crc = 16'hFFFF;

    always @(posedge clk) begin
        start <= idle && pend && !start;
        ph    <= idle ? 2'd0 : ph + 1'b1;
        p2    <= !idle && ph == 2'd1;
        bnd   <= p2;
    end

    wire stuff  = ones[5];
    wire in_eop = (st == S_EOP);
    wire norm   = !stuff && !in_eop;
    wire bitv   = (st == S_SYNC) ? (n == 4'd7) : (st == S_CRC) ? !crc[15] : sh[0];
    wire flip   = stuff || (!in_eop && !bitv);
    wire e_sync = norm && st == S_SYNC && n == 4'd7;
    wire e_byte = norm && st == S_DATA && n == 4'd7;
    wire e_crc  = norm && st == S_CRC  && n == 4'd15;
    wire take   = e_byte && !last && pend;

    reg [5:0] x_ones = 6'd0;
    reg [3:0] x_n = 4'd0;
    reg [2:0] x_st = S_SYNC;
    reg       x_j = 1'b1, x_dp = 1'b1, x_dn = 1'b0, x_pid = 1'b0, x_seen = 1'b0,
              x_take = 1'b0, x_shr = 1'b0, x_crcd = 1'b0, x_crcc = 1'b0, x_fb = 1'b0,
              x_rel = 1'b0;
    always @(posedge clk) begin
        if (p2) begin
            x_ones <= stuff ? 6'd0 : !norm ? ones : bitv ? {ones[4:0], 1'b1} : 6'd0;
            x_j    <= flip ? ~j : j;
            x_dp   <= flip ? ~j : in_eop ? (n == 4'd2) : dp;
            x_dn   <= flip ? j : in_eop ? 1'b0 : dn;
            x_n    <= stuff ? n : (e_sync || e_byte || e_crc) ? 4'd0 : n + 1'b1;
            x_st   <= e_sync ? S_DATA : e_crc ? S_EOP :
                      (e_byte && !take) ? (crc_en ? S_CRC : S_EOP) : st;
            x_pid  <= e_byte ? 1'b0 : pid;
            x_seen <= take ? tog_s[1] : seen;
            x_take <= take;
            x_shr  <= norm && st == S_DATA;
            x_crcd <= norm && st == S_DATA && !pid;
            x_crcc <= norm && st == S_CRC;
            x_fb   <= bitv ^ crc[15];
            x_rel  <= !stuff && in_eop && n == 4'd3;
        end
    end

    always @(posedge clk) begin
        if (start) begin
            seen    <= tog_s[1];
            b_taken <= ~b_taken;
            sh      <= b_data;
            last    <= b_last;
            crc_en  <= b_crc;
            pid     <= 1'b1;
            crc     <= 16'hFFFF;
            st      <= S_SYNC;
            idle    <= 1'b0;
            n       <= 4'd0;
            ones    <= 6'd0;
            j       <= 1'b1;
            oe      <= 1'b1;
            dp      <= 1'b1;
            dn      <= 1'b0;
        end
        if (bnd) begin
            ones <= x_ones;
            j    <= x_j;
            dp   <= x_dp;
            dn   <= x_dn;
            n    <= x_n;
            st   <= x_st;
            pid  <= x_pid;
            seen <= x_seen;
            if (x_take) begin
                b_taken <= ~b_taken;
                sh      <= b_data;
                last    <= b_last;
            end else if (x_shr) begin
                sh <= {1'b0, sh[7:1]};
            end
            if (x_crcd) crc <= {crc[14:0], 1'b0} ^ (x_fb ? 16'h8005 : 16'h0000);
            if (x_crcc) crc <= {crc[14:0], 1'b1};
            if (x_rel) begin
                oe       <= 1'b0;
                idle     <= 1'b1;
                done_tog <= ~done_tog;
            end
        end
    end
endmodule

`default_nettype wire
