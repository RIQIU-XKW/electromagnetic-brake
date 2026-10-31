#=============================================================================
# fix_sim_library.tcl
#-----------------------------------------------------------------------------
# Fixes the behavioural-simulation compile failure in the IP packaging project:
#
#   [USF-XSim-62] 'compile' step failed with error(s)
#   ERROR: [XSIM 43-3268] Logical library name '".../hdl/pwm_axi_ip.v"' should
#                         not contain white space, new line, /, \, = or .
#   ERROR: [XSIM 43-3217] tb_pwm_vlog.prj (line 2): Incorrect project file syntax
#
# Cause
#   In edit_pwm_axi_ip_v1_0.xpr the two HDL files in sources_1 carry an EMPTY
#   Library attribute:
#
#       <File Path="$PPRDIR/pwm_axi_ip_1_0/hdl/pwm_axi_ip.v">
#         <Attr Name="Library" Val=""/>          <-- empty string
#
#   Vivado therefore generates a malformed xvlog project file:
#
#       verilog   \                                <-- library name missing
#       "../../../../pwm_axi_ip_1_0/hdl/pwm_axi_ip.v" \
#
#   and xvlog then parses the quoted FILE PATH as the LIBRARY NAME, which is
#   rejected because a library name may not contain '/' '\' '.' or spaces.
#
#   Note the difference: a MISSING Library attribute is harmless (Vivado falls
#   back to the fileset default xil_defaultlib) - it is the PRESENT BUT EMPTY
#   attribute that breaks.  tb_pwm.v has no Library attribute at all, which is
#   why the testbench line in the same .prj file is fine.
#
#   The IP packager's "edit" project is not meant to be simulated, so this
#   latent problem only shows up the first time someone runs a simulation in it.
#
# What this script does
#   1. assigns xil_defaultlib to the Verilog files of sources_1  (the real fix)
#   2. makes sure tb_pwm is the simulation top and is enabled
#   3. sets xsim.simulate.runtime to all  (the default 1000 ns would only show
#      0.7 % of the 145.8 us waveform demo)
#   4. refreshes the compile order and prints the resulting settings
#
# HOW TO RUN
#   Either paste the commands into the Tcl console of the already-open project,
#   or from Vivado:  source scripts/fix_sim_library.tcl
#
#   NOTE: do NOT run this with a second Vivado instance in batch mode while you
#   already have the project open in the GUI - two instances on one project can
#   corrupt it.
#=============================================================================

if { [llength [get_projects -quiet]] == 0 } {
    error "No project is open - open edit_pwm_axi_ip_v1_0.xpr first."
}

#-----------------------------------------------------------------------------
# 0. make sure every hdl/*.v of the IP is part of the project's sources_1.
#
#    The testbench only instantiates pwm_axi_ip, but the submodules it contains
#    (pwm_half_bridge, pwm_h_bridge) live in the IP repository and have to be
#    compiled by the project as well.  If any of them is missing, elaboration
#    fails with:
#        ERROR: [VRFC 10-2063] Module <pwm_h_bridge> not found
#              while processing module instance <g_bridge[0].u_h_bridge>
#    This happens whenever new RTL files are added to the IP, so it is checked
#    here as well as in scripts/repackage_ip.tcl.
#-----------------------------------------------------------------------------
set script_dir [file dirname [file normalize [info script]]]
set hdl_dir    [file normalize [file join $script_dir .. ip_repo pwm_axi_ip_1_0 hdl]]

set missing {}
foreach f [lsort [glob -nocomplain [file join $hdl_dir *.v]]] {
    set have 0
    foreach pf [get_files -quiet -of_objects [get_filesets sources_1]] {
        if { [file normalize $pf] eq [file normalize $f] } { set have 1 ; break }
    }
    if { !$have } { lappend missing $f }
}
if { [llength $missing] > 0 } {
    puts "INFO: adding missing RTL to sources_1:"
    foreach f $missing { puts "        [file tail $f]" }
    add_files -norecurse -fileset sources_1 $missing
} else {
    puts "INFO: sources_1 already contains every hdl/*.v"
}

#-----------------------------------------------------------------------------
# 1. the actual fix: give the RTL files a real library name
#    (only *.v of sources_1 - sources_1 also holds component.xml, and setting
#     a HDL library on the core definition file would corrupt it)
#-----------------------------------------------------------------------------
set rtl_files [get_files -quiet -of_objects [get_filesets sources_1] *.v]
if { [llength $rtl_files] == 0 } {
    error "no Verilog files found in sources_1 - wrong project?"
}

foreach f $rtl_files {
    set old [get_property -quiet library $f]
    set_property library xil_defaultlib $f
    puts "INFO: library '$old' -> 'xil_defaultlib'  : $f"
}

#-----------------------------------------------------------------------------
# 2. simulation top / enabled / runtime
#-----------------------------------------------------------------------------
set tb [get_files -quiet -of_objects [get_filesets sim_1] *tb_pwm.v]
if { [llength $tb] == 0 } {
    puts "WARNING: tb_pwm.v is not in sim_1 - skipping the simulation setup"
} else {
    set_property is_enabled true $tb
    set_property top tb_pwm [get_filesets sim_1]
    set_property -name {xsim.simulate.runtime} -value {all} -objects [get_filesets sim_1]
    puts "INFO: sim_1 top     = [get_property top [get_filesets sim_1]]"
    # NOTE: get_property takes POSITIONAL arguments only.  It has no -name /
    # -objects option (unlike set_property, which does).  Writing
    #   get_property -name {...} -objects {...}
    # fails with [Common 17-170] Unknown option '-objects' and aborts the
    # script, which is what happened on the first run.
    puts "INFO: sim_1 runtime = [get_property xsim.simulate.runtime [get_filesets sim_1]]"
}

#-----------------------------------------------------------------------------
# 3. refresh the compile order so the regenerated .prj picks up the library
#-----------------------------------------------------------------------------
update_compile_order -fileset sources_1
update_compile_order -fileset sim_1

#-----------------------------------------------------------------------------
# 4. report
#-----------------------------------------------------------------------------
puts ""
puts "============================================================"
puts " sources_1 libraries"
foreach f [get_files -quiet -of_objects [get_filesets sources_1] *.v] {
    puts "   [get_property library $f]  [file tail $f]"
}
puts "------------------------------------------------------------"
puts " sim_1 files"
foreach f [get_files -quiet -of_objects [get_filesets sim_1]] {
    puts "   [file tail $f]"
}
puts "============================================================"
puts " Done.  Now run:  Run Simulation -> Run Behavioral Simulation"
puts " (the .prj is regenerated, so the old xvlog error will be gone)"
puts "============================================================"
