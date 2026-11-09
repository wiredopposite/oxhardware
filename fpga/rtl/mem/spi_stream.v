// Sequential SPI-flash byte streamer (single-bit SPI, mode 0, READ 0x03).
// SCK may pause high between bytes (allowed by W25Q-class parts).
//
// Runs in whatever clock domain it is given (lpc_serve: LCLK, 33.3 MHz); SCK = clk/2.
// After a (re)start at address A it keeps the flash selected and streams bytes A, A+1, ...
// into a 2-entry buffer, stopping SCK while the buffer is full. A requester asks for one
// byte at a time:
//   req + req_addr held until ack. ack pulses for one clock with ack_data.
//   - buffered byte at req_addr          -> ack next clock
//   - req_addr is a little ahead (< 8)   -> keep streaming, discard bytes until it arrives
//   - anything else                      -> restart the read at req_addr
// At power-up (and whenever 'rst' is asserted) it sends Release-Power-Down (0xAB) first:
// the iCE40 may leave the flash in deep power-down after configuration.
//
// Costs at SCK = 16.7 MHz: restart ~41 SCK (~2.5 us) to the first byte, then one byte per
// 8 SCK (~0.5 us).
`default_nettype none

module spi_stream (
    input  wire        clk,
    input  wire        rst,            // sync, active high: abort, deselect, re-wake
    // requester
    input  wire        req,
    input  wire [23:0] req_addr,
    output reg         ack,
    output reg  [7:0]  ack_data,
    output reg         restarted,      // pulse: a restart was needed (statistics)
    // flash pins
    output reg         spi_cs_n,
    output reg         spi_sck,
    output reg         spi_mosi,
    input  wire        spi_miso
);
    localparam [2:0] S_WAKE = 3'd0, S_WAKEGAP = 3'd1, S_IDLE = 3'd2, S_GAP = 3'd3,
                     S_CMD = 3'd4, S_DATA = 3'd5;

    reg [2:0]  state = S_WAKE;
    reg [31:0] tx;                 // command / address shifter (MSB first)
    reg [5:0]  nbits;              // bits left in the current command
    reg [7:0]  rx;
    reg [2:0]  rxbits;
    reg        phase;              // 0: SCK low (set MOSI), 1: SCK high
    reg [7:0]  gap;
    reg [23:0] next_addr;          // address of the byte currently being shifted in

    // 2-entry buffer: entry 0 = head
    reg        v0, v1;
    reg [23:0] a0, a1;
    reg [7:0]  d0, d1;

    wire        full     = v0 && v1;
    // How far ahead of the oldest available/in-flight byte the request is. The compare is
    // registered (one clock late) for timing; 'settle' blocks decisions for a clock after
    // anything that changes the buffer head (pop, restart) and after a new request.
    wire [23:0] base     = v0 ? a0 : next_addr;
    wire [23:0] ahead    = req_addr - base;
    reg         hit0, in_reach, settle, req_d;
    always @(posedge clk) begin
        hit0     <= v0 && (a0 == req_addr);
        in_reach <= (state == S_DATA || state == S_CMD || state == S_GAP) && (ahead < 24'd8);
        req_d    <= req;
    end
    wire do_req     = req && req_d && !ack && !settle;
    wire do_hit     = do_req && hit0;
    wire do_skip    = do_req && !hit0 && in_reach;
    wire can_rst    = (state == S_IDLE || state == S_CMD || state == S_DATA);
    wire do_restart = do_req && !hit0 && !in_reach && can_rst;
    wire pop        = do_hit || (do_skip && v0);

    // byte completes on this clock (falling SCK after the 8th bit)
    wire        byte_done = (state == S_DATA) && phase && (rxbits == 3'd7);
    wire [7:0]  byte_val  = {rx[6:0], spi_miso};

    always @(posedge clk) begin
        ack       <= 1'b0;
        restarted <= 1'b0;
        if (rst) begin
            settle   <= 1'b0;
            state    <= S_WAKE;
            spi_cs_n <= 1'b1;
            spi_sck  <= 1'b0;
            phase    <= 1'b0;
            v0 <= 1'b0; v1 <= 1'b0;
            gap      <= 8'd4;
            tx       <= {8'hAB, 24'd0};
            nbits    <= 6'd8;
        end else begin
            // ---------------------------------------------------------- requester
            // (buffer pops happen here; pushes below may land in the same clock)
            settle <= pop || do_restart || (byte_done && !v0);   // head changed
            if (do_hit) begin
                ack      <= 1'b1;
                ack_data <= d0;
            end
            if (pop) begin                        // hit, or discard a byte we've gone past
                v0 <= v1; a0 <= a1; d0 <= d1; v1 <= 1'b0;
            end
            if (do_restart) begin
                    // restart the read at req_addr
                    restarted <= 1'b1;
                    spi_cs_n  <= 1'b1;
                    spi_sck   <= 1'b0;
                    phase     <= 1'b0;
                    v0 <= 1'b0; v1 <= 1'b0;
                    tx        <= {8'h03, req_addr};
                    nbits     <= 6'd32;
                    next_addr <= req_addr;
                    gap       <= 8'd2;            // CS# high >= 2 clocks (60 ns)
                    state     <= S_GAP;
            end

            // ---------------------------------------------------------- SPI engine
            case (state)
            S_WAKE, S_CMD: begin                  // shift tx out, MSB first
                if (gap != 0 && state == S_WAKE) begin
                    gap <= gap - 1'b1;            // short CS# high before wake
                end else if (!(do_restart && state == S_CMD)) begin
                    spi_cs_n <= 1'b0;
                    if (!phase) begin
                        spi_mosi <= tx[31];
                        spi_sck  <= 1'b0;
                        phase    <= 1'b1;
                    end else begin
                        spi_sck <= 1'b1;
                        phase   <= 1'b0;
                        tx      <= {tx[30:0], 1'b0};
                        nbits   <= nbits - 1'b1;
                        if (nbits == 6'd1) begin
                            if (state == S_WAKE) begin
                                state <= S_WAKEGAP;
                                gap   <= 8'd120;      // tRES1 3 us
                            end else begin
                                state  <= S_DATA;
                                rxbits <= 3'd0;
                            end
                        end
                    end
                end
            end
            S_WAKEGAP: begin
                spi_sck <= 1'b0;
                if (phase) begin                  // finish the last SCK high phase
                    phase <= 1'b0;
                end else begin
                    spi_cs_n <= 1'b1;
                    if (gap != 0) gap <= gap - 1'b1;
                    else          state <= S_IDLE;
                end
            end
            S_IDLE: begin
                spi_cs_n <= 1'b1;
                spi_sck  <= 1'b0;
            end
            S_GAP: begin
                if (gap != 0) gap <= gap - 1'b1;
                else          state <= S_CMD;
            end
            S_DATA: begin
                if (!do_restart) begin
                    // SCK is high after the last address bit and between bytes. Falling
                    // edge: the flash shifts the next bit out. One clock (30 ns) later the
                    // rising edge, where MISO is sampled. A full buffer stalls with SCK high.
                    if (!phase) begin
                        if (!full || pop) begin
                            spi_sck <= 1'b0;
                            phase   <= 1'b1;
                        end
                    end else begin
                        spi_sck <= 1'b1;
                        phase   <= 1'b0;
                        rx      <= {rx[6:0], spi_miso};
                        rxbits  <= rxbits + 1'b1;
                        if (byte_done) begin
                            next_addr <= next_addr + 1'b1;
                            // push; account for a pop in the same clock (requester above)
                            if (pop) begin
                                if (v1) begin a0 <= a1; d0 <= d1; v0 <= 1'b1;
                                              a1 <= next_addr; d1 <= byte_val; v1 <= 1'b1; end
                                else    begin a0 <= next_addr; d0 <= byte_val; v0 <= 1'b1; end
                            end else if (!v0) begin
                                a0 <= next_addr; d0 <= byte_val; v0 <= 1'b1;
                            end else begin
                                a1 <= next_addr; d1 <= byte_val; v1 <= 1'b1;
                            end
                        end
                    end
                end
            end
            default: state <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
