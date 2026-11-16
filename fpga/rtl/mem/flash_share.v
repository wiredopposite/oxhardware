// Lends the BIOS flash to the ESP32. Once REQ is set and the console has gone QUIET_CLKS
// without a BIOS read, or is in reset, the streamer is held in reset and GRANT rises; the
// top then floats the flash pins. Clearing REQ hands them back at once. BIOS reads in the
// meantime get 0xFF and are counted in FLASH_MISS.
`default_nettype none

module flash_share #(
    parameter integer QUIET_CLKS = 24000
) (
    input  wire        clk,
    input  wire        wr,
    input  wire [15:0] addr,
    input  wire [7:0]  wdata,
    output reg  [7:0]  rdata = 8'd0,
    output reg         hit = 1'b0,
    input  wire        served_pulse,
    input  wire        lreset_n,
    output reg         grant = 1'b0,
    input  wire        lclk,
    output reg         hold_l = 1'b0,
    input  wire        req,
    output wire        ack,
    output wire [7:0]  ack_data,
    output wire        s_req,
    input  wire        s_ack,
    input  wire [7:0]  s_data
);
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

    reg [1:0] park_s = 2'b00;
    reg       ff_ack = 1'b0;
    always @(posedge lclk) begin
        park_s <= {park_s[0], park};
        hold_l <= park_s[1];
        ff_ack <= hold_l && req && !ff_ack;
        if (hold_l && req && !ff_ack) miss_tog <= ~miss_tog;
    end
    assign s_req    = req && !hold_l;
    assign ack      = hold_l ? ff_ack : s_ack;
    assign ack_data = hold_l ? 8'hFF : s_data;
endmodule

`default_nettype wire
