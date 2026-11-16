// 8N1 UART receiver. Frames with a bad stop bit are dropped. The bit timer flags sample
// points a clock early to keep the compare off the critical path.
`default_nettype none

module uart_rx #(
    parameter integer CLKS_PER_BIT = 417
) (
    input  wire       clk,
    input  wire       rst,
    input  wire       rx,
    output reg  [7:0] data,
    output reg        valid
);
    localparam integer CW = $clog2(CLKS_PER_BIT);
    localparam [CW-1:0] HALF_LAST = CLKS_PER_BIT / 2 - 2;
    localparam [CW-1:0] FULL_LAST = CLKS_PER_BIT - 2;
    localparam [1:0] S_IDLE = 2'd0, S_START = 2'd1, S_DATA = 2'd2, S_STOP = 2'd3;

    reg [1:0]    sync;
    initial valid = 1'b0;
    wire         rx_s = sync[1];
    reg [1:0]    state;
    reg [CW-1:0] cnt;
    reg          cnt_done;
    reg [2:0]    bitcnt;
    reg [7:0]    shreg;

    always @(posedge clk) begin
        sync  <= {sync[0], rx};
        valid <= 1'b0;
        if (rst) begin
            sync  <= 2'b11;
            state <= S_IDLE;
        end else if (state == S_IDLE) begin
            if (!rx_s) begin
                cnt      <= 0;
                cnt_done <= 1'b0;
                state    <= S_START;
            end
        end else if (!cnt_done) begin
            cnt      <= cnt + 1'b1;
            cnt_done <= (cnt == ((state == S_START) ? HALF_LAST : FULL_LAST));
        end else begin
            cnt      <= 0;
            cnt_done <= 1'b0;
            case (state)
            S_START:
                if (!rx_s) begin
                    bitcnt <= 3'd0;
                    state  <= S_DATA;
                end else
                    state <= S_IDLE;
            S_DATA: begin
                shreg  <= {rx_s, shreg[7:1]};
                bitcnt <= bitcnt + 1'b1;
                if (bitcnt == 3'd7) state <= S_STOP;
            end
            default: begin
                if (rx_s) begin
                    data  <= shreg;
                    valid <= 1'b1;
                end
                state <= S_IDLE;
            end
            endcase
        end
    end
endmodule

`default_nettype wire
