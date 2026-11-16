// 8N1 UART transmitter. The bit timer flags the end of a bit a clock early to keep the
// compare off the critical path.
`default_nettype none

module uart_tx #(
    parameter integer CLKS_PER_BIT = 417
) (
    input  wire       clk,
    input  wire       rst,
    input  wire [7:0] data,
    input  wire       valid,
    output wire       ready,
    output reg        tx
);
    localparam integer CW = $clog2(CLKS_PER_BIT);

    reg [8:0]    shreg;
    reg [3:0]    bitcnt;
    reg [CW-1:0] baud;
    reg          baud_done;
    reg          busy;

    assign ready = !busy;

    always @(posedge clk) begin
        if (rst) begin
            tx   <= 1'b1;
            busy <= 1'b0;
        end else if (!busy) begin
            if (valid) begin
                tx        <= 1'b0;
                shreg     <= {1'b1, data};
                bitcnt    <= 4'd0;
                baud      <= 0;
                baud_done <= 1'b0;
                busy      <= 1'b1;
            end
        end else if (!baud_done) begin
            baud      <= baud + 1'b1;
            baud_done <= (baud == CLKS_PER_BIT - 2);
        end else begin
            baud      <= 0;
            baud_done <= 1'b0;
            if (bitcnt == 4'd9) begin
                busy <= 1'b0;
            end else begin
                tx     <= shreg[0];
                shreg  <= {1'b1, shreg[8:1]};
                bitcnt <= bitcnt + 1'b1;
            end
        end
    end
endmodule

`default_nettype wire
