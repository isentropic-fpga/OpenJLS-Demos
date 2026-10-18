# Copyright (C) 2026 Vitor Mendes Camilo
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of OpenJLS-Demos. Available under GPLv3 or a
# commercial license. See LICENSE.md and README.md for details.
#
# build.tcl: recreate the PYNQ-Z2 project in ./build and optionally the bitstream
#
# Usage: vivado -mode batch -source build.tcl
#          [-tclargs [--bitstream] [--bitness N] [--jobs N] [--build-dir PATH]]

set demo_dir [file dirname [file normalize [info script]]]
set openjls_dir [file normalize [file join $demo_dir .. .. .. ThirdParty OpenJLS]]

if {![info exists argv]} { set argv {} }

set bitness 8
set bidx [lsearch $argv "--bitness"]
if {$bidx >= 0} { set bitness [lindex $argv [expr {$bidx + 1}]] }
if {![string is integer -strict $bitness] || $bitness < 8 || $bitness > 16} {
    error "--bitness must be an integer in 8..16 (got '$bitness')"
}

# Concurrent runs, not threads: each is a ~2.5 GB vivado process.
set jobs 4
set jidx [lsearch $argv "--jobs"]
if {$jidx >= 0} { set jobs [lindex $argv [expr {$jidx + 1}]] }
if {![string is integer -strict $jobs] || $jobs < 1} {
    error "--jobs must be a positive integer (got '$jobs')"
}

# Relative to this script. Concurrent builds each need their own.
set proj_dir [file join $demo_dir build]
set didx [lsearch $argv "--build-dir"]
if {$didx >= 0} {
    set proj_dir [file normalize [file join $demo_dir [lindex $argv [expr {$didx + 1}]]]]
}

create_project encode_ethernet $proj_dir -part xc7z020clg400-1 -force
set_property target_language VHDL [current_project]

# Optional: the PS configuration is baked into the BD script.
if {[catch {set_property BOARD_PART tul.com.tw:pynq-z2:part0:1.0 [current_project]} err]} {
    puts "WARNING: PYNQ-Z2 board files not installed, continuing with bare part: $err"
}

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

# All I/O is through the PS, so there is no XDC.
source [file join $demo_dir design_encode_ethernet.tcl]
if {[get_files -quiet design_encode_ethernet.bd] eq ""} {
    error "Block design was not created — see the messages above (Vivado version mismatch?)."
}

# s_axis_pixel is 8 bits wide at BITNESS 8 and 16 above, so the MM2S width
# must follow or validate_bd_design fails.
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

# The limit is logic depth, not congestion (the part is ~25% full); the
# strategy is kept as a margin. The fabric clock is 70 MHz (DDR PLL 1050 / 15,
# see INTERNALS.md). With OpenJLS v1.3.1 the tightest depths are b16
# (WNS +0.334 ns) and b12 (+0.375 ns).
set_property strategy Congestion_SpreadLogic_high [get_runs impl_1]

if {[info exists argv] && [lsearch $argv "--bitstream"] >= 0} {
    launch_runs impl_1 -to_step write_bitstream -jobs $jobs
    wait_on_run impl_1
    if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
        error "Implementation failed — open the project under ./build/ to inspect."
    }
    # Vivado writes the .bit despite violated timing, so gate on WNS here.
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
