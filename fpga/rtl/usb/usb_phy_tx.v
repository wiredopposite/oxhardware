// USB full-speed transmitter, 48 MHz (one bit every 4 clocks).
//
// Bytes come from the SIE domain one at a time: the SIE sets b_data / b_last / b_crc and
// flips b_tog; the transmitter flips b_taken when it has copied the byte, and the SIE then
// supplies the next one (it has a full byte time to do so). The first byte of a packet
// (the PID) starts it: SYNC, the bytes LSB first, CRC16 over everything after the PID if
// b_crc, then EOP (two bits of SE0, one of J). Bit stuffing and NRZI as usual; a stuffed
// bit after the last six ones goes out before the EOP. done_tog flips once the line is
// released.
//
// Each bit boundary (ph 3) is decided the clock before (ph 2), so the boundary itself
// only applies registered decisions.
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

    localparam [2:0] S_IDLE = 3'd0, S_SYNC = 3'd1, S_DATA = 3'd2, S_CRC = 3'd3,
                     S_EOP = 3'd4;
    reg [2:0]  st = S_IDLE;
    reg [1:0]  ph = 2'd0;
    reg [3:0]  n = 4'd0;
    reg [7:0]  sh = 8'd0;
    reg        last = 1'b0, crc_en = 1'b0, pid = 1'b0;
    reg [5:0]  ones = 6'd0;              // shift: ones[5] = six ones sent
    reg        j = 1'b1;                 // current data state, 1 = J
    reg [15:0] crc = 16'hFFFF;

    // decided at ph 2
    reg q_stuff = 1'b0, q_eop = 1'b0, q_bit = 1'b0, q_take = 1'b0, q_tog = 1'b0,
        q_end_sync = 1'b0, q_end_byte = 1'b0, q_end_crc = 1'b0, q_crcbit = 1'b0,
        q_eop_j = 1'b0, q_release = 1'b0;
    always @(posedge clk) begin
        if (ph == 2'd2) begin
            q_stuff    <= ones[5];
            q_eop      <= (st == S_EOP);
            q_bit      <= (st == S_SYNC) ? (n == 4'd7) : (st == S_CRC) ? !crc[15] : sh[0];
            q_end_sync <= (st == S_SYNC) && n == 4'd7;
            q_end_byte <= (st == S_DATA) && n == 4'd7;
            q_end_crc  <= (st == S_CRC) && n == 4'd15;
            q_crcbit   <= (st == S_DATA) && !pid;
            q_take     <= (st == S_DATA) && n == 4'd7 && !last && pend;
            q_tog      <= tog_s[1];
            q_eop_j    <= (n == 4'd2);
            q_release  <= (n == 4'd3);
        end
    end

    always @(posedge clk) begin
        if (st == S_IDLE) begin
            ph <= 2'd0;
            if (pend) begin                         // the PID
                seen    <= tog_s[1];
                b_taken <= ~b_taken;
                sh      <= b_data;
                last    <= b_last;
                crc_en  <= b_crc;
                pid     <= 1'b1;
                crc     <= 16'hFFFF;
                st      <= S_SYNC;
                n       <= 4'd0;
                ones    <= 6'd0;
                j       <= 1'b1;
                oe      <= 1'b1;
                dp      <= 1'b1;
                dn      <= 1'b0;
            end
        end else begin
            ph <= ph + 1'b1;
            if (ph == 2'd3) begin                   // bit boundary
                if (q_stuff) begin                  // stuffed zero
                    ones <= 6'd0;
                    j  <= ~j;
                    dp <= ~j;
                    dn <= j;
                end else if (q_eop) begin
                    n  <= n + 1'b1;
                    dp <= q_eop_j;
                    dn <= 1'b0;
                    if (q_release) begin
                        oe <= 1'b0;
                        st <= S_IDLE;
                        done_tog <= ~done_tog;
                    end
                end else begin
                    ones <= q_bit ? {ones[4:0], 1'b1} : 6'd0;
                    if (!q_bit) begin
                        j  <= ~j;
                        dp <= ~j;
                        dn <= j;
                    end
                    if (q_crcbit)
                        crc <= {crc[14:0], 1'b0} ^ ((q_bit ^ crc[15]) ? 16'h8005 : 16'h0000);
                    if (st == S_CRC)
                        crc <= {crc[14:0], 1'b1};
                    if (st == S_DATA) sh <= {1'b0, sh[7:1]};
                    n <= n + 1'b1;
                    if (q_end_sync) begin st <= S_DATA; n <= 4'd0; end
                    if (q_end_crc)  begin st <= S_EOP;  n <= 4'd0; end
                    if (q_end_byte) begin
                        n   <= 4'd0;
                        pid <= 1'b0;
                        if (q_take) begin
                            seen    <= q_tog;
                            b_taken <= ~b_taken;
                            sh      <= b_data;
                            last    <= b_last;
                        end else begin
                            st <= crc_en ? S_CRC : S_EOP;
                        end
                    end
                end
            end
        end
    end
endmodule

`default_nettype wire
