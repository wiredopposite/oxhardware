`default_nettype none

module bootctl_port (
    input  wire        clk,
    input  wire        en,
    input  wire [15:0] base,
    input  wire [63:0] vals,
    input  wire        io_addr_last,
    input  wire [15:0] io_addr_nxt,
    input  wire [15:0] io_addr,
    input  wire        io_wr,
    input  wire [7:0]  io_wdata,
    output reg         hit = 1'b0,
    output reg  [7:0]  rdata = 8'd0,
    output reg         wr_tog = 1'b0,
    output reg  [2:0]  wr_off = 3'd0,
    output reg  [7:0]  wr_data = 8'd0
);
    reg [63:0] vals_q = 64'd0;
    always @(posedge clk) begin
        vals_q <= vals;
        if (io_addr_last) begin
            hit   <= en && (io_addr_nxt[15:3] == base[15:3]);
            rdata <= vals_q[8*io_addr_nxt[2:0] +: 8];
        end
        if (io_wr && hit) begin
            wr_off  <= io_addr[2:0];
            wr_data <= io_wdata;
            wr_tog  <= ~wr_tog;
        end
    end
endmodule

`default_nettype wire
