// LPC target: BIOS memory reads in FFxxxxxx answered from flash with long-wait SYNC,
// and I/O cycles offered to a port bus and claimed with a ready SYNC.
//
// Clock j is the LCLK during which the host drives nibble j:
//   mem read  j=0 START, 1 CYCTYPE, 2-9 A31..A0, 10-11 TAR, 12.. SYNC, D3:0, D7:4, TAR
//   I/O read  j=0 START, 1 CYCTYPE, 2-5 A15..A0, 6-7 TAR, 8 SYNC, 9-10 data, 11-12 TAR
//   I/O write j=0 START, 1 CYCTYPE, 2-5 A15..A0, 6-7 data, 8-9 TAR, 10 SYNC, 11-12 TAR
// Inputs and outputs are both registered, so the edge that starts clock e sees nibble
// e-2 and decides what is driven during clock e.
//
// START is found from LAD alone, since LFRAME# isn't on the 1.3-1.5 header and on 1.6 we
// drive it. Cycles we don't claim are followed to their end so that a 0000 SYNC or data
// nibble isn't taken for a START.
//
// LFRAME# abort (1.6): an external MOSFET pulls LFRAME# low from clock 3 (during CYCTYPE
// it would look like a new frame) until the second TAR clock, when LAD is 1111. From
// reset it stays low until the read of ..74 has been served.
`default_nettype none

module lpc_target #(
    parameter integer SYNC_TIMEOUT = 2048
) (
    input  wire        lclk,
    input  wire        lreset_n,
    input  wire        lframe_n,
    input  wire [3:0]  lad_in,
    output reg  [3:0]  lad_out,
    output reg         lad_oe,
    output wire        lframe_oe,
    input  wire        serve_en,
    input  wire        lframe_abort,
    input  wire        io_en,
    input  wire [23:0] rom_base,          // 64KB aligned
    input  wire [23:0] rom_mask,
    output reg         req,
    output reg  [23:0] req_addr,
    input  wire        ack,
    input  wire [7:0]  ack_data,
    output reg  [15:0] io_addr,
    output reg         io_write,
    output wire [15:0] io_addr_nxt,
    output wire        io_addr_last,
    input  wire        io_hit,
    output reg         io_rd,
    input  wire [7:0]  io_rdata,
    output reg         io_wr,
    output reg  [7:0]  io_wdata,
    output reg         io_done,
    output reg         io_took,
    output reg         stat_served,
    output reg         stat_timeout,
    output reg  [11:0] stat_wait,
    output wire        in_reset,
    output reg         stat_recover,
    output reg         stat_badaddr,      // memory read outside FFxxxxxx
    output reg  [23:0] stat_addr,
    output reg  [7:0]  stat_data,
    output wire        lframe_holding,
    output wire [3:0]  dbg_state
);
    localparam [3:0] S_IDLE = 4'd0, S_CT = 4'd1, S_ADDR = 4'd2, S_TAR = 4'd3,
                     S_SYNC = 4'd4, S_DLO = 4'd5, S_DHI = 4'd6, S_TAR1 = 4'd7,
                     S_REL = 4'd8,
                     S_SKIP1 = 4'd9, S_SKSYNC = 4'd10, S_SKIP2 = 4'd11,
                     S_IOADDR = 4'd12, S_IOTAR = 4'd13, S_IOWD = 4'd14, S_IOSY = 4'd15;

    // LRESET# counts after 16 low clocks. It starts out of reset so that a bitstream
    // loaded while the console runs doesn't take the boot hold.
    reg [3:0] lad_q;
    reg [1:0] lrst_s = 2'b11;
    reg [3:0] lrst_lo = 4'd0;
    reg       lrst_q = 1'b1;
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
    reg [3:0]  skip_n;
    reg [3:0]  skip_after;
    reg [1:0]  ones;
    reg [27:0] addr;
    reg        have;
    reg [7:0]  data;
    reg [11:0] wait_cnt;
    reg        lf_cyc;
    reg        addr_ok;
    reg        is_io;
    // Holding LFRAME# low through the first reads means the Xyclops never sees a START,
    // whatever the switching speed of the MOSFET.
    reg        hold = 1'b0;
    assign lframe_oe      = lf_cyc | hold;
    assign lframe_holding = hold;
    assign dbg_state      = state;
    assign io_addr_nxt    = {io_addr[11:0], lad_q};
    assign io_addr_last   = (state == S_IOADDR) && (nib == 4'd1);
    wire [19:0] win       = {addr[15:0], lad_q} & rom_mask[19:0];

    // Outputs are forced off in every listening state, so a corrupted state can't leave
    // the bus driven.
    wire listening = (state == S_IDLE) || (state == S_CT) || (state == S_SKIP1) ||
                     (state == S_SKSYNC) || (state == S_SKIP2) || (state == S_IOADDR) ||
                     (state == S_IOTAR) || (state == S_IOWD) || (state == S_IOSY);

    always @(posedge lclk) begin
        stat_served  <= 1'b0;
        stat_timeout <= 1'b0;
        stat_recover <= 1'b0;
        stat_badaddr <= 1'b0;
        io_rd        <= 1'b0;
        io_wr        <= 1'b0;
        io_done      <= 1'b0;
        if (listening && (lad_oe || (lf_cyc && state != S_CT)))
            stat_recover <= 1'b1;
        if (ack) begin
            have <= 1'b1;
            data <= ack_data;
            req  <= 1'b0;
        end

        if (!lrst_q) begin
            hold   <= lframe_abort && serve_en;
            state  <= S_IDLE;
            lad_oe <= 1'b0;
            lf_cyc <= 1'b0;
            req    <= 1'b0;
        end else begin
            if (listening) begin
                lad_oe <= 1'b0;
                if (state != S_CT) lf_cyc <= 1'b0;
            end
            case (state)
            S_IDLE:
                if (lad_q == 4'b0000) state <= S_CT;          // START
            S_CT: begin
                ones <= 2'd0;
                case (lad_q[3:1])
                    3'b010:                                   // memory read
                        if (serve_en) begin
                            state   <= S_ADDR;
                            nib     <= 4'd8;
                            addr_ok <= 1'b1;
                            if (lframe_abort) lf_cyc <= 1'b1;
                        end else begin
                            skip_n <= 4'd10; skip_after <= 4'd4; state <= S_SKIP1;
                        end
                    3'b000, 3'b001:                           // I/O read / write
                        if (io_en) begin
                            io_write <= lad_q[1];
                            nib      <= 4'd4;
                            state    <= S_IOADDR;
                        end else if (!lad_q[1]) begin
                            skip_n <= 4'd6; skip_after <= 4'd4; state <= S_SKIP1;
                        end else begin
                            skip_n <= 4'd8; skip_after <= 4'd2; state <= S_SKIP1;
                        end
                    3'b011: begin                             // memory write: A x8, D x2, TAR x2
                        skip_n <= 4'd12; skip_after <= 4'd2; state <= S_SKIP1;
                    end
                    default: state <= S_IDLE;                 // DMA / reserved: re-hunt
                endcase
            end
            S_SKIP1:
                if (skip_n == 4'd1) state <= S_SKSYNC;
                else                skip_n <= skip_n - 1'b1;
            S_SKSYNC:
                if (lad_q == 4'b0000 || lad_q == 4'b1010) begin
                    skip_n <= skip_after;
                    state  <= S_SKIP2;
                end else if (lad_q == 4'b1111) begin
                    if (ones == 2'd2) state <= S_IDLE;
                    ones <= ones + 1'b1;
                end
            S_SKIP2:
                if (skip_n == 4'd1) state <= S_IDLE;
                else                skip_n <= skip_n - 1'b1;
            S_IOADDR: begin
                io_addr <= {io_addr[11:0], lad_q};
                nib     <= nib - 1'b1;
                if (nib == 4'd1) begin
                    nib   <= 4'd2;
                    state <= io_write ? S_IOWD : S_IOTAR;
                end
            end
            S_IOTAR: begin
                io_done <= 1'b1;
                io_took <= io_hit;
                if (io_hit) begin
                    lad_oe  <= 1'b1;
                    lad_out <= 4'b0000;
                    data    <= io_rdata;
                    have    <= 1'b1;
                    io_rd   <= 1'b1;
                    is_io   <= 1'b1;
                    state   <= S_DLO;
                end else begin
                    skip_n <= 4'd1; skip_after <= 4'd4; state <= S_SKIP1;
                end
            end
            S_IOWD: begin
                if (nib == 4'd2) io_wdata[3:0] <= lad_q;
                else begin
                    io_wdata[7:4] <= lad_q;
                    state         <= S_IOSY;
                end
                nib <= nib - 1'b1;
            end
            S_IOSY: begin
                io_done <= 1'b1;
                io_took <= io_hit;
                if (io_hit) begin
                    lad_oe  <= 1'b1;
                    lad_out <= 4'b0000;
                    io_wr   <= 1'b1;
                    is_io   <= 1'b1;
                    state   <= S_TAR1;
                end else begin
                    skip_n <= 4'd1; skip_after <= 4'd2; state <= S_SKIP1;
                end
            end
            S_ADDR: begin
                addr <= {addr[23:0], lad_q};
                nib  <= nib - 1'b1;
                if ((nib == 4'd8 || nib == 4'd7) && lad_q != 4'hF)
                    addr_ok <= 1'b0;
                if (nib == 4'd1 && !addr_ok) begin
                    lf_cyc       <= 1'b0;
                    stat_badaddr <= 1'b1;
                    skip_n       <= 4'd2;
                    skip_after   <= 4'd4;
                    ones         <= 2'd0;
                    state        <= S_SKIP1;
                end else if (nib == 4'd1) begin
                    req      <= 1'b1;
                    req_addr <= {rom_base[23:16] + {4'd0, win[19:16]}, win[15:0]};
                    have     <= 1'b0;
                    is_io    <= 1'b0;
                    state    <= S_TAR;
                end
            end
            S_TAR: begin
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
                lad_out <= data[7:4];
                if (!is_io) begin
                    stat_served <= 1'b1;
                    stat_addr   <= req_addr;
                    stat_data   <= data;
                    stat_wait   <= wait_cnt;
                end
                state <= S_TAR1;
            end
            S_TAR1: begin
                lad_out <= 4'b1111;
                state   <= S_REL;
            end
            S_REL: begin
                if (!is_io && req_addr[7:0] == 8'h74) hold <= 1'b0;
                lad_oe <= 1'b0;
                lf_cyc <= 1'b0;
                state  <= S_IDLE;
            end
            default: begin
                lad_oe <= 1'b0;
                lf_cyc <= 1'b0;
                req    <= 1'b0;
                state  <= S_IDLE;
            end
            endcase
        end
    end
endmodule

`default_nettype wire
