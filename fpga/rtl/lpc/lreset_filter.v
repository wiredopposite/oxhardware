// LRESET# follows the pin once it has held a new level for STABLE clocks. Runs on a
// free-running clock, since LCLK can stop in reset.
`default_nettype none

module lreset_filter #(
    parameter integer STABLE = 32
) (
    input  wire clk,
    input  wire lreset_n_pin,
    output reg  lreset_n = 1'b0
);
    reg [1:0] s = 2'b00;
    reg [$clog2(STABLE)-1:0] cnt = 0;
    always @(posedge clk) begin
        s <= {s[0], lreset_n_pin};
        if (s[1] == lreset_n) begin
            cnt <= 0;
        end else if (cnt == STABLE - 1) begin
            lreset_n <= s[1];
            cnt      <= 0;
        end else begin
            cnt <= cnt + 1'b1;
        end
    end
endmodule

`default_nettype wire
