// 16550 registers for the console side of the COM port. The divisor is kept but unused,
// since the UART runs at a fixed baud. LSR only shows THRE while the TX FIFO has room, so
// a polled sender waits instead of losing bytes. No interrupts.
//
// Reset clears the registers and drains RX, but TX keeps draining so the last output
// before a reboot isn't lost.
`default_nettype none

module com16550 (
    input  wire       clk,
    input  wire       in_reset,
    input  wire [2:0] reg_a,
    input  wire       sample,
    input  wire [2:0] sample_a,
    input  wire       rd,
    input  wire       wr,
    input  wire [7:0] wdata,
    output reg  [7:0] rdata = 8'h00,
    output reg        tx_push,
    output reg  [7:0] tx_data,
    input  wire       tx_full,
    input  wire       rx_valid,
    input  wire [7:0] rx_data,
    output wire       rx_pop
);
    reg [7:0] dll = 8'h01, dlm = 8'h00, lcr = 8'h03, scr = 8'h00;
    reg [3:0] ier = 4'h0;
    reg [4:0] mcr = 5'h00;
    reg       fifo_en = 1'b0;
    reg       flush = 1'b0;
    wire      dlab = lcr[7];

    reg rbr_ok = 1'b0;                    // RBR only pops if the sampled read had a byte
    always @(posedge clk) if (sample) begin
        rbr_ok <= sample_a == 3'd0 && !dlab && rx_valid && !flush;
        case (sample_a)
        3'd0:    rdata <= dlab ? dll : (rx_valid ? rx_data : 8'h00);
        3'd1:    rdata <= dlab ? dlm : {4'h0, ier};
        3'd2:    rdata <= fifo_en ? 8'hC1 : 8'h01;
        3'd3:    rdata <= lcr;
        3'd4:    rdata <= {3'b000, mcr};
        3'd5:    rdata <= {1'b0, !tx_full, !tx_full, 4'b0000, rx_valid && !flush};
        3'd6:    rdata <= 8'hB0;          // DCD | DSR | CTS
        default: rdata <= scr;
        endcase
    end

    reg pop_rd = 1'b0;
    initial tx_push = 1'b0;
    assign rx_pop = (flush && rx_valid) || pop_rd;

    always @(posedge clk) begin
        tx_push <= 1'b0;
        pop_rd  <= 1'b0;
        if (flush && !rx_valid) flush <= 1'b0;
        if (in_reset) begin
            dll <= 8'h01; dlm <= 8'h00; lcr <= 8'h03; scr <= 8'h00;
            ier <= 4'h0; mcr <= 5'h00; fifo_en <= 1'b0;
            flush <= 1'b1;
        end else begin
            if (rd && rbr_ok && !flush) pop_rd <= 1'b1;
            if (wr) case (reg_a)
                3'd0: if (dlab) dll <= wdata;
                      else begin tx_push <= 1'b1; tx_data <= wdata; end
                3'd1: if (dlab) dlm <= wdata; else ier <= wdata[3:0];
                3'd2: begin
                    fifo_en <= wdata[0];
                    if (wdata[1]) flush <= 1'b1;
                end
                3'd3: lcr <= wdata;
                3'd4: mcr <= wdata[4:0];
                3'd7: scr <= wdata;
                default: ;
            endcase
        end
    end
endmodule

`default_nettype wire
