// Passive LPC cycle-header sniffer (LCLK domain). Never drives anything.
//
// For every cycle it decodes START, CYCTYPE/DIR (or IDSEL for firmware-hub cycles) and
// the address, then publishes {start, ct, addr} with a toggle (rec_toggle) for a
// slower/faster clock domain to pick up. Cycles are >= ~10 LCLKs apart, so a single
// holding register is enough for a 48 MHz consumer.
//
// LPC header (Intel LPC 1.1): LFRAME# low with LAD = START; first clock after LFRAME#
// rises carries CYCTYPE/DIR; then 4 (I/O) or 8 (memory) address nibbles, MSN first.
// Firmware-hub START 1101/1110: IDSEL then 7 address nibbles.
`default_nettype none

module lpc_sniff (
    input  wire        lclk,
    input  wire        lreset_n,
    input  wire        lframe_n,
    input  wire [3:0]  lad,
    output reg  [3:0]  rec_start,
    output reg  [3:0]  rec_ct,
    output reg  [31:0] rec_addr,
    output reg         rec_toggle = 1'b0,
    output wire        lclk_div16       // toggles every 16 LCLKs (activity / frequency)
);
    localparam [1:0] S_IDLE = 2'd0, S_CT = 2'd1, S_ADDR = 2'd2;

    // Register inputs once on LCLK rising (host drives on rising edge, data valid
    // well before the next one).
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

    reg [1:0]  state = S_IDLE;
    reg [3:0]  start, ct, nib_left;
    reg [31:0] addr;

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
                    rec_toggle <= ~rec_toggle;
                    state      <= S_IDLE;
                end
            end
            S_ADDR: begin
                addr     <= {addr[27:0], lad_q};
                nib_left <= nib_left - 1'b1;
                if (nib_left == 4'd1) begin
                    rec_start  <= start;
                    rec_ct     <= ct;
                    rec_addr   <= {addr[27:0], lad_q};
                    rec_toggle <= ~rec_toggle;
                    state      <= S_IDLE;
                end
            end
            default: ;                                       // idle: wait for LFRAME#
            endcase
        end
    end
endmodule

`default_nettype wire
