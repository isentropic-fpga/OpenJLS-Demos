# Recreate the Vivado project for the EncodeOverEthernet demo on the PYNQ-Z2
# and optionally build the bitstream. The block design itself lives in
# design_encode_ethernet.tcl (write_bd_tcl export, pinned to Vivado 2025.2).
#
# Usage:
#   vivado -mode batch -source build.tcl                      # project only
#   vivado -mode batch -source build.tcl -tclargs --bitstream # + bitstream
#
# The project lands in ./build/ (git-ignored) — the Tcl scripts are the
# source of truth; never commit the generated project. --build-dir moves it and
# --jobs caps Vivado's own parallelism, which together let build_all_bitness.sh
# run several depths at once without exhausting memory.

set demo_dir [file dirname [file normalize [info script]]]
set openjls_dir [file normalize [file join $demo_dir .. .. .. ThirdParty OpenJLS]]

if {![info exists argv]} { set argv {} }

# --bitness N : encoder sample precision (8..16) baked into the block design.
# Default 8 — the depth the demo was originally brought up and verified at.
# BITNESS 8 feeds the encoder an 8-bit pixel stream; 9..16 feed 16 bits
# (s_axis_pixel_tdata is 8*ceil(BITNESS/8) wide in openjls_axis_regs.vhd), so
# the DMA MM2S stream width has to move with it — both are set together below.
set bitness 8
set bidx [lsearch $argv "--bitness"]
if {$bidx >= 0} { set bitness [lindex $argv [expr {$bidx + 1}]] }
if {![string is integer -strict $bitness] || $bitness < 8 || $bitness > 16} {
    error "--bitness must be an integer in 8..16 (got '$bitness')"
}

# --jobs N : how many runs Vivado may launch at once. This is not a thread
# count — the block design has ~8 out-of-context IP synth runs, and each job is
# a separate ~2.5 GB vivado process, so N multiplies peak memory. Default 4 for
# a lone build; build_all_bitness.sh passes 1 and gets its parallelism from
# running several depths side by side instead.
set jobs 4
set jidx [lsearch $argv "--jobs"]
if {$jidx >= 0} { set jobs [lindex $argv [expr {$jidx + 1}]] }
if {![string is integer -strict $jobs] || $jobs < 1} {
    error "--jobs must be a positive integer (got '$jobs')"
}

# --build-dir PATH : where the generated project goes (a relative path is taken
# from this script's directory). Default ./build. Two Vivado runs sharing one
# project directory would clobber each other's runs/impl_1, so every depth in a
# concurrent sweep needs its own — see build_all_bitness.sh.
set proj_dir [file join $demo_dir build]
set didx [lsearch $argv "--build-dir"]
if {$didx >= 0} {
    set proj_dir [file normalize [file join $demo_dir [lindex $argv [expr {$didx + 1}]]]]
}

create_project encode_ethernet $proj_dir -part xc7z020clg400-1 -force
set_property target_language VHDL [current_project]

# Board files come from the Vivado Board Store; the design still builds
# without them since the PS configuration is baked into the BD script.
if {[catch {set_property BOARD_PART tul.com.tw:pynq-z2:part0:1.0 [current_project]} err]} {
    puts "WARNING: PYNQ-Z2 board files not installed, continuing with bare part: $err"
}

# The encoder now comes in as packaged IP (isentropic:openjls:*:1.3)
# from the submodule's committed IP repo. The cores are self-contained (OpenJLS
# RTL + the open-logic primitives bundled under each core's src/), so no raw
# RTL is added here and create_libraries_vivado.tcl is no longer sourced —
# which also retires the old VHDL-2008 vs module-reference FILE_TYPE dance.
set ip_repo [file join $openjls_dir Sources Xilinx ip_repo]
if {![file isdirectory $ip_repo]} {
    error "Packaged IP repo not found at $ip_repo — run\
           \"git submodule update --init --recursive\" from the repo root. The\
           block design instantiates isentropic:openjls:openjls_axis_regs:1.3,\
           so OpenJLS v1.3 or later is required; the submodule is pinned at a\
           commit that provides it."
}
set_property ip_repo_paths $ip_repo [current_project]
update_ip_catalog -rebuild

# Block design, then its HDL wrapper as top. All I/O is through the PS
# (DDR/FIXED_IO), so there is no XDC.
source [file join $demo_dir design_encode_ethernet.tcl]
if {[get_files -quiet design_encode_ethernet.bd] eq ""} {
    error "Block design was not created — see the messages above (Vivado version mismatch?)."
}

# Retarget the design to the requested precision. The encoder's BITNESS generic
# and the DMA's stream-side data width move together: a mismatch makes
# validate_bd_design fail on the MM2S <-> s_axis_pixel connection. The block
# design is authored at BITNESS 8 / 8-bit stream, so only non-8 depths change.
set pixel_stream_width [expr {$bitness <= 8 ? 8 : 16}]
set_property CONFIG.BITNESS $bitness [get_bd_cells openjls_axis_regs_0]
set_property CONFIG.c_m_axis_mm2s_tdata_width $pixel_stream_width [get_bd_cells axi_dma_0]
validate_bd_design
save_bd_design
puts "openjls: BITNESS=$bitness, pixel stream ${pixel_stream_width}-bit"

set wrapper [make_wrapper -files [get_files design_encode_ethernet.bd] -top]
add_files -norecurse $wrapper
set_property top design_encode_ethernet_wrapper [get_filesets sources_1]
update_compile_order -fileset sources_1

# Extra congestion spreading during placement; kept as belt-and-suspenders. The
# limit here is path depth, not congestion or fit (the part is only ~25% full),
# so this strategy alone does not rescue the design at higher clocks. Through
# OpenJLS v1.2 the critical path was inside the byte_stuffer, which forced the
# fabric clock down to 50 MHz. v1.3 reworked that block; a b16 probe at 100 MHz
# now closes to WNS = -2.797 ns, with the worst path running
# openjls_top/sReg1D1 -> ctx_ram/sUseInitReg and the runner-up in the errval
# datapath. The fabric clock is therefore set to 71.43 MHz (= 1000 MHz IO PLL
# / 14) in design_encode_ethernet.tcl -- the next divisor up, 76.9 MHz, does not
# close. Do not read the -2.797 ns probe as "Fmax is 78 MHz": the router only
# works as hard as the constraint demands, so a slack-derived Fmax from a
# relaxed run is optimistic. Measured at 14 ns, the binding depths are b16
# (WNS +0.131 ns) and b12 (+0.224 ns) -- under 2% margin, deliberately accepted.
# Any timing regression will surface at those two depths first.
set_property strategy Congestion_SpreadLogic_high [get_runs impl_1]

if {[info exists argv] && [lsearch $argv "--bitstream"] >= 0} {
    launch_runs impl_1 -to_step write_bitstream -jobs $jobs
    wait_on_run impl_1
    if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
        error "Implementation failed — open the project under ./build/ to inspect."
    }
    # Vivado writes a bitstream even when timing is violated (only a critical
    # warning), so "a .bit exists" does NOT mean the design is sound. Gate on the
    # post-route worst negative slack: a negative WNS means a staged bitstream
    # would be unreliable, so fail the build instead of shipping it. (This is what
    # caught the byte_stuffer path that forced the 83 -> 50 MHz fabric clock.)
    set wns [get_property STATS.WNS [get_runs impl_1]]
    if {$wns eq "" } {
        error "Could not read post-route WNS for impl_1 — cannot certify timing."
    }
    if {$wns < 0} {
        error "Timing NOT met: post-route WNS = ${wns} ns. Lower the fabric clock\
               (design_encode_ethernet.tcl PCW_*FPGA0_PERIPHERAL_FREQMHZ) or fix the\
               failing path; refusing to stage a timing-violating bitstream."
    }
    puts "Timing met: post-route WNS = ${wns} ns"
    puts "Bitstream: [glob [file join $proj_dir encode_ethernet.runs impl_1 *.bit]]"
}
