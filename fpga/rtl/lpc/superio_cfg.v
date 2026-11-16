// SMSC-style Super I/O configuration space at 0x2E/0x2F, enough for the kernel to find
// the serial port and place it. 0x55 enters configuration mode and 0xAA leaves it.
// Logical devices 4 and 5 are serial ports, enabled by 0x30 bit 0 at the base in
// 0x60/0x61. Index port writes are always claimed so the enter key is seen.
`default_nettype none

module superio_cfg (
    input  wire        clk,
    input  wire        in_reset,
    input  wire        sel_idx,
    input  wire        sel_data,
    output reg         cfg = 1'b0,
    input  wire        wr,
    input  wire [7:0]  wdata,
    output reg  [7:0]  rdata,
    output wire        com_active,
    output wire [15:0] com_base
);
    reg [7:0] idx = 8'h00, ldn = 8'h00;
    reg       act4 = 1'b0, act5 = 1'b0;
    reg [7:0] b4h = 8'h00, b4l = 8'h00, b5h = 8'h00, b5l = 8'h00;

    wire is_idx  = sel_idx;
    wire is_data = sel_data;

    wire l4 = (ldn == 8'd4), l5 = (ldn == 8'd5);
    always @(*) begin
        if (is_idx) rdata = idx;
        else case (idx)
            8'h07: rdata = ldn;
            8'h26: rdata = 8'h2E;
            8'h27: rdata = 8'h00;
            8'h30: rdata = {7'd0, (l4 && act4) || (l5 && act5)};
            8'h60: rdata = l4 ? b4h : l5 ? b5h : 8'h00;
            8'h61: rdata = l4 ? b4l : l5 ? b5l : 8'h00;
            default: rdata = 8'h00;
        endcase
    end

    always @(posedge clk) begin
        if (in_reset) begin
            cfg <= 1'b0; idx <= 8'h00; ldn <= 8'h00;
            act4 <= 1'b0; act5 <= 1'b0;
            b4h <= 8'h00; b4l <= 8'h00; b5h <= 8'h00; b5l <= 8'h00;
        end else if (wr) begin
            if (!cfg) begin
                if (is_idx && wdata == 8'h55) cfg <= 1'b1;
            end else if (is_idx) begin
                if (wdata == 8'hAA) cfg <= 1'b0;
                else                idx <= wdata;
            end else if (is_data) case (idx)
                8'h07: ldn <= wdata;
                8'h30: begin if (l4) act4 <= wdata[0]; if (l5) act5 <= wdata[0]; end
                8'h60: begin if (l4) b4h <= wdata; if (l5) b5h <= wdata; end
                8'h61: begin if (l4) b4l <= wdata; if (l5) b5l <= wdata; end
                default: ;
            endcase
        end
    end

    wire v4 = act4 && ({b4h, b4l} != 16'h0000);
    wire v5 = act5 && ({b5h, b5l} != 16'h0000);
    assign com_active = v4 || v5;
    assign com_base   = v4 ? {b4h, b4l[7:3], 3'b000} : {b5h, b5l[7:3], 3'b000};
endmodule

`default_nettype wire
