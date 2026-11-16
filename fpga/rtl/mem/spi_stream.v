// Streams bytes from SPI flash with READ (0x03), SCK = clk/2. After a start at address A
// it keeps the flash selected and reads A, A+1, ... into a 2-byte buffer, pausing SCK
// high while the buffer is full. A request for the buffered byte is acked the next
// clock, one a few bytes ahead waits for the stream, and anything else restarts the read.
//
// MISO is sampled at the end of SCK's high phase rather than the start, which gives the
// flash two clocks from the falling edge and makes the read independent of placement.
//
// Release Power-Down (0xAB) is sent after reset, since the iCE40 can leave the flash
// powered down after configuration.
`default_nettype none

module spi_stream (
    input  wire        clk,
    input  wire        rst,
    input  wire        park,
    output reg         parked = 1'b0,
    input  wire        req,
    input  wire [23:0] req_addr,
    output reg         ack,
    output reg  [7:0]  ack_data,
    output reg         restarted,
    output reg         spi_cs_n,
    output reg         spi_sck,
    output reg         spi_mosi,
    input  wire        spi_miso
);
    localparam [2:0] S_WAKE = 3'd0, S_WAKEGAP = 3'd1, S_IDLE = 3'd2, S_GAP = 3'd3,
                     S_CMD = 3'd4, S_DATA = 3'd5;

    reg [2:0]  state = S_WAKE;
    reg [31:0] tx;
    reg [5:0]  nbits;
    reg [7:0]  rx;
    reg [2:0]  rxbits;
    reg        phase;
    reg        pend;
    reg [7:0]  gap;
    reg [23:0] next_addr;

    reg        v0, v1;
    reg [23:0] a0, a1;
    reg [7:0]  d0, d1;

    wire        full     = v0 && v1;
    // The compares are registered for timing, so 'settle' holds off a decision for a
    // clock after the buffer head changes.
    wire [23:0] base     = v0 ? a0 : next_addr;
    wire [23:0] ahead    = req_addr - base;
    reg         hit0, in_reach, settle, req_d;
    always @(posedge clk) begin
        hit0     <= v0 && (a0 == req_addr);
        in_reach <= (state == S_DATA || state == S_CMD || state == S_GAP) && (ahead < 24'd8);
        req_d    <= req;
    end
    // Parking waits for the wake sequence so the flash isn't left powered down
    reg [1:0] park_s = 2'b00;
    reg       hold = 1'b0;
    always @(posedge clk) begin
        park_s <= {park_s[0], park};
        hold   <= park_s[1] && state != S_WAKE && state != S_WAKEGAP;
        parked <= hold && spi_cs_n;
    end
    wire do_req     = req && req_d && !ack && !settle && !hold;
    wire do_hit     = do_req && hit0;
    wire do_skip    = do_req && !hit0 && in_reach;
    wire can_rst    = (state == S_IDLE || state == S_CMD || state == S_DATA);
    wire do_restart = do_req && !hit0 && !in_reach && can_rst;
    wire pop        = do_hit || (do_skip && v0);

    wire        byte_done = (state == S_DATA) && !phase && pend && (rxbits == 3'd7);
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
            pend     <= 1'b0;
            v0 <= 1'b0; v1 <= 1'b0;
            gap      <= 8'd4;
            tx       <= {8'hAB, 24'd0};
            nbits    <= 6'd8;
        end else if (hold) begin
            settle   <= 1'b0;
            state    <= S_IDLE;
            spi_cs_n <= 1'b1;
            spi_sck  <= 1'b0;
            phase    <= 1'b0;
            pend     <= 1'b0;
            v0 <= 1'b0; v1 <= 1'b0;
        end else begin
            settle <= pop || do_restart || (byte_done && !v0);
            if (do_hit) begin
                ack      <= 1'b1;
                ack_data <= d0;
            end
            if (pop) begin
                v0 <= v1; a0 <= a1; d0 <= d1; v1 <= 1'b0;
            end
            if (do_restart) begin
                    restarted <= 1'b1;
                    spi_cs_n  <= 1'b1;
                    spi_sck   <= 1'b0;
                    phase     <= 1'b0;
                    pend      <= 1'b0;
                    v0 <= 1'b0; v1 <= 1'b0;
                    tx        <= {8'h03, req_addr};
                    nbits     <= 6'd32;
                    next_addr <= req_addr;
                    gap       <= 8'd2;
                    state     <= S_GAP;
            end

            case (state)
            S_WAKE, S_CMD: begin
                if (gap != 0 && state == S_WAKE) begin
                    gap <= gap - 1'b1;
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
                                gap   <= 8'd120;      // tRES1, 3us
                            end else begin
                                state  <= S_DATA;
                                rxbits <= 3'd0;
                                pend   <= 1'b0;
                            end
                        end
                    end
                end
            end
            S_WAKEGAP: begin
                spi_sck <= 1'b0;
                if (phase) begin
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
                    // A completed byte takes its sampling clock on its own, so the full
                    // check sees the pushed entry.
                    if (!phase) begin
                        if (pend) begin
                            pend   <= 1'b0;
                            rx     <= {rx[6:0], spi_miso};
                            rxbits <= rxbits + 1'b1;
                        end
                        if (byte_done) begin
                            next_addr <= next_addr + 1'b1;
                            if (pop) begin
                                if (v1) begin a0 <= a1; d0 <= d1; v0 <= 1'b1;
                                              a1 <= next_addr; d1 <= byte_val; v1 <= 1'b1; end
                                else    begin a0 <= next_addr; d0 <= byte_val; v0 <= 1'b1; end
                            end else if (!v0) begin
                                a0 <= next_addr; d0 <= byte_val; v0 <= 1'b1;
                            end else begin
                                a1 <= next_addr; d1 <= byte_val; v1 <= 1'b1;
                            end
                        end else if (!full || pop) begin
                            spi_sck <= 1'b0;
                            phase   <= 1'b1;
                        end
                    end else begin
                        spi_sck <= 1'b1;
                        phase   <= 1'b0;
                        pend    <= 1'b1;
                    end
                end
            end
            default: state <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
