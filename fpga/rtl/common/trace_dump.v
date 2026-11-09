// Prints a trace_buf as 512 lines "III VVVV\r\n" (index from oldest, sample) through a
// byte-wide UART transmitter interface. 'go' starts a dump; 'busy' is high until done.
`default_nettype none

module trace_dump (
    input  wire        clk,
    input  wire        rst,
    input  wire        go,
    input  wire [8:0]  oldest,           // trace_buf.wp (frozen)
    output reg  [8:0]  raddr,
    input  wire [15:0] rdata,
    output reg         busy,
    output reg  [7:0]  tx_data,
    output reg         tx_valid,
    input  wire        tx_ready
);
    reg [9:0]  idx;                      // 0..511, 512 = done
    reg [3:0]  pos;                      // 0..9
    reg [15:0] val;
    reg [1:0]  ph;                       // 0 = load, 1 = wait, 2 = latch, 3 = print
    reg [7:0]  ch_q;
    reg        hold;

    function [7:0] hexc(input [3:0] n);
        hexc = (n < 4'd10) ? (8'h30 + n) : (8'h37 + n);
    endfunction

    reg [7:0] ch;
    always @(*) begin
        case (pos)
            4'd0: ch = hexc({3'b000, idx[8]});
            4'd1: ch = hexc(idx[7:4]);
            4'd2: ch = hexc(idx[3:0]);
            4'd3: ch = 8'h20;
            4'd4: ch = hexc(val[15:12]);
            4'd5: ch = hexc(val[11:8]);
            4'd6: ch = hexc(val[7:4]);
            4'd7: ch = hexc(val[3:0]);
            4'd8: ch = 8'h0D;
            default: ch = 8'h0A;
        endcase
    end
    always @(posedge clk) ch_q <= ch;

    always @(posedge clk) begin
        tx_valid <= 1'b0;
        if (rst) begin
            busy <= 1'b0;
        end else if (!busy) begin
            if (go) begin busy <= 1'b1; idx <= 0; ph <= 2'd0; end
        end else begin
            case (ph)
            2'd0: begin raddr <= oldest + idx[8:0]; ph <= 2'd1; end
            2'd1: ph <= 2'd2;
            2'd2: begin val <= rdata; pos <= 0; hold <= 1'b1; ph <= 2'd3; end
            default:
                if (hold) hold <= 1'b0;
                else if (tx_ready && !tx_valid) begin
                    tx_data  <= ch_q;
                    tx_valid <= 1'b1;
                    hold     <= 1'b1;
                    pos      <= pos + 1'b1;
                    if (pos == 4'd9) begin
                        if (idx == 10'd511) busy <= 1'b0;
                        idx <= idx + 1'b1;
                        ph  <= 2'd0;
                    end
                end
            endcase
        end
    end
endmodule

`default_nettype wire
