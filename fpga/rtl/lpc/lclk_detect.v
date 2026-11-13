// Whether the console's LPC clock is running: the console is powered when it is, off when
// it isn't (LRESET# alone can't tell off from in reset). LCLK/16 toggles a flag in the
// LCLK domain; the system clock sees its edges. 16 edges each within 2 us of the last
// mean running, a 2 us gap means stopped, so noise on an undriven LCLK pin (it needs a
// pull-down) doesn't count.
`default_nettype none

module lclk_detect (
    input  wire lclk,
    input  wire clk,                    // system clock (24 MHz)
    output reg  running = 1'b0,
    output reg  changed = 1'b0          // one clk when running changes
);
    reg [3:0] div = 4'd0;
    always @(posedge lclk) div <= div + 1'b1;

    reg [2:0] s = 3'd0;
    reg [5:0] gap = 6'd0;               // clks since the last edge
    reg [3:0] good = 4'd0;              // edges seen in a row
    wire      edge_ = s[2] ^ s[1];
    localparam [5:0] GAP_MAX = 6'd48;   // 2 us

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
