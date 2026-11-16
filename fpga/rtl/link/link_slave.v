// ESP32 link, SPI mode 0 slave. See docs/dev/link.md.
//
//   MOSI | CMD | A[23:16] | A[15:8] | A[7:0] | data ...            (write)
//   MOSI | CMD | A[23:16] | A[15:8] | A[7:0] | dummy | 0x00 ...    (read)
//   MISO | STS | 0x00 ...                    | 0x00  | data ...
//
// Bits are shifted on SCK with CS# as an asynchronous reset, and each byte is handed to
// the system clock with a toggle. For reads the next byte has to be in tx_hold by the
// following byte boundary, which needs the system clock at about 4x the byte rate.
//
// Reads are fetched two bytes ahead of the one being sent. rd_sent marks each byte that
// was actually sent, so a FIFO can take back fetches a transaction ended without sending.
`default_nettype none

module link_slave (
    input  wire        clk,
    input  wire        rst,
    input  wire        sck,
    input  wire        cs_n,
    input  wire        mosi,
    output wire        miso,
    output wire        miso_oe,
    input  wire [7:0]  sts,
    output reg         wr = 1'b0,
    output reg         rd = 1'b0,
    output reg  [2:0]  space = 3'd0,
    output reg  [23:0] addr = 24'd0,
    output reg  [7:0]  wdata = 8'd0,
    input  wire [7:0]  rdata,
    output reg         rd_sent = 1'b0,
    output wire        idle
);
    reg [2:0] bit_cnt = 3'd0;
    reg [6:0] rx_sh = 7'd0;
    reg [7:0] rx_byte = 8'd0;
    reg       byte_tog = 1'b0;
    always @(posedge sck or posedge cs_n) begin
        if (cs_n) begin
            bit_cnt <= 3'd0;
        end else begin
            bit_cnt <= bit_cnt + 1'b1;
            rx_sh   <= {rx_sh[5:0], mosi};
            if (bit_cnt == 3'd7) begin
                rx_byte  <= {rx_sh, mosi};
                byte_tog <= ~byte_tog;
            end
        end
    end

    // tx_hold is taken on the edge that completes a byte, before the system clock can
    // react to it, so the timing is the same at any SCK rate.
    reg [7:0] tx_hold = 8'd0;
    reg [7:0] tx_next = 8'd0;
    always @(posedge sck) if (!cs_n && bit_cnt == 3'd7) tx_next <= tx_hold;

    reg [7:0] tx_sh = 8'd0;
    reg       started = 1'b0;
    always @(negedge sck or posedge cs_n) begin
        if (cs_n) begin
            started <= 1'b0;
        end else begin
            started <= 1'b1;
            if (bit_cnt == 3'd0)  tx_sh <= tx_next;
            else if (!started)    tx_sh <= {sts[6:0], 1'b0};
            else                  tx_sh <= {tx_sh[6:0], 1'b0};
        end
    end
    assign miso    = started ? tx_sh[7] : sts[7];
    assign miso_oe = !cs_n;

    // CS# goes through one more stage than the byte toggle, so a byte that completes just
    // before CS# rises is still processed.
    reg [3:0] cs_s = 4'hF;
    reg [2:0] tog_s = 3'd0;
    always @(posedge clk) begin
        cs_s  <= {cs_s[2:0], cs_n};
        tog_s <= {tog_s[1:0], byte_tog};
    end
    wire active = !cs_s[3];
    assign idle = !active;
    wire got    = tog_s[2] ^ tog_s[1];

    reg [2:0] idx = 3'd0;
    reg       is_rd = 1'b0, noinc = 1'b0, rd_pend = 1'b0;
    always @(posedge clk) begin
        wr <= 1'b0;
        rd <= 1'b0;
        rd_sent <= 1'b0;
        rd_pend <= rd;
        if (rd_pend) tx_hold <= rdata;
        if (rst || !active) begin
            idx     <= 3'd0;
            tx_hold <= 8'd0;
            rd_pend <= 1'b0;
        end else if (got) begin
            if (idx != 3'd5) idx <= idx + 1'b1;
            case (idx)
                3'd0: begin
                    is_rd <= rx_byte[7];
                    space <= rx_byte[6:4];
                    noinc <= rx_byte[3];
                end
                3'd1: addr[23:16] <= rx_byte;
                3'd2: addr[15:8]  <= rx_byte;
                3'd3: begin
                    addr[7:0] <= rx_byte;
                    rd <= is_rd;
                end
                default: begin
                    if (is_rd) begin
                        if (!noinc) addr <= addr + 1'b1;
                        rd_sent <= (idx == 3'd5);
                        rd <= 1'b1;
                    end else begin
                        wdata <= rx_byte;
                        wr <= 1'b1;
                    end
                    if (!is_rd && idx == 3'd5 && !noinc) addr <= addr + 1'b1;
                end
            endcase
        end
    end
endmodule

`default_nettype wire
