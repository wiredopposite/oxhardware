// Passive LPC cycle sniffer. Each cycle's type, address and data byte is published with a
// toggle; cycles are at least ~10 LCLKs apart, so one holding register is enough.
//
//   write:  START CT A.. D[3:0] D[7:4] TAR TAR SYNC.. TAR TAR
//   read:   START CT A.. TAR TAR SYNC.. D[3:0] D[7:4] TAR TAR
// SYNC 0000 = ready, 0101/0110 = short/long wait, 1010 = error. If nothing answers the
// bus floats to 1111; after 3 such SYNC clocks the cycle is recorded as "no response".
// Writes are recorded once the data is seen, without their SYNC. Firmware hub, DMA and
// bus master cycles are recorded without data.
`default_nettype none

module lpc_sniff (
    input  wire        lclk,
    input  wire        lreset_n,
    input  wire        lframe_n,
    input  wire [3:0]  lad,
    output reg  [3:0]  rec_start,
    output reg  [3:0]  rec_ct,
    output reg  [31:0] rec_addr,
    output reg  [7:0]  rec_data,
    output reg         rec_err,
    output reg         rec_toggle = 1'b0,
    output wire        lclk_div16
);
    localparam [2:0] S_IDLE = 3'd0, S_CT = 3'd1, S_ADDR = 3'd2, S_WDATA = 3'd3,
                     S_TAR = 3'd4, S_SYNC = 3'd5, S_RDATA = 3'd6;

    reg [3:0] lad_q;
    reg       lframe_q = 1'b1, lrst_q = 1'b0;
    always @(posedge lclk) begin
        lad_q    <= lad;
        lframe_q <= lframe_n;
        lrst_q   <= lreset_n;
    end

    reg [4:0] div = 0;
    always @(posedge lclk) div <= div + 1'b1;
    assign lclk_div16 = div[4];

    reg [2:0]  state = S_IDLE;
    reg [3:0]  start, ct, nib_left;
    reg [31:0] addr;
    reg [3:0]  dlo;
    reg [1:0]  cnt;
    reg        err;

    wire tgt_write = (start == 4'b0000) && (ct[3] == 1'b0) && ct[1];
    wire tgt_read  = (start == 4'b0000) && (ct[3] == 1'b0) && !ct[1];

    always @(posedge lclk) begin
        if (!lrst_q) begin
            state <= S_IDLE;
        end else if (!lframe_q) begin
            start <= lad_q;
            state <= S_CT;
        end else begin
            case (state)
            S_CT: begin
                ct   <= lad_q;
                addr <= 32'd0;
                if (start == 4'b0000) begin
                    if (lad_q[3:2] == 2'b00) begin
                        nib_left <= 4'd4;
                        state    <= S_ADDR;
                    end else if (lad_q[3:2] == 2'b01) begin
                        nib_left <= 4'd8;
                        state    <= S_ADDR;
                    end else begin
                        rec_start  <= start;
                        rec_ct     <= lad_q;
                        rec_addr   <= 32'd0;
                        rec_data   <= 8'h00;
                        rec_err    <= 1'b0;
                        rec_toggle <= ~rec_toggle;
                        state      <= S_IDLE;
                    end
                end else if (start == 4'b1101 || start == 4'b1110) begin
                    nib_left <= 4'd7;
                    state    <= S_ADDR;
                end else if (start == 4'b1111) begin
                    state <= S_IDLE;
                end else begin
                    rec_start  <= start;
                    rec_ct     <= lad_q;
                    rec_addr   <= 32'd0;
                    rec_data   <= 8'h00;
                    rec_err    <= 1'b0;
                    rec_toggle <= ~rec_toggle;
                    state      <= S_IDLE;
                end
            end
            S_ADDR: begin
                addr     <= {addr[27:0], lad_q};
                nib_left <= nib_left - 1'b1;
                if (nib_left == 4'd1) begin
                    if (tgt_write) begin
                        state <= S_WDATA;
                        cnt   <= 2'd0;
                    end else if (tgt_read) begin
                        state <= S_TAR;
                        cnt   <= 2'd0;
                    end else begin
                        rec_start  <= start;
                        rec_ct     <= ct;
                        rec_addr   <= {addr[27:0], lad_q};
                        rec_data   <= 8'h00;
                        rec_err    <= 1'b0;
                        rec_toggle <= ~rec_toggle;
                        state      <= S_IDLE;
                    end
                end
            end
            S_WDATA:
                if (cnt == 2'd0) begin
                    dlo <= lad_q;
                    cnt <= 2'd1;
                end else begin
                    rec_start  <= start;
                    rec_ct     <= ct;
                    rec_addr   <= addr;
                    rec_data   <= {lad_q, dlo};
                    rec_err    <= 1'b0;
                    rec_toggle <= ~rec_toggle;
                    state      <= S_IDLE;
                end
            S_TAR:
                if (cnt == 2'd1) begin
                    state <= S_SYNC;
                    cnt   <= 2'd0;
                end else
                    cnt <= cnt + 1'b1;
            S_SYNC:
                case (lad_q)
                    4'b0000: begin err <= 1'b0; cnt <= 2'd0; state <= S_RDATA; end
                    4'b1010: begin err <= 1'b1; cnt <= 2'd0; state <= S_RDATA; end
                    4'b1111:
                        if (cnt == 2'd2) begin
                            rec_start  <= start;
                            rec_ct     <= ct;
                            rec_addr   <= addr;
                            rec_data   <= 8'hFF;
                            rec_err    <= 1'b1;
                            rec_toggle <= ~rec_toggle;
                            state      <= S_IDLE;
                        end else
                            cnt <= cnt + 1'b1;
                    default: ;
                endcase
            S_RDATA:
                if (cnt == 2'd0) begin
                    dlo <= lad_q;
                    cnt <= 2'd1;
                end else begin
                    rec_start  <= start;
                    rec_ct     <= ct;
                    rec_addr   <= addr;
                    rec_data   <= {lad_q, dlo};
                    rec_err    <= err;
                    rec_toggle <= ~rec_toggle;
                    state      <= S_IDLE;
                end
            default: ;
            endcase
        end
    end
endmodule

`default_nettype wire
