// UPduino v3.1 design "lpc_iolog": passive LPC traffic logger for reverse-engineering
// modchip register use (e.g. an OpenXenium on an LPC splitter).
//
// Streams every LPC target cycle EXCEPT memory reads (I/O reads/writes with data, memory
// writes, DMA) plus power / LRESET# events, timestamped in microseconds since the FPGA
// started. Same wiring as lpc_probe. Records are buffered in SPRAM (16K entries) and sent
// over the UART at 921600 baud as:
//     R <16 hex digits>      64-bit record (decode with tools/lpc_probe_view.py --iolog)
//     D <8 hex digits>       total records dropped so far (only printed when it changes)
// Record: [63:36] t_us (28 bit, wraps every ~268 s), [35:32] CT (LPC CYCTYPE/DIR, or F =
//         event), [31] err (read: SYNC error / no response), [30:24] reserved,
//         [23:16] data, [15:0] address (I/O port, or memory A[15:0])...
// ...memory writes need more address bits, so for CT=011x the layout is
//         [31:24] data, [23:0] address A[23:0] (err not available).
// Events (CT = F): address 1 = 3.3 V up, 2 = 3.3 V down, 3 = LRESET# released,
//         4 = LRESET# asserted.
//
// LED: blue flash = record logged, green = idle, red blink = no 12 MHz clock.
`default_nettype none

module top_lpc_iolog (
    input  wire       clk12,
    output wire       led_red,
    output wire       led_green,
    output wire       led_blue,
    output wire       flash_csn,
    output wire       uart_tx,
    input  wire       uart_rx,
    input  wire       lpc_clk,
    input  wire       lpc_reset_n,
    input  wire       lpc_frame_n,
    input  wire [3:0] lpc_lad,
    input  wire       v33_sense
);
    assign flash_csn = 1'b1;

    wire clk, pll_locked, rst, clk_lf;
    upduino31_clocks #(.DIVQ(3'b101)) u_clk (      // 24 MHz
        .clk12(clk12), .clk_sys(clk), .pll_locked(pll_locked), .rst(rst), .clk_lf(clk_lf)
    );
    localparam integer UART_DIV = 26;            // 24 MHz / 921600 (0.16 % error)

    // ------------------------------------------------------------ LCLK domain
    wire [3:0]  rec_start, rec_ct;
    wire [31:0] rec_addr;
    wire [7:0]  rec_data;
    wire        rec_err, rec_toggle, lclk_div16;
    lpc_sniff u_sniff (
        .lclk(lpc_clk), .lreset_n(lpc_reset_n), .lframe_n(lpc_frame_n), .lad(lpc_lad),
        .rec_start(rec_start), .rec_ct(rec_ct), .rec_addr(rec_addr), .rec_data(rec_data),
        .rec_err(rec_err), .rec_toggle(rec_toggle), .lclk_div16(lclk_div16)
    );

    // ------------------------------------------------------------ synchronisers
    reg [2:0] v33_sync, lrst_sync, tog_sync;
    always @(posedge clk) begin
        v33_sync  <= {v33_sync[1:0],  v33_sense};
        lrst_sync <= {lrst_sync[1:0], lpc_reset_n};
        tog_sync  <= {tog_sync[1:0],  rec_toggle};
    end
    wire rec_new = tog_sync[1] ^ tog_sync[2];

    // 3.3 V sense with 1 ms stability filter
    reg [14:0] v33_cnt = 0;
    reg        v33 = 1'b0, v33_d = 1'b0;
    always @(posedge clk) begin
        v33_d <= v33;
        if (v33_sync[1] == v33)            v33_cnt <= 0;
        else if (v33_cnt == 15'd23999) begin v33 <= v33_sync[1]; v33_cnt <= 0; end
        else                               v33_cnt <= v33_cnt + 1'b1;
    end

    // LRESET# edges, only while powered (it floats low when the console is off)
    wire lrst_rise = v33 &&  lrst_sync[1] && !lrst_sync[2];
    wire lrst_fall = v33 && !lrst_sync[1] &&  lrst_sync[2];

    // ------------------------------------------------------------ microsecond timer
    reg [4:0]  presc = 0;
    reg        us_tick = 0;
    reg [27:0] t_us = 0;
    always @(posedge clk) begin
        us_tick <= (presc == 5'd22);
        presc   <= (presc == 5'd23) ? 5'd0 : presc + 1'b1;
        if (us_tick) t_us <= t_us + 1'b1;
    end

    // ------------------------------------------------------------ FIFO state (SPRAM below)
    reg  [13:0] wr_ptr, rd_ptr;
    reg  [14:0] level;
    reg  [31:0] drops;
    reg  [1:0]  fifo_take;       // which pending slot is written this cycle (0 = none)
    reg         ram_we;
    reg  [13:0] ram_addr;
    reg  [63:0] ram_wdata;
    wire [63:0] ram_rdata;
    reg         rd_req, rd_issued, rd_valid;
    wire        full = level[14];

    // ------------------------------------------------------------ record capture
    // One pending slot per source; the FIFO writer takes one per clock.
    reg        p_lpc, p_pwr, p_rst;
    reg [63:0] r_lpc, r_pwr, r_rst;

    wire is_mem_wr = (rec_ct[3:1] == 3'b011);
    wire keep      = (rec_start == 4'b0000) && !(rec_ct[3:1] == 3'b010);   // not mem read

    always @(posedge clk) begin
        if (rst) begin
            p_lpc <= 1'b0; p_pwr <= 1'b0; p_rst <= 1'b0;
        end else begin
            // clear the slot taken by the FIFO writer first; a new record (below) wins
            if (fifo_take == 2'd1) p_lpc <= 1'b0;
            if (fifo_take == 2'd2) p_pwr <= 1'b0;
            if (fifo_take == 2'd3) p_rst <= 1'b0;
            if (rec_new && keep) begin
                p_lpc <= 1'b1;
                r_lpc <= is_mem_wr ? {t_us, rec_ct, rec_data, rec_addr[23:0]}
                                   : {t_us, rec_ct, rec_err, 7'd0, rec_data, rec_addr[15:0]};
            end
            if (v33 != v33_d) begin
                p_pwr <= 1'b1;
                // back-date the filter delay (1 ms) so the event marks the actual edge
                r_pwr <= {t_us - 28'd1000, 4'hF, 8'd0, 8'd0, v33 ? 16'd1 : 16'd2};
            end
            if (lrst_rise || lrst_fall) begin
                p_rst <= 1'b1;
                r_rst <= {t_us, 4'hF, 8'd0, 8'd0, lrst_rise ? 16'd3 : 16'd4};
            end
        end
    end

    // ------------------------------------------------------------ SPRAM FIFO

    spram64 u_ram (.clk(clk), .addr(ram_addr), .we(ram_we), .wdata(ram_wdata), .rdata(ram_rdata));

    // Port arbitration (registered): writes first, then the printer's read request.
    always @(posedge clk) begin
        ram_we    <= 1'b0;
        fifo_take <= 2'd0;
        rd_issued <= 1'b0;
        rd_valid  <= rd_issued;
        if (rst) begin
            wr_ptr <= 0; rd_ptr <= 0; level <= 0; drops <= 0;
        end else begin
            if (fifo_take == 2'd0 && !ram_we && (p_lpc || p_pwr || p_rst)) begin
                if (full) begin
                    drops     <= drops + 1'b1;
                    fifo_take <= p_lpc ? 2'd1 : p_pwr ? 2'd2 : 2'd3;   // discard
                end else begin
                    ram_we    <= 1'b1;
                    ram_addr  <= wr_ptr;
                    ram_wdata <= p_lpc ? r_lpc : p_pwr ? r_pwr : r_rst;
                    fifo_take <= p_lpc ? 2'd1 : p_pwr ? 2'd2 : 2'd3;
                    wr_ptr    <= wr_ptr + 1'b1;
                    level     <= level + 1'b1;
                end
            end else if (rd_req && !rd_issued && !rd_valid && !ram_we) begin
                ram_addr  <= rd_ptr;
                rd_issued <= 1'b1;
                rd_ptr    <= rd_ptr + 1'b1;
                level     <= level - 1'b1;
            end
        end
    end

    // ------------------------------------------------------------ UART + printer
    wire [7:0] rx_data;
    wire       rx_valid;
    uart_rx #(.CLKS_PER_BIT(UART_DIV)) u_rx (
        .clk(clk), .rst(rst), .rx(uart_rx), .data(rx_data), .valid(rx_valid)
    );
    reg  [7:0] tx_data;
    reg        tx_valid;
    wire       tx_ready;
    uart_tx #(.CLKS_PER_BIT(UART_DIV)) u_tx (
        .clk(clk), .rst(rst), .data(tx_data), .valid(tx_valid), .ready(tx_ready), .tx(uart_tx)
    );

    localparam integer BANNER_LEN = 36;
    // "\015" = CR
    localparam [8*BANNER_LEN-1:0] BANNER = "\015\nlpc_iolog v1: logging LPC cycles\015\n";

    localparam [2:0] P_IDLE = 3'd0, P_BANNER = 3'd1, P_WAITRD = 3'd2, P_LINE = 3'd3;
    reg [2:0]  pstate;
    reg [5:0]  bidx;
    reg [4:0]  pos;
    reg [63:0] wsh;
    reg        is_drop;          // printing a "D" line
    reg [31:0] drops_shown;
    reg [7:0]  banner_q, line_q;
    reg        hold;

    function [7:0] hexc(input [3:0] n);
        hexc = (n < 4'd10) ? (8'h30 + n) : (8'h37 + n);
    endfunction

    // R line: 'R' ' ' 16 hex CR LF (pos 0..19); D line: 'D' ' ' 8 hex CR LF (pos 0..11)
    wire [4:0] last_hex = is_drop ? 5'd9 : 5'd17;
    reg  [7:0] line_char;
    always @(*) begin
        if (pos == 5'd0)                 line_char = is_drop ? 8'h44 : 8'h52;
        else if (pos == 5'd1)            line_char = 8'h20;
        else if (pos <= last_hex)        line_char = hexc(wsh[63:60]);
        else if (pos == last_hex + 1'b1) line_char = 8'h0D;
        else                             line_char = 8'h0A;
    end
    always @(posedge clk) begin
        banner_q <= BANNER[8*(BANNER_LEN-1-bidx) +: 8];
        line_q   <= line_char;
    end

    wire enter = rx_valid && (rx_data == 8'h0D);

    always @(posedge clk) begin
        tx_valid <= 1'b0;
        if (rst) begin
            pstate <= P_BANNER; bidx <= 0; hold <= 1'b1; rd_req <= 1'b0; drops_shown <= 0;
        end else begin
            case (pstate)
            P_IDLE:
                if (enter) begin
                    bidx <= 0; hold <= 1'b1; pstate <= P_BANNER;
                end else if (level != 0) begin
                    rd_req <= 1'b1;
                    pstate <= P_WAITRD;
                end else if (drops != drops_shown) begin
                    drops_shown <= drops;
                    wsh     <= {drops, 32'd0};
                    is_drop <= 1'b1;
                    pos     <= 0;
                    hold    <= 1'b1;
                    pstate  <= P_LINE;
                end
            P_BANNER:
                if (hold) hold <= 1'b0;
                else if (tx_ready && !tx_valid) begin
                    tx_data  <= banner_q;
                    tx_valid <= 1'b1;
                    if (bidx == BANNER_LEN - 1) pstate <= P_IDLE;
                    bidx <= bidx + 1'b1;
                end
            P_WAITRD: begin
                if (rd_issued) rd_req <= 1'b0;
                if (rd_valid) begin
                    wsh     <= ram_rdata;
                    is_drop <= 1'b0;
                    pos     <= 0;
                    hold    <= 1'b1;
                    pstate  <= P_LINE;
                end
            end
            P_LINE:
                if (hold) hold <= 1'b0;
                else if (tx_ready && !tx_valid) begin
                    tx_data  <= line_q;
                    tx_valid <= 1'b1;
                    if (pos >= 5'd2 && pos <= last_hex) wsh <= {wsh[59:0], 4'h0};
                    if (pos == last_hex + 5'd2) pstate <= P_IDLE;
                    pos  <= pos + 1'b1;
                    hold <= 1'b1;        // let line_q catch up with the new pos
                end
            default: pstate <= P_IDLE;
            endcase
        end
    end

    // ------------------------------------------------------------ LED
    reg [21:0] act = 0;
    always @(posedge clk)
        if (rec_new && keep) act <= {22{1'b1}};
        else if (act != 0)   act <= act - 1'b1;
    reg [12:0] lf_cnt = 0;
    always @(posedge clk_lf) lf_cnt <= lf_cnt + 1'b1;

    SB_RGBA_DRV #(
        .CURRENT_MODE("0b1"),
        .RGB0_CURRENT("0b000001"), .RGB1_CURRENT("0b000001"), .RGB2_CURRENT("0b000001")
    ) u_rgb (
        .CURREN(1'b1), .RGBLEDEN(1'b1),
        .RGB0PWM(pll_locked && act == 0), .RGB1PWM(pll_locked && act != 0),
        .RGB2PWM(!pll_locked && lf_cnt[12]),
        .RGB0(led_green), .RGB1(led_blue), .RGB2(led_red)
    );
endmodule

`default_nettype wire
