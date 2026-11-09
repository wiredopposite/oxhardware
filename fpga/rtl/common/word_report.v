// Prints a banner, or N 32-bit words as "II VVVVVVVV\r\n" lines, through a byte-wide
// UART transmitter interface. Words are sampled one at a time through a registered mux.
`default_nettype none

module word_report #(
    parameter integer N          = 16,          // words (<= 64)
    parameter integer BANNER_LEN = 8,
    parameter [8*BANNER_LEN-1:0] BANNER = "\015\nhello\015\n"
) (
    input  wire            clk,
    input  wire            rst,
    input  wire [N*32-1:0] words,               // word i at [32*i +: 32]
    input  wire            go_banner,           // pulse
    input  wire            go_report,           // pulse
    output reg  [7:0]      tx_data,
    output reg             tx_valid,
    input  wire            tx_ready
);
    localparam [2:0] P_IDLE = 3'd0, P_BANNER = 3'd1, P_LOAD = 3'd2, P_LATCH = 3'd3,
                     P_LINE = 3'd4;

    reg [2:0]  pstate;
    reg [6:0]  bidx;
    reg [5:0]  widx;
    reg [3:0]  pos;
    reg [31:0] wsh, word_q;
    reg [7:0]  banner_q, line_q;
    reg        hold, req_banner, req_report;

    function [7:0] hexc(input [3:0] n);
        hexc = (n < 4'd10) ? (8'h30 + n) : (8'h37 + n);
    endfunction

    reg [7:0] line_char;
    always @(*) begin
        case (pos)
            4'd0:    line_char = hexc({2'b00, widx[5:4]});
            4'd1:    line_char = hexc(widx[3:0]);
            4'd2:    line_char = 8'h20;
            4'd11:   line_char = 8'h0D;
            4'd12:   line_char = 8'h0A;
            default: line_char = hexc(wsh[31:28]);
        endcase
    end

    always @(posedge clk) begin
        word_q   <= words[32*widx +: 32];
        banner_q <= BANNER[8*(BANNER_LEN-1-bidx) +: 8];
        line_q   <= line_char;
    end

    always @(posedge clk) begin
        tx_valid <= 1'b0;
        if (go_banner) req_banner <= 1'b1;
        if (go_report) req_report <= 1'b1;
        if (rst) begin
            pstate <= P_IDLE; req_banner <= 1'b1; req_report <= 1'b0;
        end else begin
            case (pstate)
            P_IDLE:
                if (req_banner) begin
                    req_banner <= 1'b0; bidx <= 0; hold <= 1'b1; pstate <= P_BANNER;
                end else if (req_report) begin
                    req_report <= 1'b0; widx <= 0; pstate <= P_LOAD;
                end
            P_BANNER:
                if (hold) hold <= 1'b0;
                else if (tx_ready && !tx_valid) begin
                    tx_data <= banner_q; tx_valid <= 1'b1; hold <= 1'b1;
                    if (bidx == BANNER_LEN - 1) pstate <= P_IDLE;
                    bidx <= bidx + 1'b1;
                end
            P_LOAD:  pstate <= P_LATCH;          // word_q follows widx
            P_LATCH: begin
                wsh <= word_q; pos <= 0; hold <= 1'b1; pstate <= P_LINE;
            end
            P_LINE:
                if (hold) hold <= 1'b0;
                else if (tx_ready && !tx_valid) begin
                    tx_data <= line_q; tx_valid <= 1'b1; hold <= 1'b1;
                    if (pos >= 4'd3 && pos <= 4'd10) wsh <= {wsh[27:0], 4'h0};
                    if (pos == 4'd12) begin
                        if (widx == N - 1) pstate <= P_IDLE;
                        else begin widx <= widx + 1'b1; pstate <= P_LOAD; end
                    end
                    pos <= pos + 1'b1;
                end
            default: pstate <= P_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
