// Behavioural SPI NOR model (W25Q-class subset) for simulation, mode 0:
//   0xAB release power-down (ignored)   0x03 READ   0x05 status 1 (bit0 BUSY, bit1 WEL)
//   0x06 write enable   0x9F JEDEC ID (EF 40 16)   0x02 page program
//   0x20 erase 4 K   0xD8 erase 64 K   0x75 suspend   0x7A resume
// Program and erase take T_PP / T_SE / T_BE; suspend takes effect T_SUS after 0x75.
// 'violations' counts accesses a real part would get wrong: reads or commands while busy,
// program/erase without WEL, a read of a sector whose erase is suspended.
// Contents: 1 MB, initialised to FF; tests load images into 'mem'.
`timescale 1ns/1ps
module spi_flash_model #(
    parameter real T_V   = 7.0,
    parameter real T_PP  = 20_000.0,
    parameter real T_SE  = 200_000.0,
    parameter real T_BE  = 400_000.0,
    parameter real T_SUS = 15_000.0
) (
    input  wire cs_n,
    input  wire sck,
    input  wire mosi,
    output reg  miso = 1'bz
);
    reg [7:0]  mem [0:20'hFFFFF];
    reg [7:0]  pbuf [0:255];
    reg [31:0] sh;
    integer    nbits, i, k;
    reg [7:0]  cmd;
    reg [23:0] addr;
    integer    bitn, nout, npb;
    reg [7:0]  obyte;
    reg        out_en;

    // array state
    reg        busy = 0, wel = 0, sus = 0;
    real       remain = 0, sus_t = 0;
    integer    op = 0;                  // 1 program, 2 erase 4 K, 3 erase 64 K
    reg [23:0] op_addr;
    integer    op_n;
    integer    violations = 0;
    integer    programs = 0, erases = 0, suspends = 0;

    initial for (i = 0; i <= 20'hFFFFF; i = i + 1) mem[i] = 8'hFF;

    function in_op_sector(input [23:0] a);
        in_op_sector = (op == 3) ? (a[23:16] == op_addr[23:16]) : (a[23:12] == op_addr[23:12]);
    endfunction

    task finish_op;
        begin
            if (op == 1)
                for (k = 0; k < op_n; k = k + 1)
                    mem[{op_addr[19:8], op_addr[7:0] + k[7:0]}] =
                        mem[{op_addr[19:8], op_addr[7:0] + k[7:0]}] & pbuf[k];
            else if (op == 2)
                for (k = 0; k < 4096; k = k + 1) mem[{op_addr[19:12], 12'd0} + k] = 8'hFF;
            else if (op == 3)
                for (k = 0; k < 65536; k = k + 1) mem[{op_addr[19:16], 16'd0} + k] = 8'hFF;
            busy = 0; op = 0;
        end
    endtask

    initial forever begin
        #100;
        if (busy && sus_t > 0) begin
            sus_t = sus_t - 100;
            if (sus_t <= 0) begin sus = 1; busy = 0; suspends = suspends + 1; end
        end
        if (busy) begin
            remain = remain - 100;
            if (remain <= 0) finish_op;
        end
    end

    task start_op(input integer kind, input real t);
        begin
            if (!wel) begin
                $display("FLASH: program/erase without WEL at %h", addr);
                violations = violations + 1;
            end else begin
                busy = 1; wel = 0; op = kind; op_addr = addr; remain = t;
                if (kind == 1) programs = programs + 1; else erases = erases + 1;
            end
        end
    endtask

    always @(negedge cs_n) begin
        nbits = 0; out_en = 0; npb = 0; cmd = 8'h00;
    end

    always @(posedge cs_n) begin
        miso <= #(T_V) 1'bz;
        out_en = 0;
        if (nbits >= 8 && nbits % 8 == 0) begin
            case (cmd)
            8'h06: if (!busy) wel = 1;
            8'h02: if (nbits >= 40) begin op_n = npb; start_op(1, T_PP); end
            8'h20: if (nbits == 32) start_op(2, T_SE);
            8'hD8: if (nbits == 32) start_op(3, T_BE);
            8'h75: if (busy && sus_t <= 0) sus_t = T_SUS;
            8'h7A: if (sus) begin sus = 0; busy = 1; sus_t = 0; end
            default: ;
            endcase
        end
    end

    always @(posedge sck) if (!cs_n) begin
        sh = {sh[30:0], mosi};
        nbits = nbits + 1;
        if (nbits == 8) begin
            cmd = sh[7:0];
            if (busy && cmd != 8'h05 && cmd != 8'h75) begin
                $display("FLASH: command %h while busy", cmd);
                violations = violations + 1;
            end
            if (cmd == 8'h05 || cmd == 8'h9F) begin out_en = 1; nout = 0; bitn = 7; end
        end
        if (nbits == 32) begin
            addr = sh[23:0];
            if (cmd == 8'h03) begin
                out_en = 1; bitn = 7;
                if (sus && in_op_sector(addr)) begin
                    $display("FLASH: read of the suspended sector %h", addr);
                    violations = violations + 1;
                end
            end
        end
        if (cmd == 8'h02 && nbits > 32 && nbits % 8 == 0) begin
            if (npb < 256) pbuf[npb] = sh[7:0];
            npb = npb + 1;
        end
    end

    always @(negedge sck) if (!cs_n && out_en) begin
        case (cmd)
            8'h03:   obyte = busy ? 8'hxx : mem[addr[19:0]];
            8'h05:   obyte = {6'd0, wel, busy};
            8'h9F:   obyte = (nout % 3 == 0) ? 8'hEF : (nout % 3 == 1) ? 8'h40 : 8'h16;
            default: obyte = 8'hFF;
        endcase
        miso <= #(T_V) obyte[bitn];
        if (bitn == 0) begin
            bitn = 7; nout = nout + 1;
            if (cmd == 8'h03) addr = addr + 1;
        end else bitn = bitn - 1;
    end
endmodule
