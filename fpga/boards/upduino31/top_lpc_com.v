// UPduino v3.1 design "lpc_com": lpc_serve's BIOS serving plus a 16550 COM port and a
// Super I/O configuration space, so a debug kernel's serial output reaches the PC.
//
// BIOS: every LPC memory read in FFxxxxxx from a 256 KB image at flash offset 0x40000
// (mirrored), 1.6 LFRAME# abort through the external N-MOSFET (lframe_gate, all four
// identical), boot hold until the read of ..74.
// COM: 16550 registers at 0x3F8, or wherever the guest places LDN 4/5 through the
// Super I/O at 0x2E/0x2F. Its data goes to the UART pins at a fixed 115200 8N1; the
// guest's divisor is ignored.
//
// Write the image first:  make DESIGN=lpc_com rom ROM=<file>
// Link: ESP32 SPI slave, register space 0 (identity, scratch, LPC status).
// LED: blue = serving reads, green = idle, red = COM output, red blink = no 12 MHz clock.
`default_nettype none

module top_lpc_com (
    input  wire       clk12,
    output wire       led_red,
    output wire       led_green,
    output wire       led_blue,
    output wire       uart_tx,
    input  wire       uart_rx,
    // Xbox LPC
    input  wire       lpc_clk,
    input  wire       lpc_reset_n,
    output wire [3:0] lframe_gate,     // N-MOSFET gate drive (all four identical)
    inout  wire [3:0] lpc_lad,
    input  wire       v33_sense,
    input  wire       lframe_sense,    // LFRAME# sensed directly on the bus
    // on-board config flash (user mode after configuration)
    output wire       flash_csn,
    output wire       flash_sck,
    output wire       flash_mosi,
    input  wire       flash_miso,
    // ESP32 link
    input  wire       link_sck,
    input  wire       link_cs_n,
    input  wire       link_mosi,
    output wire       link_miso,
    output wire       fpga_irq_n
);
    localparam [23:0] ROM_BASE = 24'h040000;
    localparam [23:0] ROM_MASK = 24'h03FFFF;     // 256 KB
    localparam integer BAUD_DIV = 208;           // 24 MHz / 115200

    wire clk, pll_locked, rst, clk_lf;
    upduino31_clocks #(.DIVQ(3'b101)) u_clk (      // 24 MHz
        .clk12(clk12), .clk_sys(clk), .pll_locked(pll_locked), .rst(rst), .clk_lf(clk_lf)
    );

    // LRESET# through a glitch filter (system clock): everything below uses lreset_n
    wire lreset_n;
    lreset_filter u_lrf (.clk(clk), .lreset_n_pin(lpc_reset_n), .lreset_n(lreset_n));

    // ------------------------------------------------------------ LCLK domain: LPC target
    wire [3:0]  lad_out;
    wire        lad_oe, lframe_oe, holding;
    wire        req, ack, restarted;
    wire [23:0] req_addr;
    wire [7:0]  ack_data;
    wire        st_served, tgt_in_reset;
    wire [15:0] io_addr, io_addr_nxt;
    wire        io_write, io_addr_last, io_hit, io_rd, io_wr;
    wire [7:0]  io_rdata, io_wdata;
    wire        io_done, io_took;

    assign lpc_lad     = lad_oe ? lad_out : 4'bzzzz;
    assign lframe_gate = {4{lframe_oe}};

    lpc_target u_tgt (
        .lclk(lpc_clk), .lreset_n(lreset_n), .lframe_n(lframe_sense), .lad_in(lpc_lad),
        .lad_out(lad_out), .lad_oe(lad_oe), .lframe_oe(lframe_oe),
        .serve_en(1'b1), .lframe_abort(1'b1), .io_en(1'b1),
        .rom_base(ROM_BASE), .rom_mask(ROM_MASK),
        .req(req), .req_addr(req_addr), .ack(ack), .ack_data(ack_data),
        .io_addr(io_addr), .io_write(io_write), .io_addr_nxt(io_addr_nxt),
        .io_addr_last(io_addr_last), .io_hit(io_hit), .io_rd(io_rd),
        .io_rdata(io_rdata), .io_wr(io_wr), .io_wdata(io_wdata),
        .io_done(io_done), .io_took(io_took),
        .stat_served(st_served), .stat_timeout(), .stat_wait(),
        .in_reset(tgt_in_reset), .stat_recover(), .stat_badaddr(),
        .stat_addr(), .stat_data(), .lframe_holding(holding), .dbg_state()
    );

    wire f_cs_n, f_sck, f_mosi;
    spi_stream u_flash (
        .clk(lpc_clk), .rst(tgt_in_reset),           // re-wake the flash on every boot
        .req(req), .req_addr(req_addr), .ack(ack), .ack_data(ack_data),
        .restarted(restarted),
        .spi_cs_n(f_cs_n), .spi_sck(f_sck), .spi_mosi(f_mosi), .spi_miso(flash_miso)
    );
    // The flash pins are only driven while the console is out of reset, so a programmer
    // on the same pins (iceprog) never fights a running design. CS# has a pull-up.
    assign flash_csn  = tgt_in_reset ? 1'bz : f_cs_n;
    assign flash_sck  = tgt_in_reset ? 1'bz : f_sck;
    assign flash_mosi = tgt_in_reset ? 1'bz : f_mosi;

    // ------------------------------------------------------------ LCLK domain: I/O devices
    // Port decode, registered on the last address nibble so the claim decision on the
    // next clock starts from flip-flops.
    wire        sio_cfg, sio_active;
    wire [15:0] sio_base;
    wire [7:0]  sio_rdata, com_rdata;
    reg  [15:0] com_base = 16'h03F8;
    reg         sel_idx = 1'b0, sel_data = 1'b0, com_hit = 1'b0;
    always @(posedge lpc_clk) begin
        com_base <= sio_active ? sio_base : 16'h03F8;
        if (io_addr_last) begin
            sel_idx  <= (io_addr_nxt == 16'h002E);
            sel_data <= (io_addr_nxt == 16'h002F);
            com_hit  <= (io_addr_nxt[15:3] == com_base[15:3]);
        end
    end
    wire sio_hit = (sel_idx && (io_write || sio_cfg)) || (sel_data && sio_cfg);
    assign io_hit   = sio_hit || com_hit;
    assign io_rdata = (sel_idx || sel_data) ? sio_rdata : com_rdata;

    superio_cfg u_sio (
        .clk(lpc_clk), .in_reset(tgt_in_reset),
        .sel_idx(sel_idx), .sel_data(sel_data), .cfg(sio_cfg),
        .wr(io_wr && sio_hit), .wdata(io_wdata), .rdata(sio_rdata),
        .com_active(sio_active), .com_base(sio_base)
    );

    wire       tx_push, tx_full, rx_valid, rx_pop;
    wire [7:0] tx_byte, rx_byte;
    com16550 u_com (
        .clk(lpc_clk), .in_reset(tgt_in_reset),
        .reg_a(io_addr[2:0]), .rd(io_rd && com_hit), .wr(io_wr && com_hit),
        .wdata(io_wdata), .rdata(com_rdata),
        .tx_push(tx_push), .tx_data(tx_byte), .tx_full(tx_full),
        .rx_valid(rx_valid), .rx_data(rx_byte), .rx_pop(rx_pop)
    );

    // ------------------------------------------------------------ FIFOs and UART
    wire       utx_valid, utx_ready;
    wire [7:0] utx_data;
    async_fifo u_txf (
        .wclk(lpc_clk), .wr(tx_push), .wdata(tx_byte), .full(tx_full),
        .rclk(clk), .rd(utx_valid && utx_ready), .rdata(utx_data), .rvalid(utx_valid)
    );
    uart_tx #(.CLKS_PER_BIT(BAUD_DIV)) u_utx (
        .clk(clk), .rst(rst),
        .data(utx_data), .valid(utx_valid),
        .ready(utx_ready), .tx(uart_tx)
    );

    wire       urx_valid;
    wire [7:0] urx_data;
    uart_rx #(.CLKS_PER_BIT(BAUD_DIV)) u_urx (
        .clk(clk), .rst(rst), .rx(uart_rx), .data(urx_data), .valid(urx_valid)
    );
    async_fifo u_rxf (
        .wclk(clk), .wr(urx_valid), .wdata(urx_data), .full(),
        .rclk(lpc_clk), .rd(rx_pop), .rdata(rx_byte), .rvalid(rx_valid)
    );

    // ------------------------------------------------------------ ESP32 link
    reg serve_tog = 1'b0;
    always @(posedge lpc_clk) if (st_served) serve_tog <= ~serve_tog;

    reg [2:0] tog_s = 3'd0;
    reg [1:0] lrst_s = 2'b00, inrst_s = 2'b11;
    always @(posedge clk) begin
        tog_s   <= {tog_s[1:0], serve_tog};
        lrst_s  <= {lrst_s[0], lreset_n};
        inrst_s <= {inrst_s[0], tgt_in_reset};
    end
    wire served_pulse = tog_s[2] ^ tog_s[1];

    wire        l_wr, l_rd, l_miso, l_miso_oe;
    wire [2:0]  l_space;
    wire [23:0] l_addr;
    wire [7:0]  l_wdata, l_rdata, reg_rdata;
    link_slave u_link (
        .clk(clk), .rst(rst),
        .sck(link_sck), .cs_n(link_cs_n), .mosi(link_mosi),
        .miso(l_miso), .miso_oe(l_miso_oe),
        .sts({1'b0, lrst_s[1], !inrst_s[1], 5'd0}),
        .wr(l_wr), .rd(l_rd), .space(l_space), .addr(l_addr),
        .wdata(l_wdata), .rdata(l_rdata)
    );
    assign link_miso  = l_miso_oe ? l_miso : 1'bz;
    assign fpga_irq_n = 1'b1;

    link_regs #(.BOARD_ID(8'h01), .FEATURES(16'h0047)) u_regs (
        .clk(clk), .wr(l_wr && l_space == 3'd0), .addr(l_addr[15:0]), .wdata(l_wdata),
        .rdata(reg_rdata), .lreset_n(lrst_s[1]), .served_pulse(served_pulse)
    );
    assign l_rdata = (l_space == 3'd0) ? reg_rdata : 8'h00;

    // ------------------------------------------------------------ LED
    reg [21:0] act = 0, com_act = 0;
    always @(posedge clk) begin
        if (served_pulse) act <= {22{1'b1}};
        else if (act != 0)       act <= act - 1'b1;
        if (utx_valid && utx_ready) com_act <= {22{1'b1}};
        else if (com_act != 0)      com_act <= com_act - 1'b1;
    end
    reg [12:0] lf_cnt = 0;
    always @(posedge clk_lf) lf_cnt <= lf_cnt + 1'b1;

    SB_RGBA_DRV #(
        .CURRENT_MODE("0b1"),
        .RGB0_CURRENT("0b000001"), .RGB1_CURRENT("0b000001"), .RGB2_CURRENT("0b000001")
    ) u_rgb (
        .CURREN(1'b1), .RGBLEDEN(1'b1),
        .RGB0PWM(pll_locked && act == 0 && com_act == 0),
        .RGB1PWM(pll_locked && act != 0),
        .RGB2PWM((!pll_locked && lf_cnt[12]) || (pll_locked && com_act != 0)),
        .RGB0(led_green), .RGB1(led_blue), .RGB2(led_red)
    );
endmodule

`default_nettype wire
