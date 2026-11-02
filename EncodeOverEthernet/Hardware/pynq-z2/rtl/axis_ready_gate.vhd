-- Copyright (C) 2026 Vitor Mendes Camilo
-- SPDX-License-Identifier: GPL-3.0-only
--
-- This file is part of OpenJLS-Demos. Available under GPLv3 or a
-- commercial license. See LICENSE.md and README.md for details.
--
-- axis_ready_gate: throttles an AXI4-Stream with pseudo-random per-cycle ready, for HIL
--
-- Beats pass only when a 16-bit random draw is below THRESH (any address): 65536 is full rate,
-- the reset value. Writing it reseeds, so a run is reproducible, and drops any held beat.
-- Plain VHDL: Vivado rejects VHDL-2008 at the top of a BD module reference.

library ieee;
  use ieee.std_logic_1164.all;
  use ieee.numeric_std.all;

entity axis_ready_gate is
  port (
    aclk               : in    std_logic;
    aresetn            : in    std_logic;
    s_axi_ctrl_awaddr  : in    std_logic_vector(11 downto 0);
    s_axi_ctrl_awprot  : in    std_logic_vector(2 downto 0);
    s_axi_ctrl_awvalid : in    std_logic;
    s_axi_ctrl_awready : out   std_logic;
    s_axi_ctrl_wdata   : in    std_logic_vector(31 downto 0);
    s_axi_ctrl_wstrb   : in    std_logic_vector(3 downto 0);
    s_axi_ctrl_wvalid  : in    std_logic;
    s_axi_ctrl_wready  : out   std_logic;
    s_axi_ctrl_bresp   : out   std_logic_vector(1 downto 0);
    s_axi_ctrl_bvalid  : out   std_logic;
    s_axi_ctrl_bready  : in    std_logic;
    s_axi_ctrl_araddr  : in    std_logic_vector(11 downto 0);
    s_axi_ctrl_arprot  : in    std_logic_vector(2 downto 0);
    s_axi_ctrl_arvalid : in    std_logic;
    s_axi_ctrl_arready : out   std_logic;
    s_axi_ctrl_rdata   : out   std_logic_vector(31 downto 0);
    s_axi_ctrl_rresp   : out   std_logic_vector(1 downto 0);
    s_axi_ctrl_rvalid  : out   std_logic;
    s_axi_ctrl_rready  : in    std_logic;
    s_axis_tdata       : in    std_logic_vector(63 downto 0);
    s_axis_tkeep       : in    std_logic_vector(7 downto 0);
    s_axis_tvalid      : in    std_logic;
    s_axis_tlast       : in    std_logic;
    s_axis_tready      : out   std_logic;
    m_axis_tdata       : out   std_logic_vector(63 downto 0);
    m_axis_tkeep       : out   std_logic_vector(7 downto 0);
    m_axis_tvalid      : out   std_logic;
    m_axis_tlast       : out   std_logic;
    m_axis_tready      : in    std_logic
  );

  attribute x_interface_info                 : string;
  attribute x_interface_parameter            : string;
  attribute x_interface_info of aclk         : signal is "xilinx.com:signal:clock:1.0 aclk CLK";
  attribute x_interface_parameter of aclk    : signal is "ASSOCIATED_BUSIF s_axi_ctrl:s_axis:m_axis, ASSOCIATED_RESET aresetn";
  attribute x_interface_info of aresetn      : signal is "xilinx.com:signal:reset:1.0 aresetn RST";
  attribute x_interface_parameter of aresetn : signal is "POLARITY ACTIVE_LOW";
end entity axis_ready_gate;

architecture rtl of axis_ready_gate is

  constant FULL_RATE : unsigned(16 downto 0)         := to_unsigned(65536, 17);
  constant SEED      : std_logic_vector(31 downto 0) := x"2545F491";

  signal sThresh  : unsigned(16 downto 0)         := FULL_RATE;
  signal sRandom  : std_logic_vector(31 downto 0) := SEED;
  signal sOpen    : std_logic                     := '1';
  signal sWrite   : std_logic;
  signal sReady   : std_logic;
  signal sValid   : std_logic                     := '0';
  signal sBvalid  : std_logic                     := '0';
  signal sRvalid  : std_logic                     := '0';
  signal sWready  : std_logic;
  signal sArready : std_logic;

  function xorshift32 (x : std_logic_vector(31 downto 0)) return std_logic_vector is
    variable v : unsigned(31 downto 0) := unsigned(x);
  begin
    v := v xor shift_left(v, 13);
    v := v xor shift_right(v, 17);
    v := v xor shift_left(v, 5);
    return std_logic_vector(v);
  end function xorshift32;

begin

  -- One write at a time, taken only with both channels up and no response pending
  sWready  <= s_axi_ctrl_awvalid and s_axi_ctrl_wvalid and not sBvalid;
  sWrite   <= sWready;
  sArready <= s_axi_ctrl_arvalid and not sRvalid;

  s_axi_ctrl_awready <= sWready;
  s_axi_ctrl_wready  <= sWready;
  s_axi_ctrl_bresp   <= "00";
  s_axi_ctrl_bvalid  <= sBvalid;
  s_axi_ctrl_arready <= sArready;
  s_axi_ctrl_rdata   <= std_logic_vector(resize(sThresh, 32));
  s_axi_ctrl_rresp   <= "00";
  s_axi_ctrl_rvalid  <= sRvalid;

  -- The slice keeps tvalid up until its handshake, which gating it directly would not
  sReady        <= sOpen and (m_axis_tready or not sValid);
  s_axis_tready <= sReady;
  m_axis_tvalid <= sValid;

  process (aclk) is
  begin

    if rising_edge(aclk) then
      sRandom <= xorshift32(sRandom);
      if (('0' & unsigned(sRandom(31 downto 16))) < sThresh) then
        sOpen <= '1';
      else
        sOpen <= '0';
      end if;

      if (m_axis_tready = '1') then
        sValid <= '0';
      end if;
      if (sReady = '1' and s_axis_tvalid = '1') then
        sValid       <= '1';
        m_axis_tdata <= s_axis_tdata;
        m_axis_tkeep <= s_axis_tkeep;
        m_axis_tlast <= s_axis_tlast;
      end if;

      if (s_axi_ctrl_bready = '1') then
        sBvalid <= '0';
      end if;
      if (sWrite = '1') then
        sBvalid <= '1';
        sThresh <= unsigned(s_axi_ctrl_wdata(16 downto 0));
        sRandom <= SEED;
        sValid  <= '0';
      end if;

      if (s_axi_ctrl_rready = '1') then
        sRvalid <= '0';
      end if;
      if (sArready = '1') then
        sRvalid <= '1';
      end if;

      if (aresetn = '0') then
        sThresh <= FULL_RATE;
        sRandom <= SEED;
        sOpen   <= '1';
        sValid  <= '0';
        sBvalid <= '0';
        sRvalid <= '0';
      end if;
    end if;

  end process;

end architecture rtl;
