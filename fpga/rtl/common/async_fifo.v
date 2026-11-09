// Dual-clock FIFO (one EBR at the default size) with gray-coded pointers and a
// first-word-fall-through output: rdata is valid whenever rvalid is high, and rd pops it.
// wr is ignored while full.
`default_nettype none

module async_fifo #(
    parameter integer AW = 9                  // 2^AW entries
) (
    input  wire       wclk,
    input  wire       wr,
    input  wire [7:0] wdata,
    output reg        full = 1'b0,
    input  wire       rclk,
    input  wire       rd,
    output reg  [7:0] rdata,
    output reg        rvalid = 1'b0
);
    reg [7:0] mem [0:(1<<AW)-1];

    reg  [AW:0] wbin = 0, wgray = 0, rg_s1 = 0, rg_s2 = 0;
    reg  [AW:0] rbin = 0, rgray = 0, wg_s1 = 0, wg_s2 = 0;

    // write side
    wire        wput   = wr && !full;
    wire [AW:0] wbin_n = wbin + wput;
    wire [AW:0] wgry_n = (wbin_n >> 1) ^ wbin_n;
    always @(posedge wclk) begin
        if (wput) mem[wbin[AW-1:0]] <= wdata;
        wbin  <= wbin_n;
        wgray <= wgry_n;
        rg_s1 <= rgray;
        rg_s2 <= rg_s1;
        full  <= (wgry_n == {~rg_s2[AW:AW-1], rg_s2[AW-2:0]});
    end

    // read side
    wire        empty  = (rgray == wg_s2);
    wire        rload  = !empty && (!rvalid || rd);
    wire [AW:0] rbin_n = rbin + 1'b1;
    always @(posedge rclk) begin
        wg_s1 <= wgray;
        wg_s2 <= wg_s1;
        if (rload) begin
            rdata <= mem[rbin[AW-1:0]];
            rbin  <= rbin_n;
            rgray <= (rbin_n >> 1) ^ rbin_n;
        end
        if (rload)   rvalid <= 1'b1;
        else if (rd) rvalid <= 1'b0;
    end
endmodule

`default_nettype wire
