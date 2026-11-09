// Passive LPC cycle sniffer (LCLK domain). Never drives anything.
//
// Decodes START, CYCTYPE/DIR (or IDSEL for firmware-hub cycles), the address and, for
// target I/O / memory cycles, the data byte, then publishes the record with a toggle
// (rec_toggle) for another clock domain to pick up. Records are >= ~10 LCLKs apart, so
// a single holding register is enough for a 24/48 MHz consumer.
//
// Intel LPC 1.1 target cycles (START = 0000), one nibble per LCLK, LFRAME# low = START:
//   write:  START CT A.. D[3:0] D[7:4] TAR TAR SYNC.. TAR TAR
//   read:   START CT A.. TAR TAR SYNC.. D[3:0] D[7:4] TAR TAR
// SYNC 0000 = ready, 0101/0110 = short/long wait, 1010 = error. If nothing answers the
// bus floats to 1111; after 3 such SYNC clocks the cycle is recorded as "no response".
// Writes are recorded once the data byte is seen (their SYNC is not checked).
// Firmware-hub cycles (START 1101/1110: IDSEL + 7 address nibbles) and DMA/bus-master
// cycles are recorded at the end of the header without data.
`default_nettype none

module lpc_sniff (
    input  wire        lclk,
    input  wire        lreset_n,
    input  wire        lframe_n,
    input  wire [3:0]  lad,
    output reg  [3:0]  rec_start,
    output reg  [3:0]  rec_ct,
    output reg  [31:0] rec_addr,
    output reg  [7:0]  rec_data,
    output reg         rec_err,          // read: SYNC error or no response (data = FF)
    output reg         rec_toggle = 1'b0,
    output wire        lclk_div16        // toggles every 16 LCLKs (activity / frequency)
);
    localparam [2:0] S_IDLE = 3'd0, S_CT = 3'd1, S_ADDR = 3'd2, S_WDATA = 3'd3,
                     S_TAR = 3'd4, S_SYNC = 3'd5, S_RDATA = 3'd6;

    // Register inputs once on LCLK rising (host/target drive on the rising edge, data
    // valid well before the next one).
    reg [3:0] lad_q;
    reg       lframe_q = 1'b1, lrst_q = 1'b0;
    always @(posedge lclk) begin
        lad_q    <= lad;
        lframe_q <= lframe_n;
        lrst_q   <= lreset_n;
    end

    reg [4:0] div = 0;
    always @(posedge lclk) div <= div + 1'b1;
    assign lclk_div16 = div[4];

    reg [2:0]  state = S_IDLE;
    reg [3:0]  start, ct, nib_left;
    reg [31:0] addr;
    reg [3:0]  dlo;
    reg [1:0]  cnt;
    reg        err;

    wire tgt_write = (start == 4'b0000) && (ct[3] == 1'b0) && ct[1];
    wire tgt_read  = (start == 4'b0000) && (ct[3] == 1'b0) && !ct[1];

    always @(posedge lclk) begin
        if (!lrst_q) begin
            state <= S_IDLE;
        end else if (!lframe_q) begin
            start <= lad_q;                 // last nibble seen with LFRAME# low wins
            state <= S_CT;
        end else begin
            case (state)
            S_CT: begin
                ct   <= lad_q;
                addr <= 32'd0;
                if (start == 4'b0000) begin
                    if (lad_q[3:2] == 2'b00) begin           // I/O read/write
                        nib_left <= 4'd4;
                        state    <= S_ADDR;
                    end else if (lad_q[3:2] == 2'b01) begin  // memory read/write
                        nib_left <= 4'd8;
                        state    <= S_ADDR;
                    end else begin                           // DMA / reserved
                        rec_start  <= start;
                        rec_ct     <= lad_q;
                        rec_addr   <= 32'd0;
                        rec_data   <= 8'h00;
                        rec_err    <= 1'b0;
                        rec_toggle <= ~rec_toggle;
                        state      <= S_IDLE;
                    end
                end else if (start == 4'b1101 || start == 4'b1110) begin   // FWH
                    nib_left <= 4'd7;
                    state    <= S_ADDR;
                end else if (start == 4'b1111) begin         // stop / abort
                    state <= S_IDLE;
                end else begin                               // bus master etc.
                    rec_start  <= start;
                    rec_ct     <= lad_q;
                    rec_addr   <= 32'd0;
                    rec_data   <= 8'h00;
                    rec_err    <= 1'b0;
                    rec_toggle <= ~rec_toggle;
                    state      <= S_IDLE;
                end
            end
            S_ADDR: begin
                addr     <= {addr[27:0], lad_q};
                nib_left <= nib_left - 1'b1;
                if (nib_left == 4'd1) begin
                    if (tgt_write) begin
                        state <= S_WDATA;
                        cnt   <= 2'd0;
                    end else if (tgt_read) begin
                        state <= S_TAR;
                        cnt   <= 2'd0;
                    end else begin                           // FWH: header only
                        rec_start  <= start;
                        rec_ct     <= ct;
                        rec_addr   <= {addr[27:0], lad_q};
                        rec_data   <= 8'h00;
                        rec_err    <= 1'b0;
                        rec_toggle <= ~rec_toggle;
                        state      <= S_IDLE;
                    end
                end
            end
            S_WDATA:                                         // D[3:0], then D[7:4]
                if (cnt == 2'd0) begin
                    dlo <= lad_q;
                    cnt <= 2'd1;
                end else begin
                    rec_start  <= start;
                    rec_ct     <= ct;
                    rec_addr   <= addr;
                    rec_data   <= {lad_q, dlo};
                    rec_err    <= 1'b0;
                    rec_toggle <= ~rec_toggle;
                    state      <= S_IDLE;
                end
            S_TAR:                                           // two turnaround clocks
                if (cnt == 2'd1) begin
                    state <= S_SYNC;
                    cnt   <= 2'd0;
                end else
                    cnt <= cnt + 1'b1;
            S_SYNC:
                case (lad_q)
                    4'b0000: begin err <= 1'b0; cnt <= 2'd0; state <= S_RDATA; end
                    4'b1010: begin err <= 1'b1; cnt <= 2'd0; state <= S_RDATA; end
                    4'b1111:
                        if (cnt == 2'd2) begin                // nobody answered
                            rec_start  <= start;
                            rec_ct     <= ct;
                            rec_addr   <= addr;
                            rec_data   <= 8'hFF;
                            rec_err    <= 1'b1;
                            rec_toggle <= ~rec_toggle;
                            state      <= S_IDLE;
                        end else
                            cnt <= cnt + 1'b1;
                    default: ;                                // 0101 / 0110 waits
                endcase
            S_RDATA:
                if (cnt == 2'd0) begin
                    dlo <= lad_q;
                    cnt <= 2'd1;
                end else begin
                    rec_start  <= start;
                    rec_ct     <= ct;
                    rec_addr   <= addr;
                    rec_data   <= {lad_q, dlo};
                    rec_err    <= err;
                    rec_toggle <= ~rec_toggle;
                    state      <= S_IDLE;
                end
            default: ;                                       // idle: wait for LFRAME#
            endcase
        end
    end
endmodule

`default_nettype wire
