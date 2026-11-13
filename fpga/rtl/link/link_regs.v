// Link register space 0 (docs/01-interface.md §5): identity, scratch and LPC status.
// Registers outside the implemented set read 0x00 and ignore writes.
`default_nettype none

module link_regs #(
    parameter [7:0]  VER_MAJOR = 8'd0,
    parameter [7:0]  VER_MINOR = 8'd1,
    parameter [7:0]  VER_PATCH = 8'd0,
    parameter [7:0]  BOARD_ID  = 8'd0,
    parameter [15:0] FEATURES  = 16'h0000
) (
    input  wire        clk,
    input  wire        wr,
    input  wire [15:0] addr,
    input  wire [7:0]  wdata,
    output reg  [7:0]  rdata = 8'd0,
    // LPC status (synchronised by the caller)
    input  wire        lreset_n,
    input  wire        lclk_running,   // the console is powered
    input  wire        served_pulse,   // one clk per served memory read
    input  wire        armed           // a bank is armed for the next reset
);
    reg [7:0]  scratch [0:3];
    reg [7:0]  reset_count = 8'd0;
    reg [31:0] read_count = 32'd0;
    reg        lreset_q = 1'b0;
    reg        served_any = 1'b0;

    initial begin
        scratch[0] = 8'd0; scratch[1] = 8'd0; scratch[2] = 8'd0; scratch[3] = 8'd0;
    end

    always @(posedge clk) begin
        lreset_q <= lreset_n;
        if (lreset_q && !lreset_n) begin
            reset_count <= reset_count + 1'b1;
            read_count  <= 32'd0;
            served_any  <= 1'b0;
        end else if (served_pulse) begin
            read_count <= read_count + 1'b1;
            served_any <= 1'b1;
        end
        if (wr && addr[15:2] == 14'h0003) scratch[addr[1:0]] <= wdata;
    end

    always @(posedge clk) begin
        case (addr)
            16'h0000: rdata <= "O";
            16'h0001: rdata <= "X";
            16'h0002: rdata <= "F";
            16'h0003: rdata <= "G";
            16'h0004: rdata <= VER_MAJOR;
            16'h0005: rdata <= VER_MINOR;
            16'h0006: rdata <= VER_PATCH;
            16'h0007: rdata <= BOARD_ID;
            16'h0008: rdata <= FEATURES[7:0];
            16'h0009: rdata <= FEATURES[15:8];
            16'h000C, 16'h000D, 16'h000E, 16'h000F: rdata <= scratch[addr[1:0]];
            16'h0021: rdata <= {4'd0, armed, served_any, lclk_running, lreset_n};
            16'h0024: rdata <= reset_count;
            16'h002C: rdata <= read_count[7:0];
            16'h002D: rdata <= read_count[15:8];
            16'h002E: rdata <= read_count[23:16];
            16'h002F: rdata <= read_count[31:24];
            default:  rdata <= 8'd0;
        endcase
    end
endmodule

`default_nettype wire
