// lpc_probe: passive cold-boot timing logger, read with tools/lpc_probe_view.py. Time
// zero is the console's 3.3V rail rising on v33_sense.
`default_nettype none

module top_lpc_probe (
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
    upduino31_clocks #(.DIVQ(3'b101)) u_clk (
        .clk12(clk12), .clk_sys(clk), .pll_locked(pll_locked), .rst(rst), .clk_lf(clk_lf)
    );
    localparam integer UART_DIV = 208;

    wire [3:0]  rec_start, rec_ct;
    wire [31:0] rec_addr;
    wire        rec_toggle, lclk_div16;
    lpc_sniff u_sniff (
        .lclk(lpc_clk), .lreset_n(lpc_reset_n), .lframe_n(lpc_frame_n), .lad(lpc_lad),
        .rec_start(rec_start), .rec_ct(rec_ct), .rec_addr(rec_addr),
        .rec_toggle(rec_toggle), .lclk_div16(lclk_div16)
    );

    reg [2:0] v33_sync, lrst_sync, div_sync, tog_sync;
    always @(posedge clk) begin
        v33_sync  <= {v33_sync[1:0],  v33_sense};
        lrst_sync <= {lrst_sync[1:0], lpc_reset_n};
        div_sync  <= {div_sync[1:0],  lclk_div16};
        tog_sync  <= {tog_sync[1:0],  rec_toggle};
    end
    // Capture starts on the first raw high but is dropped unless the rail stays up for
    // 1ms, so a floating input can't fake a power-on.
    localparam integer V33_STABLE_CLKS = 24000;
    wire       v33_raw = v33_sync[1];
    reg [14:0] v33_cnt = 0;
    reg        v33     = 1'b0;
    always @(posedge clk) begin
        if (v33_sync[1] == v33)
            v33_cnt <= 0;
        else if (v33_cnt == V33_STABLE_CLKS - 1) begin
            v33     <= v33_sync[1];
            v33_cnt <= 0;
        end else
            v33_cnt <= v33_cnt + 1'b1;
    end
    wire lrst_hi  = lrst_sync[1];
    wire lrst_r   = lrst_sync[1] & ~lrst_sync[2];
    wire lclk_edg = div_sync[1] ^ div_sync[2];
    wire rec_new  = tog_sync[1] ^ tog_sync[2];

    reg [1:0]  frame_sync;
    reg [7:0]  lad_sync;
    always @(posedge clk) begin
        frame_sync <= {frame_sync[0], lpc_frame_n};
        lad_sync   <= {lad_sync[3:0], lpc_lad};
    end
    reg [14:0] fr_ms   = 0;
    reg        fr_tick = 0;
    reg [9:0]  fr_sec  = 0;
    reg        lclk_recent, lclk_live;
    reg        v33_prev;
    reg [7:0]  v33_tog, v33_tog_last;
    always @(posedge clk) begin
        fr_tick  <= (fr_ms == 15'd23998);
        fr_ms    <= (fr_ms == 15'd23999) ? 15'd0 : fr_ms + 1'b1;
        v33_prev <= v33_sync[1];
        if (fr_tick) begin
            lclk_live   <= lclk_recent;
            lclk_recent <= 1'b0;
        end else if (lclk_edg)
            lclk_recent <= 1'b1;
        if (fr_tick && fr_sec == 10'd999) begin
            fr_sec       <= 0;
            v33_tog_last <= v33_tog;
            v33_tog      <= 0;
        end else begin
            if (fr_tick) fr_sec <= fr_sec + 1'b1;
            if ((v33_sync[1] ^ v33_prev) && v33_tog != 8'hFF) v33_tog <= v33_tog + 1'b1;
        end
    end

    reg [5:0]  presc = 0;
    reg        us_tick, ms_tick;
    reg [23:0] t_us;
    always @(posedge clk) begin
        us_tick <= (presc == 6'd22);
        ms_tick <= (presc == 6'd22) && (t_us[9:0] == 10'h3FF);
        presc   <= (presc == 6'd23) ? 6'd0 : presc + 1'b1;
    end

    localparam [1:0] C_OFF = 2'd0, C_CAP = 2'd1, C_DONE = 2'd2;
    localparam [31:0] NEVER = 32'hFFFF_FFFF;

    reg [1:0]  cstate;
    reg [31:0] power_count;
    reg [31:0] t_lclk, t_first;
    reg [31:0] t_lrst0, t_lrst1, t_lrst2, t_lrst3;
    reg [31:0] lrst_count;
    reg [31:0] n_total, n_mrd, n_mwr, n_iord, n_iowr, n_other;
    reg [15:0] lclk_cnt;
    reg [31:0] lclk_per_ms;
    reg        lclk_seen, first_seen, lrst_full, confirmed;
    reg        have_report, report_go;
    reg        report_req;

    reg [31:0] recmem [0:31];
    reg        rm_we;
    reg [4:0]  rm_waddr;
    reg [31:0] rm_wdata;
    reg [4:0]  rm_raddr;
    reg [31:0] rm_rdata;
    always @(posedge clk) begin
        if (rm_we) recmem[rm_waddr] <= rm_wdata;
        rm_rdata <= recmem[rm_raddr];
    end

    reg [4:0]  rec_idx;
    reg        rec_pend;
    reg [31:0] rec_addr_hold;

    wire [31:0] t_now = {8'd0, t_us};

    reg        ev_any, ev_mrd, ev_mwr, ev_iord, ev_iowr, ev_other;
    reg [7:0]  ev_hdr;
    reg [31:0] ev_addr;
    wire       tgt = (rec_start == 4'b0000);
    always @(posedge clk) begin
        ev_any   <= rec_new && lrst_hi;
        ev_mrd   <= rec_new && lrst_hi && tgt && rec_ct[3:1] == 3'b010;
        ev_mwr   <= rec_new && lrst_hi && tgt && rec_ct[3:1] == 3'b011;
        ev_iord  <= rec_new && lrst_hi && tgt && rec_ct[3:1] == 3'b000;
        ev_iowr  <= rec_new && lrst_hi && tgt && rec_ct[3:1] == 3'b001;
        ev_other <= rec_new && lrst_hi && !(tgt && rec_ct[3] == 1'b0);
        ev_hdr   <= {rec_start, rec_ct};
        ev_addr  <= rec_addr;
    end

    always @(posedge clk) begin
        rm_we     <= 1'b0;
        report_go <= 1'b0;
        if (rst) begin
            cstate      <= C_OFF;
            power_count <= 0;
            have_report <= 1'b0;
        end else begin
            case (cstate)
            C_OFF: if (v33_raw) begin
                cstate      <= C_CAP;
                confirmed   <= 1'b0;
                t_us        <= 0;
                t_lclk      <= NEVER;  t_first <= NEVER;
                t_lrst0     <= NEVER;  t_lrst1 <= NEVER;
                t_lrst2     <= NEVER;  t_lrst3 <= NEVER;
                lrst_count  <= 0;
                lrst_full   <= 1'b0;
                n_total <= 0; n_mrd <= 0; n_mwr <= 0; n_iord <= 0; n_iowr <= 0; n_other <= 0;
                lclk_cnt    <= 0;
                lclk_per_ms <= 0;
                lclk_seen   <= 1'b0;
                first_seen  <= 1'b0;
                rec_idx     <= 0;
                rec_pend    <= 1'b0;
            end

            C_CAP: begin
                if (us_tick) t_us <= t_us + 1'b1;

                if (ms_tick) begin
                    lclk_per_ms <= {16'd0, lclk_cnt};
                    lclk_cnt    <= lclk_edg ? 16'd1 : 16'd0;
                end else if (lclk_edg) begin
                    lclk_cnt <= lclk_cnt + 1'b1;
                end
                if (lclk_edg && !lclk_seen) begin
                    lclk_seen <= 1'b1;
                    t_lclk    <= t_now;
                end

                if (lrst_r) begin
                    if (!lrst_full) begin
                        case (lrst_count[1:0])
                            2'd0: t_lrst0 <= t_now;
                            2'd1: t_lrst1 <= t_now;
                            2'd2: t_lrst2 <= t_now;
                            2'd3: t_lrst3 <= t_now;
                        endcase
                        if (lrst_count[1:0] == 2'd3) lrst_full <= 1'b1;
                    end
                    lrst_count <= lrst_count + 1'b1;
                end

                if (ev_mrd)   n_mrd   <= n_mrd + 1'b1;
                if (ev_mwr)   n_mwr   <= n_mwr + 1'b1;
                if (ev_iord)  n_iord  <= n_iord + 1'b1;
                if (ev_iowr)  n_iowr  <= n_iowr + 1'b1;
                if (ev_other) n_other <= n_other + 1'b1;
                if (ev_any) begin
                    n_total <= n_total + 1'b1;
                    if (!first_seen) begin
                        first_seen <= 1'b1;
                        t_first    <= t_now;
                    end
                    if (!rec_idx[4]) begin
                        rm_we         <= 1'b1;
                        rm_waddr      <= {rec_idx[3:0], 1'b0};
                        rm_wdata      <= {ev_hdr, t_us};
                        rec_addr_hold <= ev_addr;
                        rec_pend      <= 1'b1;
                    end
                end else if (rec_pend) begin
                    rm_we    <= 1'b1;
                    rm_waddr <= {rec_idx[3:0], 1'b1};
                    rm_wdata <= rec_addr_hold;
                    rec_pend <= 1'b0;
                    rec_idx  <= rec_idx + 1'b1;
                end

                if (!confirmed) begin
                    if (!v33_raw)
                        cstate <= C_OFF;
                    else if (t_us[10]) begin
                        confirmed   <= 1'b1;
                        power_count <= power_count + 1'b1;
                    end
                end

                if (confirmed && (t_us[22] || !v33)) begin
                    have_report <= 1'b1;
                    report_go   <= 1'b1;
                    cstate      <= v33 ? C_DONE : C_OFF;
                end
            end

            default: if (!v33) cstate <= C_OFF;
            endcase
        end
    end

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

    localparam integer BANNER_LEN = 43;
    localparam [8*BANNER_LEN-1:0] BANNER = "\015\nlpc_probe v1: waiting for Xbox power-on\015\n";

    localparam [2:0] P_IDLE = 3'd0, P_BANNER = 3'd1, P_LOAD = 3'd2, P_WAIT = 3'd3,
                     P_LATCH = 3'd4, P_LINE = 3'd5;
    localparam [5:0] N_WORDS = 6'd48;

    reg [2:0]  pstate;
    reg [5:0]  bidx;
    reg [5:0]  widx;
    reg [3:0]  pos;
    reg [31:0] wsh;
    reg [31:0] reg_word;

    always @(posedge clk) begin
        case (widx[3:0])
            4'h0: reg_word <= 32'h4C50_0001;
            4'h1: reg_word <= power_count;
            4'h2: reg_word <= t_lclk;
            4'h3: reg_word <= t_lrst0;
            4'h4: reg_word <= t_lrst1;
            4'h5: reg_word <= t_lrst2;
            4'h6: reg_word <= t_lrst3;
            4'h7: reg_word <= lrst_count;
            4'h8: reg_word <= t_first;
            4'h9: reg_word <= n_total;
            4'hA: reg_word <= n_mrd;
            4'hB: reg_word <= n_mwr;
            4'hC: reg_word <= n_iord;
            4'hD: reg_word <= n_iowr;
            4'hE: reg_word <= n_other;
            default: reg_word <= lclk_per_ms;
        endcase
    end

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

    reg [7:0] banner_q, line_q;
    reg       hold;
    always @(posedge clk) begin
        banner_q <= BANNER[8*(BANNER_LEN-1-bidx) +: 8];
        line_q   <= line_char;
    end

    wire enter  = rx_valid && (rx_data == 8'h0D);
    wire status = rx_valid && (rx_data == 8'h73 || rx_data == 8'h53);
    reg  single;

    reg [31:0] status_word;
    always @(posedge clk)
        status_word <= {8'h5A, v33_tog_last, lad_sync[7:4], frame_sync[1], lrst_sync[1],
                        v33_sync[1], v33, lclk_live, cstate, have_report, confirmed, 3'b000};

    always @(posedge clk) begin
        tx_valid <= 1'b0;
        if (report_go) report_req <= 1'b1;
        if (rst) begin
            report_req <= 1'b0;
            single     <= 1'b0;
            pstate <= P_BANNER;
            bidx   <= 0;
            hold   <= 1'b1;
        end else begin
            case (pstate)
            P_IDLE:
                if (status) begin
                    single <= 1'b1;
                    widx   <= 6'h3F;
                    pstate <= P_LOAD;
                end else if (report_req || (enter && have_report)) begin
                    report_req <= 1'b0;
                    single     <= 1'b0;
                    widx       <= 0;
                    pstate     <= P_LOAD;
                end else if (enter) begin
                    bidx   <= 0;
                    hold   <= 1'b1;
                    pstate <= P_BANNER;
                end
            P_BANNER:
                if (hold) hold <= 1'b0;
                else if (tx_ready && !tx_valid) begin
                    tx_data  <= banner_q;
                    tx_valid <= 1'b1;
                    if (bidx == BANNER_LEN - 1) pstate <= P_IDLE;
                    bidx <= bidx + 1'b1;
                end
            P_LOAD: begin
                rm_raddr <= widx[4:0] - 5'd16;
                pstate   <= P_WAIT;
            end
            P_WAIT:
                pstate <= P_LATCH;
            P_LATCH: begin
                wsh    <= single ? status_word : (widx[5:4] == 2'b00 ? reg_word : rm_rdata);
                pos    <= 0;
                hold   <= 1'b1;
                pstate <= P_LINE;
            end
            P_LINE:
                if (hold) hold <= 1'b0;
                else if (tx_ready && !tx_valid) begin
                    tx_data  <= line_q;
                    tx_valid <= 1'b1;
                    if (pos >= 4'd3 && pos <= 4'd10) wsh <= {wsh[27:0], 4'h0};
                    if (pos == 4'd12) begin
                        if (single || widx == N_WORDS - 1) pstate <= P_IDLE;
                        else begin
                            widx   <= widx + 1'b1;
                            pstate <= P_LOAD;
                        end
                    end
                    pos <= pos + 1'b1;
                end
            default: pstate <= P_IDLE;
            endcase
        end
    end

    reg [23:0] blink = 0;
    always @(posedge clk) blink <= blink + 1'b1;
    reg [12:0] lf_cnt = 0;
    always @(posedge clk_lf) lf_cnt <= lf_cnt + 1'b1;

    wire on_red   = !pll_locked && lf_cnt[12];
    wire on_green = pll_locked && ((cstate == C_OFF && blink[23]) || cstate == C_DONE);
    wire on_blue  = pll_locked && cstate == C_CAP;

    SB_RGBA_DRV #(
        .CURRENT_MODE("0b1"),
        .RGB0_CURRENT("0b000001"),
        .RGB1_CURRENT("0b000001"),
        .RGB2_CURRENT("0b000001")
    ) u_rgb (
        .CURREN(1'b1), .RGBLEDEN(1'b1),
        .RGB0PWM(on_green), .RGB1PWM(on_blue), .RGB2PWM(on_red),
        .RGB0(led_green), .RGB1(led_blue), .RGB2(led_red)
    );
endmodule

`default_nettype wire
