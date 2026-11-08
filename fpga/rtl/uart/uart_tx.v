// 8N1 UART transmitter.
// Accepts a byte when valid && ready; tx idles high.
// The bit timer counts up from zero (so its carry chain packs cleanly) and flags the
// end of each bit period one cycle early (baud_done) to keep the compare off the
// critical path at 48 MHz.
`default_nettype none

module uart_tx #(
    parameter integer CLKS_PER_BIT = 417   // 48 MHz / 115200
) (
    input  wire       clk,
    input  wire       rst,
    input  wire [7:0] data,
    input  wire       valid,
    output wire       ready,
    output reg        tx
);
    localparam integer CW = $clog2(CLKS_PER_BIT);

    reg [8:0]    shreg;    // remaining bits: data[7:0], stop
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
                tx        <= 1'b0;           // start bit
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
            if (bitcnt == 4'd9) begin        // stop bit period finished
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
