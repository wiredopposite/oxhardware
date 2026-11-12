// Bank control (system clock): which bank the console is served, one-shot armed boots,
// boot flags and IRQs (docs/06-boot-flow.md §3-4, docs/01-interface.md §5.1-5.2).
//
// At every LRESET# assertion: armed -> ACTIVE_BANK <= PENDING_BANK, ARMED_BOOT set,
// TRIAL <= the pending trial; otherwise ACTIVE_BANK <= DEFAULT_BANK and both clear.
// The arm, the pending trial and CONFIRMED clear either way. While LRESET# is held low
// (console off, or mid-reset) an arm takes effect at once, so it also covers power-on,
// which brings no falling edge. TRIAL_CONFIRMED (a trial
// boot confirmed) survives the reset until the ESP32 writes 0x3A with bit6 set.
//
// Link registers answered here (others read 0 with hit low):
//   0x10-0x13 IRQ_STATUS (W1C) / IRQ_ENABLE   0x20 LPC_CTRL   0x22 ACTIVE_BANK
//   0x23 PENDING_BANK   0x27 DEFAULT_BANK   0x28-0x2A LAST_MEM_ADDR
//   0x38-0x39 BOOTCTL_BASE (commits on 0x39)   0x3A BOOT_FLAGS   0x3B-0x3D USER/SYSTEM/DIAG
//
// Console writes to the boot control port arrive as a toggle with a port offset and data
// (from bootctl_port, LCLK domain), held stable until the next write.
`default_nettype none

module bank_ctrl #(
    parameter [7:0]  LPC_CTRL_INIT     = 8'h15,      // SERVE_EN, LFRAME_ABORT, IO_EN
    parameter [7:0]  DEFAULT_BANK_INIT = 8'h00,
    parameter [7:0]  ROM_BASE_64K      = 8'h04,      // ROM area in 64 KB units
    parameter [15:0] BOOTCTL_BASE_INIT = 16'h00E0
) (
    input  wire        clk,
    // link register bus (bus side of link_slave)
    input  wire        wr,
    input  wire [15:0] addr,
    input  wire [7:0]  wdata,
    output reg  [7:0]  rdata = 8'd0,
    output reg         hit = 1'b0,          // rdata is ours (registered with it)
    // LPC status (synchronised by the caller)
    input  wire        lreset_n,
    input  wire        served_pulse,
    input  wire [23:0] served_addr,         // stable for a while after served_pulse
    // console writes to the boot control port (LCLK domain, quasi-static)
    input  wire        cp_tog,
    input  wire [2:0]  cp_off,
    input  wire [7:0]  cp_data,
    // other IRQ sources: pulses, one per IRQ_STATUS bit
    input  wire [15:0] irq_set,
    // to the LPC side (quasi-static)
    output reg  [7:0]  lpc_ctrl = LPC_CTRL_INIT,
    output reg  [7:0]  bank_base = ROM_BASE_64K,   // served bank's flash address, 64 KB units
    output reg  [1:0]  bank_size = 2'd0,           // 0 = 256 K, 1 = 512 K, 2 = 1 M
    output reg  [15:0] bootctl_base = BOOTCTL_BASE_INIT,
    output wire [63:0] port_vals,                  // boot control port reads, +0 at [7:0]
    output wire        armed,
    output wire        irq                         // an enabled IRQ is pending
);
    localparam [7:0] NO_BANK = 8'hFF;

    reg [7:0]  active   = DEFAULT_BANK_INIT;
    reg [7:0]  pending  = DEFAULT_BANK_INIT;
    reg [7:0]  dflt     = DEFAULT_BANK_INIT;
    reg [7:0]  user_b   = NO_BANK, system_b = NO_BANK, diag_b = NO_BANK;
    reg        arm = 1'b0, trial_pend = 1'b0;
    reg        armed_boot = 1'b0, trial = 1'b0, last_failed = 1'b0, confirmed = 1'b0;
    reg        trial_ok = 1'b0;
    reg [15:0] irq_status = 16'd0, irq_enable = 16'd0;
    reg [7:0]  base_lo = BOOTCTL_BASE_INIT[7:0];
    reg [23:0] last_addr = 24'd0;

    wire [7:0] flags = {1'b0, trial_ok, trial_pend, confirmed, last_failed, trial, armed_boot,
                        arm};
    assign armed = arm;
    assign irq   = |(irq_status & irq_enable);
    assign port_vals = {active, diag_b, system_b, user_b, 8'h00,
                        arm ? pending : NO_BANK, flags, 8'h4F};

    // A bank value the FPGA can serve: SIZE 3 is not one
    function valid_bank(input [7:0] b);
        valid_bank = (b[7:6] != 2'b11);
    endfunction

    integer   i;
    reg       lreset_q = 1'b0;
    reg [2:0] cp_s = 3'd0;
    wire      lrst_fall = lreset_q && !lreset_n;
    wire      lrst_held = !lreset_q && !lreset_n;
    wire      lrst_rise = !lreset_q && lreset_n;
    wire      cp_wr = cp_s[2] ^ cp_s[1];

    always @(posedge clk) begin
        lreset_q <= lreset_n;
        cp_s     <= {cp_s[1:0], cp_tog};
        if (served_pulse) last_addr <= served_addr;

        // served bank, registered so the LPC side sees one clean value
        bank_base <= ROM_BASE_64K + {active[5:0], 2'b00};
        bank_size <= active[7:6];

        if (lrst_rise) irq_status[1] <= 1'b1;

        // ---------------------------------------------------- link writes
        if (wr) case (addr)
            16'h0010: irq_status[7:0]  <= irq_status[7:0]  & ~wdata;
            16'h0011: irq_status[15:8] <= irq_status[15:8] & ~wdata;
            16'h0012: irq_enable[7:0]  <= wdata;
            16'h0013: irq_enable[15:8] <= wdata;
            16'h0020: begin
                lpc_ctrl <= {wdata[7:4], 1'b0, wdata[2:0]};
                arm      <= wdata[3];
            end
            16'h0022: if (valid_bank(wdata)) active  <= wdata;
            16'h0023: if (valid_bank(wdata)) pending <= wdata;
            16'h0027: if (valid_bank(wdata)) dflt    <= wdata;
            16'h0038: base_lo <= wdata;
            16'h0039: bootctl_base <= {wdata, base_lo};
            16'h003A: begin
                trial_pend  <= wdata[2];
                last_failed <= wdata[3];
                if (wdata[6]) trial_ok <= 1'b0;
            end
            16'h003B: user_b   <= wdata;
            16'h003C: system_b <= wdata;
            16'h003D: diag_b   <= wdata;
            default: ;
        endcase

        // ---------------------------------------------------- console writes
        if (cp_wr) begin
            case (cp_off)
                3'd1: if (cp_data[3]) last_failed <= 1'b0;
                3'd2: begin
                    pending <= cp_data;
                    arm     <= valid_bank(cp_data);
                end
                3'd3: if (cp_data == 8'hA5) begin
                    confirmed <= 1'b1;
                    if (trial) trial_ok <= 1'b1;
                end
                default: ;
            endcase
            if (cp_off == 3'd2 || (cp_off == 3'd3 && cp_data == 8'hA5))
                irq_status[11] <= 1'b1;
        end

        for (i = 0; i < 16; i = i + 1)
            if (irq_set[i]) irq_status[i] <= 1'b1;

        // ---------------------------------------------------- reset: one-shot arm
        if (lrst_fall || (lrst_held && arm)) begin
            if (lrst_fall) irq_status[0] <= 1'b1;
            if (arm) begin
                active     <= pending;
                armed_boot <= 1'b1;
                trial      <= trial_pend;
            end else begin
                active     <= dflt;
                armed_boot <= 1'b0;
                trial      <= 1'b0;
            end
            arm        <= 1'b0;
            trial_pend <= 1'b0;
            confirmed  <= 1'b0;
        end
    end

    // ---------------------------------------------------- link reads
    always @(posedge clk) begin
        hit <= 1'b1;
        case (addr)
            16'h0010: rdata <= irq_status[7:0];
            16'h0011: rdata <= irq_status[15:8];
            16'h0012: rdata <= irq_enable[7:0];
            16'h0013: rdata <= irq_enable[15:8];
            16'h0020: rdata <= {lpc_ctrl[7:4], arm, lpc_ctrl[2:0]};
            16'h0022: rdata <= active;
            16'h0023: rdata <= pending;
            16'h0027: rdata <= dflt;
            16'h0028: rdata <= last_addr[7:0];
            16'h0029: rdata <= last_addr[15:8];
            16'h002A: rdata <= last_addr[23:16];
            16'h0038: rdata <= bootctl_base[7:0];
            16'h0039: rdata <= bootctl_base[15:8];
            16'h003A: rdata <= flags;
            16'h003B: rdata <= user_b;
            16'h003C: rdata <= system_b;
            16'h003D: rdata <= diag_b;
            default: begin rdata <= 8'h00; hit <= 1'b0; end
        endcase
    end
endmodule

`default_nettype wire
