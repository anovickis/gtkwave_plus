// Small multi-module test design used by test-gw-netlist-query.c to
// exercise the slang-netlist subprocess integration end-to-end: 2+ levels
// of hierarchy, a continuous driver (assign), an always_ff and an
// always_comb block, a signal with multiple loads, and named port
// connections.

module counter #(
    parameter int WIDTH = 4
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic             en,
    output logic [WIDTH-1:0] count,
    output logic             at_max
);

  logic [WIDTH-1:0] count_q;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n)
      count_q <= '0;
    else if (en)
      count_q <= count_q + 1'b1;
  end

  assign count = count_q;

  always_comb begin
    at_max = (count_q == {WIDTH{1'b1}});
  end

endmodule


module top (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       enable,
    output logic [3:0] value,
    output logic       max_reached,
    output logic       max_reached_reg
);

  logic [3:0] internal_count;
  logic       internal_at_max;

  counter #(
      .WIDTH(4)
  ) u_counter (
      .clk    (clk),
      .rst_n  (rst_n),
      .en     (enable),
      .count  (internal_count),
      .at_max (internal_at_max)
  );

  assign value = internal_count;
  assign max_reached = internal_at_max;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n)
      max_reached_reg <= 1'b0;
    else
      max_reached_reg <= internal_at_max;
  end

endmodule
