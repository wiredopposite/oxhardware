// USB full-speed receiver at 48MHz, 4 samples per bit. Each bit is sampled two clocks
// after the last transition. A packet starts after SYNC and ends at EOP; CRC5 and CRC16
// run over everything after the PID and their residues are reported at the end.
// Counters are one-hot and the checks registered to keep paths short at 48MHz.
//
// Events cross to the SIE with a toggle, and ev_kind, ev_data and ev_flags hold until
// the next one, at least 8 clocks later. Kind 0 is a packet start, 1 a byte, 2 the end
// with ev_flags = {err, crc16_ok, crc5_ok}. err is a stuffing error or an EOP off a byte
// boundary.
`default_nettype none

module usb_phy_rx (
    input  wire       clk,
    input  wire       dp,
    input  wire       dn,
    input  wire       en,
    output reg        ev_tog = 1'b0,
    output reg  [1:0] ev_kind = 2'd0,
    output reg  [7:0] ev_data = 8'd0,
    output reg  [2:0] ev_flags = 3'd0,
    output reg        se0_long = 1'b0,
    output wire [1:0] line
);
    reg [1:0] s1 = 2'b10, s2 = 2'b10, s3 = 2'b10;
    always @(posedge clk) begin
        s1 <= {dp, dn};
        s2 <= s1;
        s3 <= s2;
    end
    assign line = s2;

    // Bus reset: SE0 for 4us
    reg [7:0] se0_cnt = 8'd0;
    always @(posedge clk) begin
        if (s2 != 2'b00) se0_cnt <= 8'd0;
        else if (!se0_cnt[7] || !se0_cnt[6]) se0_cnt <= se0_cnt + 1'b1;
        se0_long <= se0_cnt[7] && se0_cnt[6];
    end

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

    reg  last_j = 1'b1;
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

    localparam [1:0] EV_START = 2'd0, EV_BYTE = 2'd1, EV_END = 2'd2;

    reg        active = 1'b0, in_eop = 1'b0, pid_done = 1'b0, err = 1'b0;
    reg [3:0]  zeros = 4'd0;
    reg [5:0]  ones = 6'd0;
    reg [7:0]  nb = 8'd1;
    reg [7:0]  sh = 8'd0;
    reg [4:0]  crc5 = 5'h1F;
    reg [15:0] crc16 = 16'hFFFF;
    reg        c5ok = 1'b0, c16ok = 1'b0;
    always @(posedge clk) begin
        c5ok  <= (crc5 == 5'h0C);
        c16ok <= (crc16 == 16'h800D);
    end

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
            if (a_sync) begin
                active   <= 1'b1;
                pid_done <= 1'b0;
                err      <= 1'b0;
                nb       <= 8'd1;
                ones     <= 6'b000001;               // SYNC's last one counts for stuffing
                crc5     <= 5'h1F;
                crc16    <= 16'hFFFF;
                ev_kind  <= EV_START; ev_tog <= ~ev_tog;
            end
            if (a_stuff) begin
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
