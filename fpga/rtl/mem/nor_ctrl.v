// NOR engine (system clock): the ESP32's access to the flash while the FPGA serves the
// BIOS from it (docs/01-interface.md §5.5). Reads into a 256-byte buffer, page program
// from it, 4 K / 64 K erase, CRC-32 of a range, and the JEDEC ID.
//
// Sharing the flash: BIOS reads (spi_stream, LCLK) always come first. The engine takes
// the pins only when the console has made no flash read for QUIET_CLKS, or has been in
// reset that long: it asks spi_stream to park (deselect) and drives the pins once it
// has. When a BIOS read arrives while the engine holds them, it lets go well within the
// LPC read's wait budget:
//   - reads (buffer, CRC) stop at the next byte and continue from there later;
//   - program data goes out in chunks of at most PROG_CHUNK bytes;
//   - a program or erase in progress is suspended (0x75) and resumed (0x7A) once the
//     console has gone quiet again. A suspend is never sent within RESUME_GAP of a resume.
// SCK = clk / 2 (12 MHz at 24 MHz), MISO sampled where SCK falls (as in spi_stream).
//
// Link registers 0xC0-0xDB: addresses and lengths are write-only and take effect byte by
// byte; NOR_OP and CRC_CTRL are ignored while busy. SPACE 3 port 3 is the buffer; each link transaction to it
// starts at offset 0.
`default_nettype none

module nor_ctrl #(
    parameter [23:0] PROTECT_END = 24'h020000,  // below: the bitstream, needs NOR_UNLOCK
    parameter integer QUIET_CLKS = 24000,        // 1 ms
    parameter integer TRES_CLKS  = 96,           // release from power-down, 3 us + margin
    parameter integer SUS_CLKS   = 24,           // after 0x75, then BUSY is polled
    parameter integer RESUME_GAP = 2400,         // 100 us
    parameter integer PROG_CHUNK = 32
) (
    input  wire        clk,
    // link register bus
    input  wire        wr,
    input  wire [15:0] addr,
    input  wire [7:0]  wdata,
    output reg  [7:0]  rdata = 8'd0,
    output reg         hit = 1'b0,
    // SPACE 3 port 3: the buffer
    input  wire        b_wr,                // wdata
    input  wire        b_rd,
    output wire [7:0]  b_rdata,             // the cycle after b_rd
    input  wire        b_idle,              // no transaction: offsets back to 0
    // sharing with spi_stream
    input  wire        lpc_req,             // LCLK domain: a BIOS read is waiting
    input  wire        lreset_n,            // synchronised
    output reg         park = 1'b0,         // to spi_stream: deselect and wait
    input  wire        parked,              // LCLK domain
    output reg         own = 1'b0,          // the engine drives the flash pins
    // flash
    output reg         f_cs_n = 1'b1,
    output reg         f_sck = 1'b0,
    output reg         f_mosi = 1'b0,
    input  wire        f_miso,
    // status
    output wire        busy,
    output reg         nor_done = 1'b0,     // pulses: IRQ_STATUS bits 9 and 8
    output reg         crc_done = 1'b0
);
    // ======================================================== sharing
    reg [1:0]  req_s = 2'b00, parked_s = 2'b00;
    reg [15:0] quiet = 16'd0, rst_quiet = 16'd0;
    wire       lpc_want = req_s[1] && lreset_n;
    wire       rst_long = (rst_quiet == QUIET_CLKS);     // console in reset long enough
    always @(posedge clk) begin
        req_s    <= {req_s[0], lpc_req};
        parked_s <= {parked_s[0], parked};
        if (lpc_want)                 quiet <= 16'd0;
        else if (quiet != QUIET_CLKS) quiet <= quiet + 1'b1;
        if (lreset_n)       rst_quiet <= 16'd0;
        else if (!rst_long) rst_quiet <= rst_quiet + 1'b1;
    end

    // ======================================================== state
    localparam [2:0] K_NONE = 3'd0, K_ERASE4 = 3'd1, K_ERASE64 = 3'd2, K_PROG = 3'd3,
                     K_READ = 3'd5, K_CRC = 3'd6;
    localparam [3:0] O_IDLE = 4'd0, O_ACQ = 4'd1, O_WAKE = 4'd2, O_TRES = 4'd3,
                     O_JEDEC = 4'd4, O_STEP = 4'd5, O_WREN = 4'd6, O_CMD = 4'd7,
                     O_POLL = 4'd8, O_SUS = 4'd9, O_SUSWAIT = 4'd10, O_RESUME = 4'd11,
                     O_RELEASE = 4'd12, O_FINISH = 4'd13;
    localparam [1:0] X_IDLE = 2'd0, X_LOAD = 2'd1, X_BITS = 2'd2, X_GAP = 2'd3;

    reg [3:0]  os = O_ACQ;                  // wake the flash and read the ID first
    reg [2:0]  kind = K_NONE;
    reg [23:0] op_addr = 24'd0, crc_addr = 24'd0, crc_len = 24'd0;
    reg [7:0]  op_len = 8'd0;               // bytes - 1
    reg [7:0]  unlock = 8'd0;
    reg [23:0] cur = 24'd0;                 // flash address of the next data byte
    reg [23:0] left = 24'd0;                // data bytes still to go
    reg [7:0]  bidx = 8'd0;                 // buffer index of the next data byte
    reg [5:0]  chunk = 6'd0;
    reg [23:0] jedec = 24'd0;
    reg        init_done = 1'b0, refused = 1'b0, suspended = 1'b0;
    reg [15:0] tmr = 16'd0;
    reg [15:0] since_resume = RESUME_GAP;
    assign busy = (os != O_IDLE);

    // Frame: one CS# assertion. hn header bytes from hdr, then data: the operation's own
    // (x_op: read into the buffer or the CRC, counted by 'left'; or programmed from the
    // buffer, counted by 'chunk'), or x_n status / ID bytes read into x_val.
    reg [1:0]  xs = X_IDLE;
    reg        sent = 1'b0, x_done = 1'b0, x_cut = 1'b0, x_op = 1'b0, cur_data = 1'b0;
    reg [31:0] hdr = 32'd0;
    reg [2:0]  hn = 3'd0;
    reg [1:0]  x_n = 2'd0;
    reg [7:0]  sh_tx = 8'd0, sh_rx = 8'd0, x_val = 8'd0;
    reg [3:0]  bit_n = 4'd0;
    reg        ph = 1'b0;
    reg [1:0]  gap = 2'd0;
    wire [7:0] v_in = {sh_rx[6:0], f_miso};
    wire       prog = x_op && kind == K_PROG;
    wire       last_data = x_op ? (prog ? chunk == 6'd1 : left == 24'd1) : x_n == 2'd1;
    wire       no_data   = x_op ? (prog ? chunk == 6'd0 : left == 24'd0) : x_n == 2'd0;
    wire       byte_end  = (xs == X_BITS) && !ph && bit_n == 4'd8;

    // CRC-32, bit-serial: data bytes are >= 17 clocks apart
    reg [31:0] crc = 32'hFFFFFFFF;
    reg [7:0]  crc_b = 8'd0;
    reg [3:0]  crc_n = 4'd0;

    // ======================================================== buffer
    reg  [7:0] bufm [0:255];
    reg  [7:0] b_wptr = 8'd0, b_rptr = 8'd0;
    reg        e_wr = 1'b0;
    reg  [7:0] e_wdata = 8'd0, e_waddr = 8'd0;
    reg  [7:0] buf_q = 8'd0;
    always @(posedge clk) begin
        if (e_wr)      bufm[e_waddr] <= e_wdata;
        else if (b_wr) bufm[b_wptr]  <= wdata;
        buf_q <= bufm[prog ? bidx : b_rptr];
        if (b_idle) begin
            b_wptr <= 8'd0;
            b_rptr <= 8'd0;
        end else begin
            if (b_wr) b_wptr <= b_wptr + 1'b1;
            if (b_rd) b_rptr <= b_rptr + 1'b1;
        end
    end
    assign b_rdata = buf_q;

    // ======================================================== engine
    task frame(input [31:0] h, input [2:0] n_h, input op_data, input [1:0] n);
        begin
            hdr <= h; hn <= n_h; x_op <= op_data; x_n <= n; sent <= 1'b1;
            f_cs_n <= 1'b0; x_cut <= 1'b0; xs <= X_LOAD;
        end
    endtask

    always @(posedge clk) begin
        x_done   <= 1'b0;
        e_wr     <= 1'b0;
        nor_done <= 1'b0;
        crc_done <= 1'b0;
        if (since_resume != RESUME_GAP) since_resume <= since_resume + 1'b1;
        if (crc_n != 4'd0) begin
            crc   <= {1'b0, crc[31:1]} ^ ((crc[0] ^ crc_b[0]) ? 32'hEDB88320 : 32'd0);
            crc_b <= {1'b0, crc_b[7:1]};
            crc_n <= crc_n - 1'b1;
        end

        // ---------------------------------------------------- bits
        case (xs)
        X_IDLE: ;
        X_LOAD: begin                               // SCK low, first bit of a byte
            cur_data <= (hn == 3'd0);
            if (hn != 3'd0) begin
                sh_tx  <= {hdr[30:24], 1'b0};
                f_mosi <= hdr[31];
                hdr    <= {hdr[23:0], 8'h00};
                hn     <= hn - 1'b1;
            end else begin
                sh_tx  <= {(prog ? buf_q[6:0] : 7'd0), 1'b0};
                f_mosi <= prog && buf_q[7];
                if (prog) bidx <= bidx + 1'b1;      // buf_q follows in 2 clocks
            end
            bit_n <= 4'd0;
            ph    <= 1'b1;
            xs    <= X_BITS;
        end
        X_BITS:
            if (ph) begin                           // SCK high
                f_sck <= 1'b1;
                bit_n <= bit_n + 1'b1;
                ph    <= 1'b0;
            end else begin                          // SCK falls: sample
                f_sck <= 1'b0;
                sh_rx <= v_in;
                if (bit_n != 4'd8) begin
                    f_mosi <= sh_tx[7];
                    sh_tx  <= {sh_tx[6:0], 1'b0};
                    ph     <= 1'b1;
                end else begin                      // byte complete
                    if (cur_data) begin
                        x_val <= v_in;
                        if (!x_op) x_n <= x_n - 1'b1;
                        else if (prog) chunk <= chunk - 1'b1;
                        else begin                  // read / CRC data byte
                            cur  <= cur + 1'b1;
                            left <= left - 1'b1;
                            if (kind == K_READ) begin
                                e_wr    <= 1'b1;
                                e_waddr <= bidx;
                                e_wdata <= v_in;
                                bidx    <= bidx + 1'b1;
                            end else begin
                                crc_b <= v_in;
                                crc_n <= 4'd8;
                            end
                        end
                    end
                    if (cur_data ? (last_data || (x_op && !prog && lpc_want))
                                 : (hn == 3'd0 && no_data)) begin
                        x_cut  <= cur_data && !last_data;
                        f_cs_n <= 1'b1;
                        gap    <= 2'd3;
                        xs     <= X_GAP;
                    end else
                        xs <= X_LOAD;
                end
            end
        X_GAP:                                      // CS# high >= 4 clocks
            if (gap != 2'd0) gap <= gap - 1'b1;
            else begin
                x_done <= 1'b1;
                xs     <= X_IDLE;
            end
        endcase
        if (x_done) sent <= 1'b0;
        if (byte_end && cur_data && os == O_JEDEC) jedec <= {jedec[15:0], v_in};

        // ---------------------------------------------------- operations
        case (os)
        O_IDLE: ;
        O_ACQ: begin                                // park only once the console is quiet
            park <= rst_long || (quiet == QUIET_CLKS && !lpc_want);
            if (rst_long || (park && parked_s[1] && !lpc_want)) begin
                own <= 1'b1;
                os  <= !init_done ? O_WAKE : suspended ? O_RESUME : O_STEP;
            end
        end
        O_WAKE:                                     // release from deep power-down
            if (!sent) frame({8'hAB, 24'd0}, 3'd1, 1'b0, 2'd0);
            else if (x_done) begin tmr <= TRES_CLKS; os <= O_TRES; end
        O_TRES:
            if (tmr != 16'd0) tmr <= tmr - 1'b1;
            else os <= O_JEDEC;
        O_JEDEC:
            if (!sent) frame({8'h9F, 24'd0}, 3'd1, 1'b0, 2'd3);
            else if (x_done) begin
                init_done <= 1'b1;
                os        <= (kind == K_NONE) ? O_FINISH : O_STEP;
            end
        O_STEP:                                     // the next piece of the operation
            if (sent) begin                         // a read frame: ended or cut short
                if (x_done) os <= x_cut ? O_RELEASE : O_FINISH;
            end else if (lpc_want)
                os <= O_RELEASE;
            else if (kind == K_READ || kind == K_CRC) begin
                if (left == 24'd0) os <= O_FINISH;
                else frame({8'h03, cur}, 3'd4, 1'b1, 2'd0);
            end else if (kind == K_PROG && left == 24'd0)
                os <= O_FINISH;
            else
                os <= O_WREN;
        O_WREN:
            if (!sent) frame({8'h06, 24'd0}, 3'd1, 1'b0, 2'd0);
            else if (x_done) os <= O_CMD;
        O_CMD:
            if (!sent) begin
                if (kind == K_PROG) begin
                    chunk <= (left[8:0] > PROG_CHUNK) ? PROG_CHUNK[5:0] : left[5:0];
                    frame({8'h02, cur}, 3'd4, 1'b1, 2'd0);
                end else
                    frame({(kind == K_ERASE4) ? 8'h20 : 8'hD8, cur}, 3'd4, 1'b0, 2'd0);
            end else if (x_done) os <= O_POLL;
        O_POLL:                                     // status 1 until BUSY clears
            if (!sent) frame({8'h05, 24'd0}, 3'd1, 1'b0, 2'd1);
            else if (x_done) begin
                if (!x_val[0]) begin
                    if (kind == K_PROG) begin       // a chunk is in (all full but the last)
                        // programs stay within one page: 9-bit arithmetic is enough
                        cur[7:0]  <= cur[7:0] + PROG_CHUNK[7:0];
                        left[8:0] <= (left[8:0] > PROG_CHUNK) ? left[8:0] - PROG_CHUNK[8:0]
                                                              : 9'd0;
                        os        <= O_STEP;
                    end else
                        os <= O_FINISH;
                end else if (lpc_want && since_resume == RESUME_GAP)
                    os <= O_SUS;
            end
        O_SUS:
            if (!sent) frame({8'h75, 24'd0}, 3'd1, 1'b0, 2'd0);
            else if (x_done) begin tmr <= SUS_CLKS; os <= O_SUSWAIT; end
        O_SUSWAIT:                                  // BUSY reads 0 once suspended (<= tSUS)
            if (tmr != 16'd0) tmr <= tmr - 1'b1;
            else if (!sent) frame({8'h05, 24'd0}, 3'd1, 1'b0, 2'd1);
            else if (x_done && !x_val[0]) begin
                suspended <= 1'b1;
                os        <= O_RELEASE;
            end
        O_RESUME:
            if (!sent) frame({8'h7A, 24'd0}, 3'd1, 1'b0, 2'd0);
            else if (x_done) begin
                suspended    <= 1'b0;
                since_resume <= 16'd0;
                os           <= O_POLL;
            end
        O_RELEASE: begin                            // give the pins back, take them later
            own  <= 1'b0;
            park <= 1'b0;
            if (!own && (!parked_s[1] || !lreset_n)) os <= O_ACQ;
        end
        O_FINISH: begin
            own  <= 1'b0;
            park <= 1'b0;
            if (crc_n == 4'd0) begin                // the last CRC byte folded in
                crc_done <= (kind == K_CRC);
                nor_done <= (kind != K_CRC && kind != K_NONE);
                os       <= O_IDLE;
            end
        end
        default: os <= O_IDLE;
        endcase

        // ---------------------------------------------------- link writes
        if (wr) case (addr)
            16'h00C8: op_addr[7:0]   <= wdata;
            16'h00C9: op_addr[15:8]  <= wdata;
            16'h00CA: op_addr[23:16] <= wdata;
            16'h00CB: if (os == O_IDLE) begin
                unlock  <= 8'd0;
                refused <= 1'b0;
                if ((wdata != 8'd1 && wdata != 8'd2 && wdata != 8'd3 && wdata != 8'd5) ||
                    (wdata != 8'd5 && op_addr < PROTECT_END && unlock != 8'h5A) ||
                    (wdata == 8'd3 && {1'b0, op_addr[7:0]} + op_len > 9'd255)) begin
                    refused  <= 1'b1;
                    nor_done <= 1'b1;
                end else begin
                    kind <= wdata[2:0];
                    cur  <= op_addr;
                    left <= {16'd0, op_len} + 1'b1;
                    bidx <= 8'd0;
                    os   <= O_ACQ;
                end
            end
            16'h00CC: op_len <= wdata;
            16'h00CD: unlock <= wdata;
            16'h00D0: crc_addr[7:0]   <= wdata;
            16'h00D1: crc_addr[15:8]  <= wdata;
            16'h00D2: crc_addr[23:16] <= wdata;
            16'h00D3: crc_len[7:0]    <= wdata;
            16'h00D4: crc_len[15:8]   <= wdata;
            16'h00D5: crc_len[23:16]  <= wdata;
            16'h00D6: if (wdata[0] && os == O_IDLE) begin
                kind <= K_CRC;
                cur  <= crc_addr;
                left <= crc_len;
                crc  <= 32'hFFFFFFFF;
                os   <= O_ACQ;
            end
            default: ;
        endcase
    end

    // ---------------------------------------------------- link reads
    wire [31:0] crc_result = ~crc;          // valid from CRC_DONE until the next CRC
    always @(posedge clk) begin
        hit <= 1'b1;
        case (addr)
            16'h00C0: rdata <= {2'b00, refused, kind == K_CRC && busy, suspended, busy,
                                1'b0, init_done};
            16'h00C2: rdata <= jedec[23:16];
            16'h00C3: rdata <= jedec[15:8];
            16'h00C4: rdata <= jedec[7:0];
            16'h00D8: rdata <= crc_result[7:0];
            16'h00D9: rdata <= crc_result[15:8];
            16'h00DA: rdata <= crc_result[23:16];
            16'h00DB: rdata <= crc_result[31:24];
            default: begin rdata <= 8'h00; hit <= 1'b0; end
        endcase
    end
endmodule

`default_nettype wire
