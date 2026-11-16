// Mailbox: the programmer protocol's console port and its link side. See
// docs/dev/programmer-protocol.md#console-port and docs/dev/link.md#mailbox.
//
// The link fetches bytes ahead of the ones it sends, so a byte from the console only
// counts as taken once it has been shifted out (f_done). Unsent fetches are returned when
// the transaction ends (f_idle).
`default_nettype none

module mailbox #(
    parameter [15:0] BASE_INIT = 16'h0B00
) (
    input  wire        clk,
    input  wire        wr,
    input  wire [15:0] addr,
    input  wire [7:0]  wdata,
    output reg  [7:0]  rdata = 8'd0,
    output reg         hit = 1'b0,
    input  wire        f_wr,
    input  wire        f_rd,
    output wire [7:0]  f_rdata,
    input  wire        f_done,
    input  wire        f_idle,
    output reg         irq = 1'b0,
    output wire        sts_rx,
    input  wire        lclk,
    input  wire        io_en,
    input  wire        io_addr_last,
    input  wire [15:0] io_addr_nxt,
    input  wire        io_rd,
    input  wire        io_wr,
    input  wire [7:0]  io_wdata,
    output reg         io_hit = 1'b0,
    output wire [7:0]  io_rdata
);
    reg        enable = 1'b1, ready = 1'b0, reset_req = 1'b0;
    reg [15:0] base = BASE_INIT;
    reg [7:0]  base_lo = BASE_INIT[7:0];
    reg        tx_flush_tog = 1'b0;
    reg [2:0]  crst_s = 3'd0;
    wire       crst_tog;
    wire       crst = crst_s[2] ^ crst_s[1];
    wire       rx_flush = crst || (wr && addr == 16'h0100 && wdata[1] && reset_req);

    wire [9:0] rx_level, tx_free;

    always @(posedge clk) begin
        crst_s <= {crst_s[1:0], crst_tog};
        if (wr) case (addr)
            16'h0100: begin
                enable <= wdata[0];
                ready  <= wdata[2];
                if (wdata[1] && reset_req) begin
                    reset_req    <= 1'b0;
                    tx_flush_tog <= ~tx_flush_tog;
                end
            end
            16'h0101: base_lo <= wdata;
            16'h0102: base    <= {wdata, base_lo};
            default: ;
        endcase
        if (crst) reset_req <= 1'b1;
    end

    always @(posedge clk) begin
        hit <= 1'b1;
        case (addr)
            16'h0100: rdata <= {5'd0, ready, reset_req, enable};
            16'h0101: rdata <= base[7:0];
            16'h0102: rdata <= base[15:8];
            16'h0103: rdata <= rx_level[7:0];
            16'h0104: rdata <= {6'd0, rx_level[9:8]};
            16'h0105: rdata <= tx_free[7:0];
            16'h0106: rdata <= {6'd0, tx_free[9:8]};
            default: begin rdata <= 8'h00; hit <= 1'b0; end
        endcase
    end

    reg       f_rd_q = 1'b0;
    reg [3:0] okq = 4'd0;
    reg [2:0] qn = 3'd0;
    wire      rx_rok;
    wire      q_pop   = f_done && qn != 3'd0;
    reg       rx_commit = 1'b0;
    reg [1:0] idle_d = 2'b11;               // rewind after the last commit has landed
    always @(posedge clk) begin
        f_rd_q    <= f_rd;
        rx_commit <= q_pop && okq[0];
        idle_d    <= {idle_d[0], f_idle};
        if (f_idle || rx_flush) begin
            qn <= 3'd0;
        end else case ({f_rd_q, q_pop})
            2'b10: begin okq[qn[1:0]] <= rx_rok; qn <= qn + 1'b1; end
            2'b01: begin okq <= okq >> 1; qn <= qn - 1'b1; end
            2'b11: begin okq <= okq >> 1; okq[qn[1:0] - 1'b1] <= rx_rok; end
            default: ;
        endcase
    end

    wire [7:0] rx_rdata;
    assign f_rdata = rx_rok ? rx_rdata : 8'h00;
    assign sts_rx  = (rx_level != 10'd0) || reset_req;

    reg rx_any_q = 1'b0, rreq_q = 1'b0;
    always @(posedge clk) begin
        rx_any_q <= rx_level != 10'd0;
        rreq_q   <= reset_req;
        irq      <= (!rx_any_q && rx_level != 10'd0) || (!rreq_q && reset_req);
    end

    reg [15:0] base_l = BASE_INIT;
    reg        en_l = 1'b0;
    reg [1:0]  ready_s = 2'b00, rreq_s = 2'b00;
    reg [2:0]  txf_s = 3'd0;
    reg        rst_pend = 1'b0;
    reg        crst_tog_r = 1'b0;
    reg [1:0]  off_q = 2'd0;
    reg [7:0]  port_q = 8'd0;
    reg        up_q = 1'b0;
    wire       up = ready_s[1] && !rreq_s[1] && !rst_pend;
    wire       tx_flush = (txf_s[2] ^ txf_s[1]) || (io_wr && io_hit && off_q == 2'd3 && io_wdata == 8'h01);
    wire [9:0] tx_level, rx_free;
    wire [7:0] tx_rdata;
    wire       tx_rok;
    assign crst_tog = crst_tog_r;

    function [7:0] count(input [9:0] n);
        count = (n > 10'd127) ? 8'h7F : {1'b0, n[6:0]};
    endfunction

    always @(posedge lclk) begin
        base_l  <= base;
        en_l    <= enable && io_en;
        ready_s <= {ready_s[0], ready};
        rreq_s  <= {rreq_s[0], reset_req};
        txf_s   <= {txf_s[1:0], tx_flush_tog};
        if (rreq_s[1]) rst_pend <= 1'b0;
        if (io_addr_last) begin
            io_hit <= en_l && (io_addr_nxt[15:2] == base_l[15:2]);
            off_q  <= io_addr_nxt[1:0];
            up_q   <= up;
            case (io_addr_nxt[1:0])
                2'd1:    port_q <= up ? count(tx_level) : 8'h00;
                2'd2:    port_q <= up ? count(rx_free)  : 8'h00;
                2'd3:    port_q <= up ? "O" : "R";
                default: port_q <= 8'h00;
            endcase
        end
        if (io_wr && io_hit && off_q == 2'd3 && io_wdata == 8'h01) begin
            rst_pend   <= 1'b1;
            crst_tog_r <= ~crst_tog_r;
        end
    end
    assign io_rdata = port_q | (tx_rdata & {8{off_q == 2'd0 && up_q && tx_rok}});

    mbx_fifo u_rx (
        .wclk(lclk), .wr(io_wr && io_hit && off_q == 2'd0 && up), .wdata(io_wdata),
        .wfree(rx_free),
        .rclk(clk), .rd(f_rd), .rdata(rx_rdata), .rok(rx_rok),
        .commit(rx_commit), .rewind(idle_d[1]), .flush(rx_flush), .rlevel(rx_level)
    );
    mbx_fifo #(.SPEC(0)) u_tx (
        .wclk(clk), .wr(f_wr), .wdata(wdata), .wfree(tx_free),
        .rclk(lclk), .rd(io_addr_last && io_addr_nxt[1:0] == 2'd0), .rdata(tx_rdata),
        .rok(tx_rok), .commit(io_rd && io_hit && off_q == 2'd0 && up_q && tx_rok),
        .rewind(io_addr_last), .flush(tx_flush), .rlevel(tx_level)
    );
endmodule

`default_nettype wire
