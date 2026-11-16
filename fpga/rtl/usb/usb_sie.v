// USB full-speed device SIE for up to 8 devices at their own addresses (a hub and the
// devices behind it), with 32 endpoint slots: 0-15 IN, 16-31 OUT, and slots d and 16+d
// are device d's EP0. A token is resolved by RAM lookups: address to device, then
// {device, endpoint, direction} to slot, then the slot's state and IN length.
//
// Link space 1:
//   IN buffers   0x000-0x3FF  write, slot k at k*64
//   OUT buffers  0x400-0x7FF  read,  slot 16+k at 0x400 + k*64
//   SETUP        0x800-0x83F  read,  device d at 0x800 + d*8
//   OUT length   0x840-0x84F  read,  bytes received by slot 16+k
//   EP map       0xC00-0xCFF  write, [{dev, ep, in}] = bit7 valid, bits 3:0 slot
//   IN arm       0xD00-0xD0F  write, arm IN slot k with N bytes
//
// Slot commands, arms and address map writes from the ESP32 are queued and run only
// between transactions. The handshake is decided here within the turnaround time, so the
// ESP32 is never in that loop. A SET_ADDRESS is applied when the status stage's
// zero-length IN is acknowledged.
`default_nettype none

module usb_sie (
    input  wire        clk,
    input  wire        rst,
    input  wire        ev_tog,
    input  wire [1:0]  ev_kind,
    input  wire [7:0]  ev_data,
    input  wire [2:0]  ev_flags,
    input  wire        se0_long,
    input  wire [1:0]  line,
    output reg         b_tog = 1'b0,
    output reg  [7:0]  b_data = 8'd0,
    output reg         b_last = 1'b0,
    output reg         b_crc = 1'b0,
    input  wire        b_taken,
    input  wire        tx_done,
    output wire        pu_en,
    input  wire        wr,
    input  wire [15:0] addr,
    input  wire [7:0]  wdata,
    output reg  [7:0]  rdata = 8'd0,
    output wire        hit,
    input  wire        b1_wr,
    input  wire [11:0] b1_addr,
    output reg  [7:0]  b1_rdata = 8'd0,
    output reg         irq_pulse = 1'b0,
    output wire        irq_level
);
    assign hit = (addr[15:8] == 8'h02);

    reg [2:0] ev_s = 3'd0, tk_s = 3'd0, dn_s = 3'd0;
    reg [1:0] se0_s = 2'd0, line_s0 = 2'b10, line_s = 2'b10;
    always @(posedge clk) begin
        ev_s    <= {ev_s[1:0], ev_tog};
        tk_s    <= {tk_s[1:0], b_taken};
        dn_s    <= {dn_s[1:0], tx_done};
        se0_s   <= {se0_s[0], se0_long};
        line_s0 <= line;
        line_s  <= line_s0;
    end
    wire ev    = ev_s[2] ^ ev_s[1];
    wire taken = tk_s[2] ^ tk_s[1];
    wire txend = dn_s[2] ^ dn_s[1];
    wire in_reset = se0_s[1];
    wire ev_start = ev && ev_kind == 2'd0;
    wire ev_byte  = ev && ev_kind == 2'd1;
    wire ev_end   = ev && ev_kind == 2'd2;
    wire f_crc5   = ev_flags[0], f_crc16 = ev_flags[1], f_err = ev_flags[2];

    reg  [1:0]  ctrl = 2'b00;               // bit0 PULLUP, bit1 SIE_EN
    assign pu_en = ctrl[0];
    reg  [6:0]  map_a = 7'd0;
    reg         p_on = 1'b0;
    reg  [2:0]  p_dev = 3'd0;
    reg  [6:0]  p_addr = 7'd0;
    reg  [7:0]  setup_dev = 8'd0;
    reg  [15:0] in_done = 16'd0, out_done = 16'd0;
    reg  [3:0]  irq_lat = 4'd0;             // bus reset, suspend, resume, SOF
    reg  [7:0]  irq_en = 8'd0;
    reg  [10:0] frame = 11'd0;
    reg         suspended = 1'b0;
    reg  [17:0] idle_cnt = 18'd0;

    wire [7:0] irq_view = {|out_done, |in_done, |setup_dev, 1'b0, irq_lat};
    assign irq_level = |(irq_view & irq_en);

    reg [7:0] iram [0:1023];
    reg [7:0] tram [0:511];
    reg [7:0] oram [0:1103];
    reg [9:0]  i_ra = 10'd0;
    reg [8:0]  t_ra = 9'd0;
    reg [7:0]  i_q = 8'd0, t_q = 8'd0;
    reg        o_we = 1'b0;
    reg [10:0] o_wa = 11'd0;
    reg [7:0]  o_wd = 8'd0;
    wire b1_iram = b1_wr && b1_addr[11:10] == 2'b00;
    wire b1_tram = b1_wr && b1_addr[11:9] == 3'b110;
    always @(posedge clk) begin
        if (b1_iram) iram[b1_addr[9:0]] <= wdata;
        if (b1_tram) tram[b1_addr[8:0]] <= wdata;
        i_q <= iram[i_ra];
        t_q <= tram[t_ra];
        if (o_we) oram[o_wa] <= o_wd;
        b1_rdata <= oram[{b1_addr[11], b1_addr[9:0]}];
    end
    wire       b1_arm   = b1_wr && b1_addr[11:4] == 8'hD0;

    // 0-31 slot state: bit3 enable, bit2 stall, bit1 toggle, bit0 armed (IN) / full (OUT)
    // 128-255 address map: [128 + address] = bit3 valid, bits 2:0 device
    (* ram_style = "block" *) reg [3:0] sram [0:255];
    reg  [7:0] sr_ra = 8'd0, sr_wa = 8'd0;
    reg  [3:0] sr_q = 4'd0, sr_wd = 4'd0;
    reg        sr_we = 1'b0;
    always @(posedge clk) begin
        if (sr_we) sram[sr_wa] <= sr_wd;
        sr_q <= sram[sr_ra];
    end

    // Command queue: {type, index, data}. Type 0 is a slot command, 1 an arm, 2 an
    // address map write.
    (* ram_style = "block" *) reg [15:0] cq [0:255];
    reg  [7:0]  cq_wp = 8'd0, cq_rp = 8'd0;
    reg  [15:0] cq_q = 16'd0;
    reg         cq_ne = 1'b0;
    wire        ex_go;
    wire        l_cmd = wr && hit && addr[7:5] == 3'b010;
    wire        l_map = wr && hit && addr[7:0] == 8'h11;
    always @(posedge clk) begin
        if (l_cmd || b1_arm || l_map) begin
            cq[cq_wp] <= b1_arm ? {2'd1, 3'd0, b1_addr[3:0], 7'd0} :
                         l_map  ? {2'd2, map_a, 3'd0, wdata[7], wdata[2:0]} :
                                  {2'd0, 2'd0, addr[4:0], wdata[6:0]};
            cq_wp <= cq_wp + 1'b1;
        end
        cq_q  <= cq[cq_rp];
        cq_ne <= (cq_wp != cq_rp);
    end

    reg  [6:0] tok_a = 7'd0;
    reg  [3:0] tok_ep = 4'd0;

    localparam [3:0] S_IDLE = 4'd0, S_PID = 4'd1, S_TOK1 = 4'd2, S_TOK2 = 4'd3,
                     S_TOKEND = 4'd4, S_DECIDE = 4'd5, S_DATA = 4'd6, S_DEND = 4'd7,
                     S_TX = 4'd8, S_TXW = 4'd9, S_ACKW = 4'd10, S_ACKPID = 4'd11,
                     S_ACKEND = 4'd12, S_SKIP = 4'd13;
    localparam [3:0] P_OUT = 4'h1, P_IN = 4'h9, P_SOF = 4'h5, P_SETUP = 4'hD,
                     P_DATA0 = 4'h3, P_DATA1 = 4'hB, P_ACK = 4'h2, P_NAK = 4'hA,
                     P_STALL = 4'hE;

    reg [3:0]  st = S_IDLE;
    reg [3:0]  pid = 4'd0;
    reg [7:0]  tok1 = 8'd0;
    reg [2:0]  lk = 3'd0;
    reg        lk_done = 1'b0, m_hit = 1'b0, m_ok = 1'b0;
    reg [2:0]  m_dev = 3'd0;
    reg [4:0]  m_slot = 5'd0;
    reg        m_en = 1'b0, m_stall = 1'b0, m_tog = 1'b0, m_full = 1'b0, m_ep0 = 1'b0;
    reg [6:0]  m_len = 7'd0;
    reg [5:0]  m_lenm1 = 6'd0;
    reg        x_setup = 1'b0, x_out = 1'b0;
    reg [2:0]  x_dev = 3'd0;
    reg [4:0]  x_slot = 5'd0;
    reg        x_ep0 = 1'b0, x_zlp = 1'b0, x_dtog = 1'b0, x_wok = 1'b0, x_ovf = 1'b0;
    reg [6:0]  n = 7'd0;
    reg [7:0]  d0 = 8'd0, d1 = 8'd0;
    reg        tx_data = 1'b0, tx_lastsent = 1'b0;
    reg [5:0]  tmo = 6'd0;

    wire [6:0] wi = n - 7'd2;

    task send(input [3:0] p, input last, input crc);
        begin
            b_data <= {~p, p}; b_last <= last; b_crc <= crc; b_tog <= ~b_tog;
        end
    endtask

    reg [7:0] ev_set;
    reg       q_setup = 1'b0, q_out = 1'b0, q_inack = 1'b0, q_addr = 1'b0, q_reset = 1'b0;
    reg       se0_d = 1'b0, susp_d = 1'b0;

    always @(posedge clk) begin
        ev_set = 8'd0;
        o_we  <= 1'b0;
        q_setup <= 1'b0;
        q_out   <= 1'b0;
        q_inack <= 1'b0;
        q_reset <= 1'b0;

        if (ex_go) sr_ra <= {cq_q[15], cq_q[13:7]};

        case (lk)
            3'd1: begin
                m_hit <= sr_q[3];
                m_dev <= sr_q[2:0];
                m_ep0 <= (tok_ep == 4'd0);
                t_ra  <= {1'b0, sr_q[2:0], tok_ep, pid == P_IN};
                lk <= 3'd2;
            end
            3'd2: lk <= 3'd3;
            3'd3: begin
                if (m_ep0) begin
                    m_slot <= {pid != P_IN, 1'b0, m_dev};
                    sr_ra  <= {3'd0, pid != P_IN, 1'b0, m_dev};
                    m_ok   <= m_hit;
                    t_ra   <= {5'b10000, 1'b0, m_dev};
                end else begin
                    m_slot <= {pid != P_IN, t_q[3:0]};
                    sr_ra  <= {3'd0, pid != P_IN, t_q[3:0]};
                    m_ok   <= m_hit && t_q[7] && pid != P_SETUP;
                    t_ra   <= {5'b10000, t_q[3:0]};
                end
                lk <= 3'd4;
            end
            3'd4: lk <= 3'd5;
            3'd5: begin
                m_len   <= t_q[6:0];
                m_lenm1 <= t_q[5:0] - 1'b1;
                {m_en, m_stall, m_tog, m_full} <= sr_q;
                if (!m_ep0 && !sr_q[3]) m_ok <= 1'b0;
                lk_done <= 1'b1;
                lk <= 3'd0;
            end
            default: ;
        endcase

        case (st)
            S_IDLE:
                if (ev_start) st <= S_PID;
            S_PID:
                if (ev_byte) begin
                    pid <= ev_data[3:0];
                    if (ev_data[7:4] != ~ev_data[3:0]) st <= S_SKIP;
                    else case (ev_data[3:0])
                        P_OUT, P_IN, P_SETUP, P_SOF: begin
                            st <= S_TOK1;
                            x_setup <= 1'b0;
                            x_out   <= 1'b0;
                        end
                        P_DATA0, P_DATA1:
                            if (x_setup || x_out) begin
                                st   <= S_DATA;
                                n    <= 7'd0;
                                x_ovf <= 1'b0;
                                x_dtog <= ev_data[3];
                            end else st <= S_SKIP;
                        default: begin
                            st <= S_SKIP;
                            x_setup <= 1'b0;
                            x_out   <= 1'b0;
                        end
                    endcase
                end else if (ev_end) st <= S_IDLE;
            S_TOK1:
                if (ev_byte) begin
                    tok1 <= ev_data;
                    sr_ra <= {1'b1, ev_data[6:0]};
                    st <= S_TOK2;
                end else if (ev_end) st <= S_IDLE;
            S_TOK2:
                if (ev_byte) begin
                    tok_a  <= tok1[6:0];
                    tok_ep <= {ev_data[2:0], tok1[7]};
                    if (pid != P_SOF) begin
                        lk <= 3'd1;
                        lk_done <= 1'b0;
                    end
                    st <= S_TOKEND;
                end else if (ev_end) st <= S_IDLE;
            S_TOKEND:
                if (ev_byte) st <= S_SKIP;
                else if (ev_end) begin
                    if (!f_crc5 || f_err) st <= S_IDLE;
                    else if (pid == P_SOF) begin
                        frame <= {tok_ep, tok_a};
                        irq_lat[3] <= 1'b1;
                        ev_set[3] = 1'b1;
                        st <= S_IDLE;
                    end else st <= S_DECIDE;
                end
            S_DECIDE:
                if (lk_done) begin
                    x_dev  <= m_dev;
                    x_slot <= m_slot;
                    x_ep0  <= m_ep0;
                    x_zlp  <= (m_len == 7'd0);
                    st <= S_IDLE;
                    if (m_ok) case (pid)
                        P_IN: begin
                            st <= S_TX;
                            tx_data <= !m_stall && m_full;
                            tx_lastsent <= 1'b0;
                            n <= 7'd0;
                            i_ra <= {m_slot[3:0], 6'd0};
                            if (m_stall)      send(P_STALL, 1'b1, 1'b0);
                            else if (!m_full) send(P_NAK, 1'b1, 1'b0);
                            else              send(m_tog ? P_DATA1 : P_DATA0, m_len == 7'd0, 1'b1);
                        end
                        P_SETUP: begin
                            x_setup <= 1'b1;
                            x_wok   <= 1'b1;
                        end
                        default: begin
                            x_out <= 1'b1;
                            x_wok <= !m_stall && !m_full;
                        end
                    endcase
                end
            S_DATA:
                if (ev_byte) begin
                    d0 <= ev_data;
                    d1 <= d0;
                    if (n != 7'h7F) n <= n + 1'b1;
                    if (n >= 7'd2) begin
                        if (x_setup ? (wi >= 7'd8) : (wi >= 7'd64)) x_ovf <= 1'b1;
                        else if (x_wok) begin
                            o_we <= 1'b1;
                            o_wd <= d1;
                            o_wa <= x_setup ? {1'b1, 4'b0000, x_dev, wi[2:0]}
                                            : {1'b0, x_slot[3:0], wi[5:0]};
                        end
                    end
                end else if (ev_end) begin
                    st <= S_IDLE;
                    if (f_crc16 && !f_err && n >= 7'd2 && !x_ovf) st <= S_DEND;
                    else begin
                        x_setup <= 1'b0;
                        x_out   <= 1'b0;
                    end
                end
            S_DEND: begin
                x_setup <= 1'b0;
                x_out   <= 1'b0;
                tx_data <= 1'b0;
                st <= S_IDLE;
                if (x_setup) begin
                    if (n == 7'd10 && !x_dtog) begin
                        send(P_ACK, 1'b1, 1'b0);
                        st <= S_TX;
                        q_setup <= 1'b1;
                        ev_set[5] = 1'b1;
                    end
                end else begin
                    st <= S_TX;
                    if (m_stall)            send(P_STALL, 1'b1, 1'b0);
                    else if (!x_wok)        send(P_NAK, 1'b1, 1'b0);
                    else begin
                        send(P_ACK, 1'b1, 1'b0);
                        if (x_dtog == m_tog) begin
                            q_out <= 1'b1;
                            ev_set[7] = 1'b1;
                            o_we <= 1'b1;
                            o_wd <= {1'b0, wi};
                            o_wa <= {1'b1, 4'b0001, 2'b00, x_slot[3:0]};
                        end
                    end
                end
            end
            S_TX:
                if (taken) begin
                    if (!tx_data || (x_zlp && n == 7'd0) || tx_lastsent) st <= S_TXW;
                    else begin
                        b_data <= i_q;
                        b_last <= (n[5:0] == m_lenm1);
                        b_tog  <= ~b_tog;
                        tx_lastsent <= (n[5:0] == m_lenm1);
                        n    <= n + 1'b1;
                        i_ra <= {x_slot[3:0], n[5:0] + 6'd1};
                    end
                end
            S_TXW:
                if (txend) begin
                    tmo <= 6'd0;
                    st  <= tx_data ? S_ACKW : S_IDLE;
                end
            S_ACKW: begin
                tmo <= tmo + 1'b1;
                if (ev_start) st <= S_ACKPID;
                else if (tmo == 6'd63) st <= S_IDLE;     // no handshake: stays armed
            end
            S_ACKPID:
                if (ev_byte) st <= (ev_data == {~P_ACK, P_ACK}) ? S_ACKEND : S_SKIP;
                else if (ev_end) st <= S_IDLE;
            S_ACKEND:
                if (ev_byte) st <= S_SKIP;
                else if (ev_end) begin
                    st <= S_IDLE;
                    if (!f_err) begin
                        q_inack <= 1'b1;
                        ev_set[6] = 1'b1;
                        q_addr <= x_ep0 && x_zlp && p_on && p_dev == x_dev;
                    end
                end
            S_SKIP:
                if (ev_end) st <= S_IDLE;
            default: st <= S_IDLE;
        endcase
        if (ev_start && st != S_IDLE && st != S_ACKW) st <= S_PID;

        se0_d <= in_reset;
        if (in_reset && !se0_d) begin
            q_reset <= 1'b1;
            irq_lat[0] <= 1'b1;
            ev_set[0] = 1'b1;
        end
        if (in_reset || !ctrl[1] || rst) begin
            st <= S_IDLE;
            lk <= 3'd0;
            x_setup <= 1'b0;
            x_out   <= 1'b0;
        end
        if (line_s != 2'b10) idle_cnt <= 18'd0;
        else if (!idle_cnt[17]) idle_cnt <= idle_cnt + 1'b1;
        suspended <= idle_cnt[17];
        susp_d <= suspended;
        if (suspended && !susp_d) begin irq_lat[1] <= 1'b1; ev_set[1] = 1'b1; end
        if (!suspended && susp_d) begin irq_lat[2] <= 1'b1; ev_set[2] = 1'b1; end

        // After the engine, so the ESP32's writes win
        if (wr && hit) begin
            casez (addr[7:0])
                8'h00: ctrl <= wdata[1:0];
                8'h04: irq_lat <= irq_lat & ~wdata[3:0];
                8'h05: irq_en <= wdata;
                default: ;
            endcase
        end
        irq_pulse <= |(ev_set & irq_en);
    end

    wire lw    = wr && hit;

    reg  [8:0] rs_n = 9'd0;
    reg        setup2 = 1'b0, addr2 = 1'b0;
    reg  [1:0] ex = 2'd0;
    reg  [15:0] ex_c = 16'd0;
    assign     ex_go = cq_ne && ex == 2'd0 && st == S_IDLE && !x_out && !x_setup &&
                       !in_reset && rs_n == 9'd0;
    wire [3:0] ex_new = ex_c[15] ? ex_c[3:0] :
                        ex_c[14] ? {sr_q[3:1], 1'b1} :
                        {ex_c[0] ? 1'b1 : ex_c[1] ? 1'b0 : sr_q[3],
                         ex_c[2] ? 1'b1 : ex_c[3] ? 1'b0 : sr_q[2],
                         ex_c[4] ? 1'b0 : ex_c[5] ? 1'b1 : sr_q[1],
                         ex_c[6] ? 1'b0 : sr_q[0]};
    always @(posedge clk) begin
        sr_we <= 1'b0;
        if (q_reset) rs_n <= 9'd256;
        setup2 <= q_setup;
        addr2  <= q_inack && q_addr;
        if (rs_n != 9'd0) begin
            sr_we <= 1'b1; sr_wa <= rs_n[7:0] - 1'b1; sr_wd <= 4'd0;
            rs_n  <= rs_n - 1'b1;
        end else if (q_setup || setup2) begin
            sr_we <= 1'b1; sr_wa <= {3'd0, setup2, 1'b0, x_dev}; sr_wd <= 4'b0010;
        end else if (q_out || q_inack) begin
            sr_we <= 1'b1; sr_wa <= {3'd0, x_slot}; sr_wd <= {m_en, m_stall, ~m_tog, q_out};
        end else if (addr2) begin
            sr_we <= 1'b1; sr_wa <= {1'b1, p_addr}; sr_wd <= {1'b1, p_dev};
        end else if (ex == 2'd3) begin
            sr_we <= 1'b1; sr_wa <= {ex_c[15], ex_c[13:7]}; sr_wd <= ex_new;
            ex <= 2'd0;
        end
        if (lw && addr[7:0] == 8'h12) p_dev <= wdata[2:0];
        if (lw && addr[7:0] == 8'h13) p_addr <= wdata[6:0];
        if (lw && addr[7:0] == 8'h10) map_a <= wdata[6:0];
        if (q_reset || addr2) p_on <= 1'b0;
        if (lw && addr[7:0] == 8'h13) p_on <= 1'b1;
        if (ex_go) begin
            ex_c  <= cq_q;
            cq_rp <= cq_rp + 1'b1;
            ex    <= 2'd1;
        end else if (ex == 2'd1) ex <= 2'd2;
        else if (ex == 2'd2) ex <= 2'd3;
    end

    integer i;
    always @(posedge clk) begin
        if (lw && addr[7:0] == 8'h08) in_done[7:0]   <= in_done[7:0]   & ~wdata;
        if (lw && addr[7:0] == 8'h09) in_done[15:8]  <= in_done[15:8]  & ~wdata;
        if (lw && addr[7:0] == 8'h0A) out_done[7:0]  <= out_done[7:0]  & ~wdata;
        if (lw && addr[7:0] == 8'h0B) out_done[15:8] <= out_done[15:8] & ~wdata;
        if (lw && addr[7:0] == 8'h06) setup_dev <= setup_dev & ~wdata;
        for (i = 0; i < 16; i = i + 1) begin
            if (q_inack && x_slot[3:0] == i) in_done[i]  <= 1'b1;
            if (q_out   && x_slot[3:0] == i) out_done[i] <= 1'b1;
        end
        for (i = 0; i < 8; i = i + 1)
            if (q_setup && x_dev == i) setup_dev[i] <= 1'b1;
    end

    always @(posedge clk) begin
        casez (addr[7:0])
            8'h00: rdata <= {6'd0, ctrl};
            8'h01: rdata <= {4'd0, line_s, suspended, in_reset};
            8'h02: rdata <= frame[7:0];
            8'h03: rdata <= {5'd0, frame[10:8]};
            8'h04: rdata <= irq_view;
            8'h05: rdata <= irq_en;
            8'h06: rdata <= setup_dev;
            8'h08: rdata <= in_done[7:0];
            8'h09: rdata <= in_done[15:8];
            8'h0A: rdata <= out_done[7:0];
            8'h0B: rdata <= out_done[15:8];
            default: rdata <= 8'd0;
        endcase
    end
endmodule

`default_nettype wire
