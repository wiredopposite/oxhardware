// 16K x 64-bit single-port RAM from the four iCE40UP5K SPRAM blocks (16K x 16 each).
// Synchronous: write when we=1; otherwise rdata = mem[addr] one clock later.
// Simulation (no SYNTHESIS define) uses a behavioural array.
`default_nettype none

module spram64 (
    input  wire        clk,
    input  wire [13:0] addr,
    input  wire        we,
    input  wire [63:0] wdata,
    output wire [63:0] rdata
);
`ifdef SYNTHESIS
    genvar i;
    generate
        for (i = 0; i < 4; i = i + 1) begin : g_bank
            SB_SPRAM256KA u_spram (
                .ADDRESS   (addr),
                .DATAIN    (wdata[16*i +: 16]),
                .MASKWREN  (4'b1111),
                .WREN      (we),
                .CHIPSELECT(1'b1),
                .CLOCK     (clk),
                .STANDBY   (1'b0),
                .SLEEP     (1'b0),
                .POWEROFF  (1'b1),
                .DATAOUT   (rdata[16*i +: 16])
            );
        end
    endgenerate
`else
    reg [63:0] mem [0:16383];
    reg [63:0] q;
    always @(posedge clk) begin
        if (we) mem[addr] <= wdata;
        else    q <= mem[addr];
    end
    assign rdata = q;
`endif
endmodule

`default_nettype wire
