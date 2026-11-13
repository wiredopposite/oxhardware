// UPduino v3.1 design "lpc_com": BIOS serving, COM port, Super I/O, boot control port,
// mailbox and the full-speed USB device.
//
// BIOS: every LPC memory read in FFxxxxxx from the active bank (bank_ctrl: 256 K slots
// from flash offset 0x40000, slot 0 by default, mirrored), 1.6 LFRAME# abort through the
// external N-MOSFET (lframe_gate, all four identical), boot hold until the read of ..74.
// COM: 16550 registers at 0x3F8, or wherever the guest places LDN 4/5 through the
// Super I/O at 0x2E/0x2F. Its data goes to the UART pins at a fixed 115200 8N1; the
// guest's divisor is ignored.
//
// Boot control port for the Loader at 0x00E0 (bootctl_port), one-shot armed boots.
// Mailbox: the programmer protocol's console port at 0x0B00, to the ESP32 over the link.
// Flash: the ESP32 borrows it (flash_share) and reads, programs and erases it itself over
// the config bus; BIOS reads meanwhile get 0xFF.
// USB: D+/D- to the console's controller port (usb_core); 48 MHz for its line side, the
// rest of the design on 24 MHz divided from it.
//
// Write the image first:  make DESIGN=lpc_com rom ROM=<file>
// Link: ESP32 SPI slave, register space 0 (identity, scratch, LPC status, banks, IRQs,
// flash, mailbox, USB 0x200-0x25F), SPACE 1 (USB buffers), SPACE 3 port 1 (mailbox FIFOs).
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
    output wire       fpga_irq_n,
    // USB to the console's controller port: D+/D- through 27 R, 1.5 k from usb_pu to D+
    inout  wire       usb_dp,
    inout  wire       usb_dn,
    output wire       usb_pu
);
    localparam integer BAUD_DIV = 208;           // 24 MHz / 115200

    wire clk, clk48, pll_locked, rst, clk_lf;
    upduino31_clocks #(.DUAL(1)) u_clk (           // 24 MHz system, 48 MHz USB line side
        .clk12(clk12), .clk_sys(clk), .clk_2x(clk48), .pll_locked(pll_locked), .rst(rst),
        .clk_lf(clk_lf)
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
    wire [23:0] st_addr;

    // Quasi-static settings from bank_ctrl, registered once in this domain
    wire [7:0]  lpc_ctrl, bank_base;
    wire [1:0]  bank_size;
    reg  [7:0]  base_l = 8'h04;
    reg  [23:0] mask_l = 24'h03FFFF;
    reg         serve_en = 1'b1, lframe_abort = 1'b1, io_en = 1'b1;
    always @(posedge lpc_clk) begin
        base_l       <= bank_base;
        mask_l       <= (bank_size == 2'd2) ? 24'h0FFFFF :
                        (bank_size == 2'd1) ? 24'h07FFFF : 24'h03FFFF;
        serve_en     <= lpc_ctrl[0];
        lframe_abort <= lpc_ctrl[2];
        io_en        <= lpc_ctrl[4];
    end
    wire [15:0] io_addr, io_addr_nxt;
    wire        io_write, io_addr_last, io_hit, io_rd, io_wr;
    wire [7:0]  io_rdata, io_wdata;
    wire        io_done, io_took;

    assign lpc_lad     = lad_oe ? lad_out : 4'bzzzz;
    assign lframe_gate = {4{lframe_oe}};

    lpc_target u_tgt (
        .lclk(lpc_clk), .lreset_n(lreset_n), .lframe_n(lframe_sense), .lad_in(lpc_lad),
        .lad_out(lad_out), .lad_oe(lad_oe), .lframe_oe(lframe_oe),
        .serve_en(serve_en), .lframe_abort(lframe_abort), .io_en(io_en),
        .rom_base({base_l, 16'h0000}), .rom_mask(mask_l),
        .req(req), .req_addr(req_addr), .ack(ack), .ack_data(ack_data),
        .io_addr(io_addr), .io_write(io_write), .io_addr_nxt(io_addr_nxt),
        .io_addr_last(io_addr_last), .io_hit(io_hit), .io_rd(io_rd),
        .io_rdata(io_rdata), .io_wr(io_wr), .io_wdata(io_wdata),
        .io_done(io_done), .io_took(io_took),
        .stat_served(st_served), .stat_timeout(), .stat_wait(),
        .in_reset(tgt_in_reset), .stat_recover(), .stat_badaddr(),
        .stat_addr(st_addr), .stat_data(), .lframe_holding(holding), .dbg_state()
    );

    wire f_cs_n, f_sck, f_mosi, s_req, s_ack, fs_hold, fs_grant;
    wire [7:0] s_data;
    spi_stream u_flash (
        .clk(lpc_clk), .rst(tgt_in_reset || fs_hold),  // re-wake the flash on every boot
        .park(1'b0), .parked(),
        .req(s_req), .req_addr(req_addr), .ack(s_ack), .ack_data(s_data),
        .restarted(restarted),
        .spi_cs_n(f_cs_n), .spi_sck(f_sck), .spi_mosi(f_mosi), .spi_miso(flash_miso)
    );
    // spi_stream drives the flash only while the console is out of reset and the ESP32
    // hasn't got it (CS# has a pull-up), so the ESP32 or iceprog can write it.
    wire f_float = tgt_in_reset || fs_grant;
    assign flash_csn  = f_float ? 1'bz : f_cs_n;
    assign flash_sck  = f_float ? 1'bz : f_sck;
    assign flash_mosi = f_float ? 1'bz : f_mosi;

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
    wire        bc_hit, bc_tog;
    wire [7:0]  bc_rdata, bc_data;
    wire [2:0]  bc_off;
    wire [15:0] bootctl_base;
    wire [63:0] bc_vals;
    wire        mbx_io_hit;
    wire [7:0]  mbx_io_rdata;
    assign io_hit   = sio_hit || com_hit || bc_hit || mbx_io_hit;
    // Every device registers its read data with its decode, so this is the only logic
    // between those flip-flops and lpc_target.
    assign io_rdata = ({8{bc_hit}} & bc_rdata) | ({8{mbx_io_hit}} & mbx_io_rdata) |
                      ({8{sel_idx || sel_data}} & sio_rdata) | ({8{com_hit}} & com_rdata);

    // Boot control port (the Loader's)
    bootctl_port u_bcp (
        .clk(lpc_clk), .en(serve_en), .base(bootctl_base), .vals(bc_vals),
        .io_addr_last(io_addr_last), .io_addr_nxt(io_addr_nxt), .io_addr(io_addr),
        .io_wr(io_wr), .io_wdata(io_wdata), .hit(bc_hit), .rdata(bc_rdata),
        .wr_tog(bc_tog), .wr_off(bc_off), .wr_data(bc_data)
    );

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
        .reg_a(io_addr[2:0]), .sample(io_addr_last), .sample_a(io_addr_nxt[2:0]),
        .rd(io_rd && com_hit), .wr(io_wr && com_hit),
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
    wire        usb_oe, usb_dp_o, usb_dn_o, usb_pu_en, usb_hit, usb_irq, usb_irq_lvl;
    wire [7:0]  usb_rdata, usb_bdata;
    wire        armed, irq, bank_hit, l_rd_sent, l_idle, mbx_hit, mbx_irq, mbx_sts, fs_hit;
    wire [7:0]  mbx_rdata, mbx_fdata, fs_rdata;
    wire [7:0]  bank_rdata;
    wire [2:0]  l_space;
    wire [23:0] l_addr;
    wire [7:0]  l_wdata, l_rdata, reg_rdata;
    link_slave u_link (
        .clk(clk), .rst(rst),
        .sck(link_sck), .cs_n(link_cs_n), .mosi(link_mosi),
        .miso(l_miso), .miso_oe(l_miso_oe),
        .sts({irq, lrst_s[1], !inrst_s[1], usb_irq_lvl, 1'b0, mbx_sts, 1'b0, fs_grant}),
        .wr(l_wr), .rd(l_rd), .space(l_space), .addr(l_addr),
        .wdata(l_wdata), .rdata(l_rdata), .rd_sent(l_rd_sent), .idle(l_idle)
    );
    assign link_miso  = l_miso_oe ? l_miso : 1'bz;
    assign fpga_irq_n = !irq;

    // Console power: its LPC clock runs whenever it is on, also in reset
    wire lclk_run, lclk_chg;
    lclk_detect u_lclk (.lclk(lpc_clk), .clk(clk), .running(lclk_run), .changed(lclk_chg));

    link_regs #(.BOARD_ID(8'h01), .FEATURES(16'h025F)) u_regs (
        .clk(clk), .wr(l_wr && l_space == 3'd0), .addr(l_addr[15:0]), .wdata(l_wdata),
        .rdata(reg_rdata), .lreset_n(lrst_s[1]), .lclk_running(lclk_run),
        .served_pulse(served_pulse),
        .armed(armed)
    );
    bank_ctrl u_bank (
        .clk(clk), .wr(l_wr && l_space == 3'd0), .addr(l_addr[15:0]), .wdata(l_wdata),
        .rdata(bank_rdata), .hit(bank_hit),
        .lreset_n(lrst_s[1]), .served_pulse(served_pulse), .served_addr(st_addr),
        .cp_tog(bc_tog), .cp_off(bc_off), .cp_data(bc_data),
        .irq_set({7'd0, lclk_chg, 2'd0, mbx_irq, 1'b0, usb_irq, 3'd0}),
        .lpc_ctrl(lpc_ctrl), .bank_base(bank_base), .bank_size(bank_size),
        .bootctl_base(bootctl_base), .port_vals(bc_vals), .armed(armed), .irq(irq)
    );
    wire l_mbx = (l_space == 3'd3) && (l_addr[7:0] == 8'h01);
    mailbox u_mbx (
        .clk(clk), .wr(l_wr && l_space == 3'd0), .addr(l_addr[15:0]), .wdata(l_wdata),
        .rdata(mbx_rdata), .hit(mbx_hit),
        .f_wr(l_wr && l_mbx), .f_rd(l_rd && l_mbx), .f_rdata(mbx_fdata),
        .f_done(l_rd_sent && l_mbx), .f_idle(l_idle), .irq(mbx_irq), .sts_rx(mbx_sts),
        .lclk(lpc_clk), .io_en(io_en), .io_addr_last(io_addr_last),
        .io_addr_nxt(io_addr_nxt), .io_rd(io_rd), .io_wr(io_wr), .io_wdata(io_wdata),
        .io_hit(mbx_io_hit), .io_rdata(mbx_io_rdata)
    );
    flash_share u_fs (
        .clk(clk), .wr(l_wr && l_space == 3'd0), .addr(l_addr[15:0]), .wdata(l_wdata),
        .rdata(fs_rdata), .hit(fs_hit),
        .served_pulse(served_pulse), .lreset_n(lrst_s[1]), .grant(fs_grant),
        .lclk(lpc_clk), .hold_l(fs_hold),
        .req(req), .ack(ack), .ack_data(ack_data),
        .s_req(s_req), .s_ack(s_ack), .s_data(s_data)
    );
    // ------------------------------------------------------------ USB
    usb_core u_usb (
        .clk48(clk48), .clk(clk), .rst(rst),
        .dp_in(usb_dp), .dn_in(usb_dn), .dp_out(usb_dp_o), .dn_out(usb_dn_o), .oe(usb_oe),
        .pu_en(usb_pu_en),
        .wr(l_wr && l_space == 3'd0), .addr(l_addr[15:0]), .wdata(l_wdata),
        .rdata(usb_rdata), .hit(usb_hit),
        .b1_wr(l_wr && l_space == 3'd1), .b1_addr(l_addr[11:0]), .b1_rdata(usb_bdata),
        .irq_pulse(usb_irq), .irq_level(usb_irq_lvl)
    );
    assign usb_dp = usb_oe ? usb_dp_o : 1'bz;
    assign usb_dn = usb_oe ? usb_dn_o : 1'bz;
    assign usb_pu = usb_pu_en ? 1'b1 : 1'bz;

    assign l_rdata = l_mbx ? mbx_fdata : (l_space == 3'd1) ? usb_bdata :
                     (l_space != 3'd0) ? 8'h00 :
                     bank_hit ? bank_rdata : mbx_hit ? mbx_rdata : fs_hit ? fs_rdata :
                     usb_hit ? usb_rdata : reg_rdata;

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
