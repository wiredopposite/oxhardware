// Whether LCLK is running, which tells a console that is off from one in reset. 16 edges
// of LCLK/16, each within 2us of the last, count as running, so noise on an undriven
// pin doesn't.
`default_nettype none

module lclk_detect (
    input  wire lclk,
    input  wire clk,
    output reg  running = 1'b0,
    output reg  changed = 1'b0
);
    reg [3:0] div = 4'd0;
    always @(posedge lclk) div <= div + 1'b1;

    reg [2:0] s = 3'd0;
    reg [5:0] gap = 6'd0;
    reg [3:0] good = 4'd0;
    wire      edge_ = s[2] ^ s[1];
    localparam [5:0] GAP_MAX = 6'd48;   // 2us

    always @(posedge clk) begin
        s       <= {s[1:0], div[3]};
        changed <= 1'b0;
        if (edge_) begin
            gap <= 6'd0;
            if (good != 4'd15) begin
                good <= good + 1'b1;
            end else if (!running) begin
                running <= 1'b1;
                changed <= 1'b1;
            end
        end else if (gap != GAP_MAX) begin
            gap <= gap + 1'b1;
        end else begin
            good <= 4'd0;
            if (running) begin
                running <= 1'b0;
                changed <= 1'b1;
            end
        end
    end
endmodule

`default_nettype wire
