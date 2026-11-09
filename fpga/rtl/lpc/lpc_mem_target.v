// LPC memory-read target (LCLK domain) — phase-1 BIOS serving.
//
// Claims every LPC memory read (START 0000, CYCTYPE 010x), maps the address into a
// flash window and returns the byte from a byte source (spi_stream) with SYNC long-wait
// until it arrives. Other cycles are followed to their end but never driven.
//
// Timing (clock j = LCLK during which the host drives nibble j):
//   j=0 START, 1 CYCTYPE, 2-9 A31..A0, 10-11 TAR (host), 12.. SYNC (us), D3:0, D7:4,
//   then one TAR clock driving 1111 and release on the second.
// Inputs are registered once (lad_q holds nibble j during clock j+1), and outputs are
// registered, so the FSM decides the nibble for clock t on the edge that starts it.
//
// START comes from LAD alone (LFRAME# is not on the header of 1.3-1.5, and on 1.6 we
// drive it ourselves): a 0000 nibble while hunting. Hunting only runs between cycles —
// cycles we don't claim (I/O, memory writes) are tracked through TAR/SYNC/data so a 0000
// SYNC or data nibble isn't taken for a START (as ox and OpenXenium do).
//
// LFRAME# abort (1.6): with lframe_abort=1, LFRAME# is pulled low (open-drain) from the
// address phase (clock 3, the first clock after CYCTYPE is known; asserting it during
// CYCTYPE would look like a new frame to the MCPX) until the second TAR clock, as EOS /
// OpenXenium do. The last clock with LFRAME# low has LAD = 1111, so it is not a START.
// The pad needs real drive (other designs use 12 mA or a MOSFET, no pull-up).
// Before that, LFRAME# is held low continuously from reset until the read of ..74 (boot
// hold, see 'hold'). Memory reads outside FFxxxxxx are not claimed (counted instead).
//
// Safety: if the byte doesn't arrive within SYNC_TIMEOUT clocks, SYNC error (1010) is
// driven and the cycle ends normally.
`default_nettype none

module lpc_mem_target #(
    parameter integer SYNC_TIMEOUT = 2048
) (
    input  wire        lclk,
    input  wire        lreset_n,
    input  wire        lframe_n,          // pin (input path; not used for decoding)
    input  wire [3:0]  lad_in,            // pin (input path)
    output reg  [3:0]  lad_out,
    output reg         lad_oe,
    output wire        lframe_oe,         // 1 = pull LFRAME# low (per-cycle | boot hold)
    // configuration (quasi-static)
    input  wire        serve_en,
    input  wire        lframe_abort,
    input  wire [23:0] rom_base,          // flash address of the image
    input  wire [23:0] rom_mask,          // image size - 1 (e.g. 0x3FFFF for 256 KB)
    // byte source
    output reg         req,
    output reg  [23:0] req_addr,
    input  wire        ack,
    input  wire [7:0]  ack_data,
    // statistics (LCLK domain)
    output reg         stat_served,       // pulse per served read
    output reg         stat_timeout,      // pulse per SYNC timeout
    output reg  [11:0] stat_wait,         // SYNC wait clocks of the last served read
    output wire        in_reset,          // filtered LRESET# (1 = in reset)
    output reg         stat_recover,      // pulse: illegal state or stray drive cleared
    output reg         stat_badaddr,      // pulse: memory read with A[31:24] != FF (ignored)
    output reg  [23:0] stat_addr,         // flash address of the last served read
    output reg  [7:0]  stat_data,         // byte returned by the last served read
    output wire        lframe_holding,    // boot hold active
    output wire [3:0]  dbg_state
);
    localparam [3:0] S_IDLE = 4'd0, S_CT = 4'd1, S_ADDR = 4'd2, S_TAR = 4'd3,
                     S_SYNC = 4'd4, S_DLO = 4'd5, S_DHI = 4'd6, S_TAR1 = 4'd7,
                     S_REL = 4'd8,
                     // following a cycle we don't claim
                     S_SKIP1 = 4'd9, S_SKSYNC = 4'd10, S_SKIP2 = 4'd11;

    // LRESET# is filtered: it must stay low for 16 consecutive clocks to count as a reset
    // (a real LRESET# lasts milliseconds; short glitches coupled from our own LAD/LFRAME#
    // switching were seen on the bench and must not abort a cycle).
    reg [3:0] lad_q;
    reg [1:0] lrst_s = 2'b00;
    reg [3:0] lrst_lo = 4'd0;
    reg       lrst_q = 1'b0;          // filtered: 1 = out of reset
    always @(posedge lclk) begin
        lad_q  <= lad_in;
        lrst_s <= {lrst_s[0], lreset_n};
        if (lrst_s[1]) begin
            lrst_lo <= 4'd0;
            lrst_q  <= 1'b1;
        end else if (lrst_lo == 4'd15)
            lrst_q  <= 1'b0;
        else
            lrst_lo <= lrst_lo + 1'b1;
    end
    assign in_reset = !lrst_q;

    reg [3:0]  state = S_IDLE;
    reg [3:0]  nib;
    reg [3:0]  skip_n;             // fixed nibbles to skip (S_SKIP1 / S_SKIP2)
    reg [3:0]  skip_after;         // nibbles after SYNC (data + TAR)
    reg [1:0]  ones;               // consecutive 1111 seen in SYNC (nobody answering)
    reg [27:0] addr;
    reg        have;
    reg [7:0]  data;
    reg [11:0] wait_cnt;
    reg        lf_cyc;                // per-cycle abort window
    reg        addr_ok;               // A[31:24] == FF so far
    // Boot hold (OpenXenium / genuine Xenium): LFRAME# held low continuously from reset
    // until the read of address ..74 has been served, then per-cycle aborts only. The
    // Xyclops never sees a valid START while LFRAME# stays low, and the first reads don't
    // depend on how fast the abort switch turns on and off.
    reg        hold = 1'b0;
    assign lframe_oe      = lf_cyc | hold;
    assign lframe_holding = hold;
    assign dbg_state      = state;

    // States in which we are listening, not driving. Fail-safe: in any of them both
    // outputs are forced off, so a corrupted state (e.g. an LCLK glitch) can't leave us
    // driving the bus; an illegal state code returns to hunting.
    wire listening = (state == S_IDLE) || (state == S_CT) || (state == S_SKIP1) ||
                     (state == S_SKSYNC) || (state == S_SKIP2);
    wire legal     = listening || (state == S_ADDR) || (state == S_TAR) ||
                     (state == S_SYNC) || (state == S_DLO) || (state == S_DHI) ||
                     (state == S_TAR1) || (state == S_REL);

    always @(posedge lclk) begin
        stat_served  <= 1'b0;
        stat_timeout <= 1'b0;
        stat_recover <= 1'b0;
        stat_badaddr <= 1'b0;
        if ((listening && (lad_oe || (lf_cyc && state != S_CT))) || !legal)
            stat_recover <= 1'b1;
        if (ack) begin
            have <= 1'b1;
            data <= ack_data;
            req  <= 1'b0;
        end

        if (!lrst_q || !serve_en) begin
            hold      <= lframe_abort && serve_en;
            state     <= S_IDLE;
            lad_oe    <= 1'b0;
            lf_cyc <= 1'b0;
            req       <= 1'b0;
        end else begin
            if (listening) begin
                lad_oe <= 1'b0;
                if (state != S_CT) lf_cyc <= 1'b0;   // S_CT may assert it below
            end
            case (state)
            // ------------------------------------------------ hunting / host phases
            S_IDLE:
                if (lad_q == 4'b0000) state <= S_CT;          // START
            S_CT: begin                                       // CYCTYPE (j=1) at edge 3
                ones <= 2'd0;
                case (lad_q[3:1])
                    3'b010: begin                             // memory read: claim
                        state   <= S_ADDR;
                        nib     <= 4'd8;
                        addr_ok <= 1'b1;
                        if (lframe_abort) lf_cyc <= 1'b1;  // low from clock 3
                    end
                    3'b000: begin                             // I/O read: A x4, TAR x2
                        skip_n <= 4'd6; skip_after <= 4'd4; state <= S_SKIP1;
                    end
                    3'b001: begin                             // I/O write: A x4, D x2, TAR x2
                        skip_n <= 4'd8; skip_after <= 4'd2; state <= S_SKIP1;
                    end
                    3'b011: begin                             // memory write: A x8, D x2, TAR x2
                        skip_n <= 4'd12; skip_after <= 4'd2; state <= S_SKIP1;
                    end
                    default: state <= S_IDLE;                 // DMA / reserved: re-hunt
                endcase
            end
            S_SKIP1:                                          // fixed part before SYNC
                if (skip_n == 4'd1) state <= S_SKSYNC;
                else                skip_n <= skip_n - 1'b1;
            S_SKSYNC:                                         // someone else's SYNC
                if (lad_q == 4'b0000 || lad_q == 4'b1010) begin
                    skip_n <= skip_after;
                    state  <= S_SKIP2;
                end else if (lad_q == 4'b1111) begin
                    if (ones == 2'd2) state <= S_IDLE;        // nobody answered
                    ones <= ones + 1'b1;
                end
            S_SKIP2:                                          // data / TAR after SYNC
                if (skip_n == 4'd1) state <= S_IDLE;
                else                skip_n <= skip_n - 1'b1;
            S_ADDR: begin                           // A31..A0 processed at edges 4..11
                addr <= {addr[23:0], lad_q};
                nib  <= nib - 1'b1;
                if ((nib == 4'd8 || nib == 4'd7) && lad_q != 4'hF)
                    addr_ok <= 1'b0;                // not a BIOS-window read
                if (nib == 4'd1 && !addr_ok) begin  // not ours: release, follow to the end
                    lf_cyc       <= 1'b0;
                    stat_badaddr <= 1'b1;
                    skip_n       <= 4'd2;           // TAR x2, then someone's SYNC
                    skip_after   <= 4'd4;           // data x2, TAR x2
                    ones         <= 2'd0;
                    state        <= S_SKIP1;
                end else if (nib == 4'd1) begin     // edge 11: full address known
                    req      <= 1'b1;
                    req_addr <= rom_base | ({addr[19:0], lad_q} & rom_mask);
                    have     <= 1'b0;
                    state    <= S_TAR;
                end
            end
            S_TAR: begin                            // edge 12: start driving SYNC
                lad_oe   <= 1'b1;
                wait_cnt <= 0;
                if (have || ack) begin
                    lad_out <= 4'b0000;
                    state   <= S_DLO;
                end else begin
                    lad_out <= 4'b0110;             // long wait
                    state   <= S_SYNC;
                end
            end
            // ------------------------------------------------ our phases (we drive)
            S_SYNC:
                if (have || ack) begin
                    lad_out <= 4'b0000;
                    state   <= S_DLO;
                end else if (wait_cnt == SYNC_TIMEOUT - 1) begin
                    lad_out      <= 4'b1010;        // SYNC error, no data
                    req          <= 1'b0;
                    stat_timeout <= 1'b1;
                    state        <= S_TAR1;
                end else
                    wait_cnt <= wait_cnt + 1'b1;
            S_DLO: begin
                lad_out <= have ? data[3:0] : ack_data[3:0];
                if (!have) data <= ack_data;
                state   <= S_DHI;
            end
            S_DHI: begin
                lad_out     <= data[7:4];
                stat_served <= 1'b1;
                stat_addr   <= req_addr;
                stat_data   <= data;
                stat_wait   <= wait_cnt;
                state       <= S_TAR1;
            end
            S_TAR1: begin                           // drive 1111, LFRAME# still low
                lad_out <= 4'b1111;
                state   <= S_REL;
            end
            S_REL: begin                            // second TAR clock: release all
                if (req_addr[7:0] == 8'h74) hold <= 1'b0;
                lad_oe    <= 1'b0;
                lf_cyc <= 1'b0;
                state     <= S_IDLE;
            end
            default: begin                          // illegal state: release, re-hunt
                lad_oe    <= 1'b0;
                lf_cyc <= 1'b0;
                req       <= 1'b0;
                state     <= S_IDLE;
            end
            endcase
        end
    end
endmodule

`default_nettype wire
