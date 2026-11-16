// Dual-clock byte FIFO whose reads stay speculative until committed, for a reader that
// fetches ahead of what it consumes. rd fetches the next entry, commit marks the oldest
// fetch consumed, and rewind goes back to the last committed entry. Only committed reads
// free space. commit and rewind must not coincide.
//
// With SPEC = 0 the read side is a plain peek and pop: rd fetches the oldest entry,
// commit removes it, rewind does nothing.
//
// The levels lag the pointers by a few clocks.
`default_nettype none

module mbx_fifo #(
    parameter integer AW   = 9,
    parameter integer SPEC = 1
) (
    input  wire        wclk,
    input  wire        wr,
    input  wire [7:0]  wdata,
    output reg  [AW:0] wfree = (1 << AW),
    input  wire        rclk,
    input  wire        rd,
    output reg  [7:0]  rdata = 8'd0,
    output reg         rok = 1'b0,
    input  wire        commit,
    input  wire        rewind,
    input  wire        flush,
    output reg  [AW:0] rlevel = 0
);
    localparam [AW:0] DEPTH = (1 << AW);

    function [AW:0] g2b(input [AW:0] g);
        integer i;
        begin
            g2b[AW] = g[AW];
            for (i = AW - 1; i >= 0; i = i - 1) g2b[i] = g2b[i + 1] ^ g[i];
        end
    endfunction

    reg [7:0] mem [0:(1<<AW)-1];

    reg  [AW:0] cgray = 0;

    reg  [AW:0] wbin = 0, wgray = 0;
    reg  [AW:0] cg_s1 = 0, cg_s2 = 0, cbin_w = 0;
    reg         full = 1'b0;
    wire        wput   = wr && !full;
    wire [AW:0] wbin_n = wbin + wput;
    always @(posedge wclk) begin
        if (wput) mem[wbin[AW-1:0]] <= wdata;
        wbin   <= wbin_n;
        wgray  <= (wbin_n >> 1) ^ wbin_n;
        cg_s1  <= cgray;
        cg_s2  <= cg_s1;
        cbin_w <= g2b(cg_s2);
        full   <= (wbin_n[AW-1:0] == cbin_w[AW-1:0]) && (wbin_n[AW] != cbin_w[AW]);
        wfree  <= DEPTH - (wbin - cbin_w);
    end

    reg  [AW:0] wg_s1 = 0, wg_s2 = 0, wbin_r = 0;
    reg  [AW:0] spec = 0, cptr = 0;
    wire [AW:0] cptr_n = SPEC ? cptr + (commit && cptr != spec) : cptr + commit;
    wire [AW:0] rpos   = (!SPEC || rewind) ? cptr : spec;
    wire        empty  = (rpos == wbin_r);
    always @(posedge rclk) begin
        wg_s1  <= wgray;
        wg_s2  <= wg_s1;
        wbin_r <= g2b(wg_s2);
        if (rd) begin
            rdata <= mem[rpos[AW-1:0]];
            rok   <= !empty;
        end
        if (flush) begin
            spec <= wbin_r;
            cptr <= wbin_r;
        end else begin
            cptr <= cptr_n;
            spec <= rpos + (rd && !empty);
        end
        cgray  <= flush ? ((wbin_r >> 1) ^ wbin_r) : ((cptr_n >> 1) ^ cptr_n);
        rlevel <= wbin_r - cptr;
    end
endmodule

`default_nettype wire
