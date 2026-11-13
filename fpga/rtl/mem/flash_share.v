// Hands the BIOS flash to the ESP32 (docs/01-interface.md §5.5). The ESP32 sets REQ; once
// the console has gone QUIET_CLKS without a BIOS read (or LRESET# is asserted) the
// streamer is held in reset, which deselects the flash, and GRANT rises: the top floats
// its flash pins and the ESP32 drives them over the config bus. Clearing REQ gives the
// pins back at once (the streamer is still deselected) and the streamer starts over,
// waking the flash first.
//
// While the flash is away, BIOS reads are answered with 0xFF here and counted.
//
// Registers (system clock): 0xC0 FLASH_CTRL (bit0 REQ rw, bit1 GRANT ro),
// 0xC1 FLASH_MISS (BIOS reads answered 0xFF, saturating; a write clears it).
`default_nettype none

module flash_share #(
    parameter integer QUIET_CLKS = 24000        // 1 ms at 24 MHz
) (
    input  wire        clk,
    input  wire        wr,
    input  wire [15:0] addr,
    input  wire [7:0]  wdata,
    output reg  [7:0]  rdata = 8'd0,
    output reg         hit = 1'b0,
    input  wire        served_pulse,            // a BIOS read was served
    input  wire        lreset_n,                // synchronised
    output reg         grant = 1'b0,
    // LCLK domain: between lpc_target and spi_stream
    input  wire        lclk,
    output reg         hold_l = 1'b0,           // keep spi_stream in reset
    input  wire        req,
    output wire        ack,
    output wire [7:0]  ack_data,
    output wire        s_req,
    input  wire        s_ack,
    input  wire [7:0]  s_data
);
    // ---------------------------------------------------------------- system clock
    reg        want = 1'b0, park = 1'b0;
    reg [14:0] quiet = 15'd0;
    reg [1:0]  back_s = 2'b00, miss_s = 2'b00;
    reg        miss_d = 1'b0, miss_tog = 1'b0;
    reg [7:0]  misses = 8'd0;
    localparam [14:0] QUIET = QUIET_CLKS;
    wire       quiet_ok = quiet == QUIET;
    wire       miss_pulse = miss_s[1] ^ miss_d;

    always @(posedge clk) begin
        back_s <= {back_s[0], hold_l};
        miss_s <= {miss_s[0], miss_tog};
        miss_d <= miss_s[1];
        if (served_pulse)   quiet <= 15'd0;
        else if (!quiet_ok) quiet <= quiet + 1'b1;

        if (wr && addr == 16'h00C0) want <= wdata[0];
        if (!want) begin
            park  <= 1'b0;
            grant <= 1'b0;
        end else begin
            if (quiet_ok || !lreset_n) park <= 1'b1;
            // the streamer is in reset (or there is no LPC clock to run it)
            if (park && (back_s[1] || !lreset_n)) grant <= 1'b1;
        end

        if (wr && addr == 16'h00C1)            misses <= 8'd0;
        else if (miss_pulse && misses != 8'hFF) misses <= misses + 1'b1;
    end

    always @(posedge clk) begin
        hit <= 1'b1;
        case (addr)
            16'h00C0: rdata <= {6'd0, grant, want};
            16'h00C1: rdata <= misses;
            default: begin rdata <= 8'h00; hit <= 1'b0; end
        endcase
    end

    // ---------------------------------------------------------------- LCLK
    reg [1:0] park_s = 2'b00;
    reg       ff_ack = 1'b0;
    always @(posedge lclk) begin
        park_s <= {park_s[0], park};
        hold_l <= park_s[1];
        ff_ack <= hold_l && req && !ff_ack;
        if (hold_l && req && !ff_ack) miss_tog <= ~miss_tog;
    end
    // A streamer ack in flight as the hold starts is dropped (the request gets 0xFF), and
    // an 0xFF ack as it ends is dropped (the streamer serves the request)
    assign s_req    = req && !hold_l;
    assign ack      = hold_l ? ff_ack : s_ack;
    assign ack_data = hold_l ? 8'hFF : s_data;
endmodule

`default_nettype wire
