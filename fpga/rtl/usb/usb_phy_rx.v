// USB full-speed receiver, 48 MHz (4 samples per bit).
//
// D+/D- are synchronised, a bit is sampled two clocks after the last line transition and
// NRZI decoded; the next two clocks decide what it is and unstuff it / assemble bytes. A packet starts after the
// SYNC pattern (three or more zeros, then a one) and ends at EOP (SE0, then J). Both CRCs
// run serially over every bit after the PID; at EOP the residues tell the SIE whether a
// token (CRC5) or data packet (CRC16) was intact. Counters are one-hot and the residue
// checks registered, to keep every path short at 48 MHz.
//
// Events go to the SIE domain through one toggle (ev_tog) with ev_kind / ev_data /
// ev_flags held until the next event, at least 8 clocks later.
//   kind 0: packet start, 1: byte (ev_data), 2: packet end (ev_flags = {err, crc16_ok, crc5_ok})
// err: a bit-stuff error or an EOP that isn't on a byte boundary.
// se0_long: SE0 for more than 4 us (bus reset).
`default_nettype none

module usb_phy_rx (
    input  wire       clk,
    input  wire       dp,
    input  wire       dn,
    input  wire       en,              // 0 while transmitting
    output reg        ev_tog = 1'b0,
    output reg  [1:0] ev_kind = 2'd0,
    output reg  [7:0] ev_data = 8'd0,
    output reg  [2:0] ev_flags = 3'd0,
    output reg        se0_long = 1'b0,
    output wire [1:0] line                // synchronised {dp, dn}
);
    reg [1:0] s1 = 2'b10, s2 = 2'b10, s3 = 2'b10;
    always @(posedge clk) begin
        s1 <= {dp, dn};
        s2 <= s1;
        s3 <= s2;
    end
    assign line = s2;

    // Bus reset: SE0 for 192 clocks
    reg [7:0] se0_cnt = 8'd0;
    always @(posedge clk) begin
        if (s2 != 2'b00) se0_cnt <= 8'd0;
        else if (!se0_cnt[7] || !se0_cnt[6]) se0_cnt <= se0_cnt + 1'b1;
        se0_long <= se0_cnt[7] && se0_cnt[6];
    end

    // Bit timing: resynchronise on every transition, sample two clocks later
    reg [1:0] ph = 2'd0;
    reg       smp = 1'b0;
    always @(posedge clk) begin
        smp <= 1'b0;
        if (s2 != s3) ph <= 2'd1;
        else begin
            ph <= ph + 1'b1;
            smp <= (ph == 2'd1);
        end
    end

    // Stage 1: classify and NRZI-decode the sample
    reg  last_j = 1'b1;                 // previous sampled data state: 1 = J
    reg  v = 1'b0, v_se0 = 1'b0, v_se1 = 1'b0, v_bit = 1'b0;
    always @(posedge clk) begin
        v <= smp && en;
        if (smp) begin
            v_se0 <= (s3 == 2'b00);
            v_se1 <= (s3 == 2'b11);
            v_bit <= (s3[1] == last_j);
            if (s3 == 2'b10 || s3 == 2'b01) last_j <= s3[1];
        end
        if (!en) last_j <= 1'b1;
    end

    // Stage 2: SYNC, unstuffing, bytes, CRCs, EOP
    localparam [1:0] EV_START = 2'd0, EV_BYTE = 2'd1, EV_END = 2'd2;

    reg        active = 1'b0, in_eop = 1'b0, pid_done = 1'b0, err = 1'b0;
    reg [3:0]  zeros = 4'd0;            // shift: zeros[2] = three zeros seen
    reg [5:0]  ones = 6'd0;             // shift: ones[5] = six ones in a row
    reg [7:0]  nb = 8'd1;               // one-hot bit position in the byte
    reg [7:0]  sh = 8'd0;
    reg [4:0]  crc5 = 5'h1F;
    reg [15:0] crc16 = 16'hFFFF;
    reg        c5ok = 1'b0, c16ok = 1'b0;
    always @(posedge clk) begin
        c5ok  <= (crc5 == 5'h0C);
        c16ok <= (crc16 == 16'h800D);
    end

    // Stage 2a: decide what the sample means (state only changes in 2b, and samples are
    // at least two clocks apart)
    reg w = 1'b0, a_end = 1'b0, a_eop = 1'b0, a_drop = 1'b0, a_zero = 1'b0, a_sync = 1'b0,
        a_hunt = 1'b0, a_stuff = 1'b0, a_data = 1'b0, a_byte = 1'b0, w_bit = 1'b0;
    always @(posedge clk) begin
        w <= v;
        w_bit <= v_bit;
        a_eop   <= v_se0;
        a_drop  <= !v_se0 && v_se1;
        a_end   <= !v_se0 && !v_se1 && in_eop;
        a_zero  <= !v_se0 && !v_se1 && !in_eop && !active && !v_bit;
        a_sync  <= !v_se0 && !v_se1 && !in_eop && !active && v_bit && zeros[2];
        a_hunt  <= !v_se0 && !v_se1 && !in_eop && !active && v_bit;
        a_stuff <= !v_se0 && !v_se1 && !in_eop && active && ones[5];
        a_data  <= !v_se0 && !v_se1 && !in_eop && active && !ones[5];
        a_byte  <= !v_se0 && !v_se1 && !in_eop && active && !ones[5] && nb[7];
    end

    // Stage 2b: apply it
    always @(posedge clk) begin
        if (!en) begin
            active <= 1'b0;
            in_eop <= 1'b0;
            zeros  <= 4'd0;
        end else if (w) begin
            if (a_eop) begin
                if (active) in_eop <= 1'b1;
                zeros <= 4'd0;
            end
            if (a_drop || a_end) begin
                active <= 1'b0;
                in_eop <= 1'b0;
                zeros  <= 4'd0;
            end
            if (a_end) begin
                ev_kind <= EV_END; ev_flags <= {err || !nb[0], c16ok, c5ok}; ev_tog <= ~ev_tog;
            end
            if (a_zero) zeros <= {zeros[2:0], 1'b1};
            if (a_hunt) zeros <= 4'd0;
            if (a_sync) begin                        // ...K K: SYNC done
                active   <= 1'b1;
                pid_done <= 1'b0;
                err      <= 1'b0;
                nb       <= 8'd1;
                ones     <= 6'b000001;               // SYNC's final one counts for stuffing
                crc5     <= 5'h1F;
                crc16    <= 16'hFFFF;
                ev_kind  <= EV_START; ev_tog <= ~ev_tog;
            end
            if (a_stuff) begin                       // stuffed bit
                ones <= 6'd0;
                if (w_bit) err <= 1'b1;
            end
            if (a_data) begin
                ones <= w_bit ? {ones[4:0], 1'b1} : 6'd0;
                sh   <= {w_bit, sh[7:1]};
                nb   <= {nb[6:0], nb[7]};
                if (pid_done) begin
                    crc5  <= {crc5[3:0], 1'b0} ^ ((w_bit ^ crc5[4]) ? 5'h05 : 5'h00);
                    crc16 <= {crc16[14:0], 1'b0} ^ ((w_bit ^ crc16[15]) ? 16'h8005 : 16'h0000);
                end
            end
            if (a_byte) begin
                ev_kind <= EV_BYTE; ev_data <= {w_bit, sh[7:1]}; ev_tog <= ~ev_tog;
                pid_done <= 1'b1;
            end
        end
    end
endmodule

`default_nettype wire
