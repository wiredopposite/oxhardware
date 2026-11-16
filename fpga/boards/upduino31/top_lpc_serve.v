// lpc_serve: bench design that serves a 256KB image at flash 0x40000 with the 1.6
// LFRAME# abort. Enter on the UART prints a report for tools/lpc_serve_report.py.
`default_nettype none

module top_lpc_serve (
    input  wire       clk12,
    output wire       led_red,
    output wire       led_green,
    output wire       led_blue,
    output wire       uart_tx,
    input  wire       uart_rx,
    input  wire       lpc_clk,
    input  wire       lpc_reset_n,
    output wire [3:0] lframe_gate,
    inout  wire [3:0] lpc_lad,
    input  wire       v33_sense,
    input  wire       lframe_sense,
    output wire       flash_csn,
    output wire       flash_sck,
    output wire       flash_mosi,
    input  wire       flash_miso
);
    localparam [23:0] ROM_BASE = 24'h040000;
    localparam [23:0] ROM_MASK = 24'h03FFFF;

    wire clk, pll_locked, rst, clk_lf;
    upduino31_clocks #(.DIVQ(3'b101)) u_clk (
        .clk12(clk12), .clk_sys(clk), .pll_locked(pll_locked), .rst(rst), .clk_lf(clk_lf)
    );

    wire [3:0]  lad_out;
    wire        lad_oe, lframe_oe, holding;
    wire        req, ack, restarted;
    wire [23:0] req_addr, st_addr;
    wire [7:0]  ack_data, st_data;
    wire        st_served, st_timeout, st_recover, st_badaddr, tgt_in_reset;
    wire [11:0] st_wait;
    wire [3:0]  dbg_state;
    wire        t_ack;
    wire [7:0]  t_ack_data;
    wire        lrst_hi_l = !tgt_in_reset;

    assign lpc_lad     = lad_oe ? lad_out : 4'bzzzz;
    // 'g' forces the gate on, for checking the MOSFET with the console off.
    reg force_gate = 1'b0;
    assign lframe_gate = {4{lframe_oe | force_gate}};

    lpc_target u_tgt (
        .lclk(lpc_clk), .lreset_n(lpc_reset_n), .lframe_n(lframe_sense), .lad_in(lpc_lad),
        .lad_out(lad_out), .lad_oe(lad_oe), .lframe_oe(lframe_oe),
        .serve_en(1'b1), .lframe_abort(1'b1), .io_en(1'b0),
        .rom_base(ROM_BASE), .rom_mask(ROM_MASK),
        .req(req), .req_addr(req_addr), .ack(t_ack), .ack_data(t_ack_data),
        .io_addr(), .io_write(), .io_addr_nxt(), .io_addr_last(), .io_hit(1'b0), .io_rd(), .io_rdata(8'h00), .io_wr(), .io_wdata(), .io_done(), .io_took(),
        .stat_served(st_served), .stat_timeout(st_timeout), .stat_wait(st_wait),
        .in_reset(tgt_in_reset), .stat_recover(st_recover), .stat_badaddr(st_badaddr),
        .stat_addr(st_addr), .stat_data(st_data), .lframe_holding(holding),
        .dbg_state(dbg_state)
    );

    wire        tr_frozen;
    wire [8:0]  tr_wp, tr_raddr;
    wire [15:0] tr_rdata;
    trace_buf #(.POST(64)) u_trace (
        .sclk(lpc_clk),
        .sample({3'b000, holding, dbg_state, lpc_reset_n, lframe_oe, lad_oe, lframe_sense, lpc_lad}),
        .trigger(st_badaddr), .frozen(tr_frozen), .wp(tr_wp),
        .rclk(clk), .raddr(tr_raddr), .rdata(tr_rdata)
    );

    // The first 4KB of the image is preloaded into EBR from build/rom_head.hex and
    // answered without the flash.
    reg  [7:0]  head [0:4095];
    initial $readmemh("build/rom_head.hex", head);
    reg         in_head = 1'b0;
    reg  [7:0]  head_q;
    reg         head_ack = 1'b0, req_d = 1'b0;
    always @(posedge lpc_clk) begin
        in_head  <= (req_addr[17:12] == 6'd0);
        head_q   <= head[req_addr[11:0]];
        req_d    <= req;
        head_ack <= req && req_d && in_head && !head_ack;
    end
    assign      t_ack      = head_ack | ack;
    assign      t_ack_data = head_ack ? head_q : ack_data;

    spi_stream u_flash (
        .clk(lpc_clk), .rst(!lrst_hi_l),
        .park(1'b0), .parked(),
        .req(req && !in_head), .req_addr(req_addr), .ack(ack), .ack_data(ack_data),
        .restarted(restarted),
        .spi_cs_n(flash_csn), .spi_sck(flash_sck), .spi_mosi(flash_mosi), .spi_miso(flash_miso)
    );

    reg [31:0] n_served = 0, n_restart = 0, n_timeout = 0, n_lrst = 0, lclk_ticks = 0;
    reg [31:0] n_lf_pull = 0, n_lf_hi_early = 0, n_lf_hi_late = 0, n_rel_lo1 = 0, n_rel_lo2 = 0;
    reg [31:0] n_lad_drv = 0, n_lad_lost = 0, n_lrst_glitch = 0, n_recover = 0;
    reg [31:0] n_badaddr = 0, n_hold_rel = 0, n_attempt = 0;
    reg [11:0] last_wait = 0, max_wait = 0;
    reg [15:0] lclk_div = 0;
    reg [1:0]  lrst_raw = 2'b00;
    reg        lrst_d = 1'b0, hold_d = 1'b0, lf_d = 1'b0, lf_dd = 1'b0;
    reg [2:0]  lf_age = 0;
    reg        serve_tog = 1'b0;

    reg [31:0] first8 [0:7];
    reg [31:0] ring   [0:7];
    reg [31:0] last8  [0:7];
    reg [2:0]  ring_wp = 0;
    reg [3:0]  snap_i = 4'd8;
    integer k;

    always @(posedge lpc_clk) begin
        lclk_div <= lclk_div + 1'b1;
        if (lclk_div == 16'hFFFF) lclk_ticks <= lclk_ticks + 1'b1;

        lrst_raw <= {lrst_raw[0], lpc_reset_n};
        if (lrst_raw == 2'b10 && lrst_hi_l) n_lrst_glitch <= n_lrst_glitch + 1'b1;
        lrst_d <= lrst_hi_l;
        if (lrst_hi_l && !lrst_d) begin
            n_lrst    <= n_lrst + 1'b1;
            n_attempt <= 0;
        end

        lf_d  <= lframe_oe;
        lf_dd <= lf_d;
        if (lframe_oe) begin
            n_lf_pull <= n_lf_pull + 1'b1;
            if (lf_age != 3'd7) lf_age <= lf_age + 1'b1;
            if (lframe_sense) begin
                if (lf_age < 3'd4) n_lf_hi_early <= n_lf_hi_early + 1'b1;
                else               n_lf_hi_late  <= n_lf_hi_late + 1'b1;
            end
        end else
            lf_age <= 0;
        if (!lframe_oe &&  lf_d && !lframe_sense) n_rel_lo1 <= n_rel_lo1 + 1'b1;
        if (!lframe_oe && !lf_d && lf_dd && !lframe_sense) n_rel_lo2 <= n_rel_lo2 + 1'b1;
        hold_d <= holding;
        if (hold_d && !holding && lrst_hi_l) n_hold_rel <= n_hold_rel + 1'b1;

        if (lad_oe) begin
            n_lad_drv <= n_lad_drv + 1'b1;
            if (lpc_lad != lad_out) n_lad_lost <= n_lad_lost + 1'b1;
        end

        if (st_served) begin
            n_served  <= n_served + 1'b1;
            n_attempt <= n_attempt + 1'b1;
            last_wait <= st_wait;
            if (st_wait > max_wait) max_wait <= st_wait;
            serve_tog <= ~serve_tog;
            if (n_attempt < 8) first8[n_attempt[2:0]] <= {st_data, st_addr};
            ring[ring_wp] <= {st_data, st_addr};
            ring_wp       <= ring_wp + 1'b1;
        end
        if (st_timeout) n_timeout <= n_timeout + 1'b1;
        if (restarted)  n_restart <= n_restart + 1'b1;
        if (st_recover) n_recover <= n_recover + 1'b1;
        if (st_badaddr) n_badaddr <= n_badaddr + 1'b1;

        if (!lrst_hi_l && lrst_d) snap_i <= 4'd0;
        else if (!snap_i[3]) begin
            last8[snap_i[2:0]] <= ring[ring_wp + snap_i[2:0]];
            snap_i <= snap_i + 1'b1;
        end
    end

    wire [7:0] rx_data;
    wire       rx_valid;
    uart_rx #(.CLKS_PER_BIT(208)) u_rx (
        .clk(clk), .rst(rst), .rx(uart_rx), .data(rx_data), .valid(rx_valid)
    );
    wire [7:0] tx_data, rep_data, dmp_data;
    wire       tx_valid, tx_ready, rep_valid, dmp_valid, dmp_busy;
    assign tx_data  = dmp_busy ? dmp_data  : rep_data;
    assign tx_valid = dmp_busy ? dmp_valid : rep_valid;
    uart_tx #(.CLKS_PER_BIT(208)) u_tx (
        .clk(clk), .rst(rst), .data(tx_data), .valid(tx_valid), .ready(tx_ready), .tx(uart_tx)
    );

    wire [32*37-1:0] words = {
        {30'd0, force_gate, tr_frozen},
        last8[7], last8[6], last8[5], last8[4], last8[3], last8[2], last8[1], last8[0],
        first8[7], first8[6], first8[5], first8[4], first8[3], first8[2], first8[1], first8[0],
        n_attempt, n_hold_rel, n_badaddr, n_recover, n_lrst_glitch, n_lad_lost, n_lad_drv,
        n_rel_lo2, n_rel_lo1, n_lf_hi_late, n_lf_hi_early, n_lf_pull,
        lclk_ticks, n_lrst, {20'd0, max_wait}, {20'd0, last_wait},
        n_timeout, n_restart, n_served, 32'h4C53_0002 };

    word_report #(
        .N(37), .BANNER_LEN(53),
        .BANNER("\015\nlpc_serve v2: 256K @ flash 0x40000, LFRAME# abort\015\n")
    ) u_rep (
        .clk(clk), .rst(rst), .words(words),
        .go_banner(1'b0), .go_report(rx_valid && rx_data == 8'h0D && !dmp_busy),
        .tx_data(rep_data), .tx_valid(rep_valid), .tx_ready(tx_ready && !dmp_busy)
    );

    always @(posedge clk) if (rx_valid && rx_data == 8'h67) force_gate <= ~force_gate;

    reg [1:0] frz_s;
    always @(posedge clk) frz_s <= {frz_s[0], tr_frozen};
    trace_dump u_dump (
        .clk(clk), .rst(rst), .go(rx_valid && rx_data == 8'h74 && frz_s[1]),
        .oldest(tr_wp), .raddr(tr_raddr), .rdata(tr_rdata), .busy(dmp_busy),
        .tx_data(dmp_data), .tx_valid(dmp_valid), .tx_ready(tx_ready)
    );

    reg [2:0]  tog_s;
    reg [21:0] act = 0;
    always @(posedge clk) begin
        tog_s <= {tog_s[1:0], serve_tog};
        if (tog_s[2] ^ tog_s[1]) act <= {22{1'b1}};
        else if (act != 0)       act <= act - 1'b1;
    end
    reg [12:0] lf_cnt = 0;
    always @(posedge clk_lf) lf_cnt <= lf_cnt + 1'b1;

    SB_RGBA_DRV #(
        .CURRENT_MODE("0b1"),
        .RGB0_CURRENT("0b000001"), .RGB1_CURRENT("0b000001"), .RGB2_CURRENT("0b000001")
    ) u_rgb (
        .CURREN(1'b1), .RGBLEDEN(1'b1),
        .RGB0PWM(pll_locked && act == 0 && !force_gate), .RGB1PWM(pll_locked && act != 0 && !force_gate),
        .RGB2PWM((!pll_locked && lf_cnt[12]) || force_gate),
        .RGB0(led_green), .RGB1(led_blue), .RGB2(led_red)
    );
endmodule

`default_nettype wire
