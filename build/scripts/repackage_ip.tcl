#=============================================================================
# repackage_ip.tcl
#-----------------------------------------------------------------------------
# Re-packages the pwm_axi_ip core so that the Vivado IP catalog picks up the
# current RTL:
#   * the top level port list (pwm_out[19:0], gate_en[9:0])
#   * the HDL file list (pwm_half_bridge.v and pwm_h_bridge.v were added when
#     the design grew from 5 half bridges to 5 full H bridges)
#
# The existing component metadata (VLNV xilinx.com:user:pwm_axi_ip:1.0, the
# software driver fileset, the xgui fileset and the bd_tcl fileset) is
# preserved - only the ports and the HDL file sets are refreshed.
#
# WHY THE FILE LIST MATTERS
#   component.xml records which HDL files make up the core.  If a new submodule
#   is not listed there, a block design that instantiates this IP fails with
#   "module 'pwm_h_bridge' not found" - the submodule is never copied into the
#   BD's ipshared area.  Adding the file to the packaging project alone is not
#   enough; it has to be merged into the core.
#
# Run from a Vivado Tcl console or in batch mode:
#     vivado -mode batch -source scripts/repackage_ip.tcl
#
# NOTE: do not run this while another Vivado instance has the packaging project
#       open - two instances on one project can corrupt it.
#=============================================================================

set script_dir [file dirname [file normalize [info script]]]
set ip_repo    [file normalize [file join $script_dir .. ip_repo]]
set proj_file  [file join $ip_repo edit_pwm_axi_ip_v1_0.xpr]
set core_xml   [file join $ip_repo pwm_axi_ip_1_0 component.xml]
set rtl_dir    [file join $ip_repo pwm_axi_ip_1_0 hdl]

puts "INFO: ip_repo   = $ip_repo"
puts "INFO: project   = $proj_file"
puts "INFO: component = $core_xml"
puts "INFO: rtl_dir   = $rtl_dir"

if { ![file exists $proj_file] } {
    error "packaging project not found: $proj_file"
}
if { ![file exists $rtl_dir] } {
    error "RTL directory not found: $rtl_dir"
}

#-----------------------------------------------------------------------------
# The authoritative RTL list = every .v in hdl/
#-----------------------------------------------------------------------------
set rtl_files [lsort [glob -nocomplain [file join $rtl_dir *.v]]]
if { [llength $rtl_files] == 0 } {
    error "no .v files in $rtl_dir"
}
puts "INFO: RTL files found in hdl/:"
foreach f $rtl_files { puts "        [file tail $f]" }

# If the packaging project is ALREADY open in this Vivado session (which is the
# normal case when you run this from the Tcl console of the IP packager window),
# do not call open_project again - that fails.  Only close it at the end if this
# script opened it.
set we_opened_it 1
set cur_proj [current_project -quiet]
if { $cur_proj ne "" && \
     [file normalize [get_property DIRECTORY $cur_proj]] eq [file normalize [file dirname $proj_file]] } {
    puts "INFO: the packaging project is already open in this session - reusing it"
    set we_opened_it 0
} else {
    open_project $proj_file
}

#-----------------------------------------------------------------------------
# 1. make sure every hdl/*.v is part of the project's sources_1
#-----------------------------------------------------------------------------
set missing {}
foreach f $rtl_files {
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
    puts "INFO: sources_1 already contains all hdl/*.v"
}

# The IP packager's managed project adds the RTL with an EMPTY Library attribute
# (<Attr Name="Library" Val=""/>).  Vivado then writes a malformed xvlog project
# file ("verilog <empty> \"<path>\"") and behavioural simulation fails with
#   ERROR: [XSIM 43-3268] Logical library name '".../pwm_axi_ip.v"' should not
#                         contain white space, new line, /, \, = or .
# A MISSING Library attribute is harmless (the fileset default xil_defaultlib
# applies); it is the present-but-empty one that breaks.  Normalise it here so
# that re-packaging never re-introduces the problem.
foreach f [get_files -quiet -of_objects [get_filesets sources_1] *.v] {
    if { [get_property -quiet library $f] eq "" } {
        puts "INFO: setting library xil_defaultlib on [file tail $f]"
        set_property library xil_defaultlib $f
    }
}

update_compile_order -fileset sources_1
set_property top pwm_axi_ip [get_filesets sources_1]

#-----------------------------------------------------------------------------
# 2. get the core
#-----------------------------------------------------------------------------
set core [ipx::current_core -quiet]
if { $core eq "" } {
    puts "INFO: opening core $core_xml"
    ipx::open_core -set_current true $core_xml
    set core [ipx::current_core]
}
puts "INFO: core VLNV = [get_property VLNV $core]"

#-----------------------------------------------------------------------------
# 3. merge the HDL file list into the core
#    In Vivado 2024.2 the merge wizard is selected with a leading argument:
#      ipx::merge_project_changes <ports|files|hdl_parameters|compatibility> <core>
#-----------------------------------------------------------------------------
if { [catch { ipx::merge_project_changes files $core } err] } {
    puts "WARNING: the file merge failed: $err"
    puts "         In the GUI use: IP Packager -> Merge changes from -> File Changes Wizard"
} else {
    puts "INFO: HDL file list merged into the core"
}

#-----------------------------------------------------------------------------
# 4. merge the top level port list into the core
#-----------------------------------------------------------------------------
if { [catch { ipx::merge_project_changes ports $core } err] } {
    puts "WARNING: the port merge failed: $err"
    puts "         In the GUI use: IP Packager -> Merge changes from -> RTL Changes Wizard"
} else {
    puts "INFO: port list merged into the core"
}

#-----------------------------------------------------------------------------
# 4b. merge the HDL parameters into the core.
#
#     C_S00_AXI_ADDR_WIDTH went from 6 to 7 when the PHASEn registers were added
#     at 0x40..0x50.  If component.xml keeps 6, the block design only propagates
#     6 address bits and writes to 0x40..0x50 silently alias onto 0x00..0x10 -
#     i.e. they would clobber CTRL / PERIOD / DUTY0 instead of setting a phase.
#     This must be 7 in the packaged core.
#-----------------------------------------------------------------------------
if { [catch { ipx::merge_project_changes hdl_parameters $core } err] } {
    puts "WARNING: the hdl_parameters merge failed: $err"
    puts "         In the GUI use: IP Packager -> Merge changes from -> RTL Changes Wizard"
} else {
    puts "INFO: HDL parameters merged into the core"
}

if { [catch {
        set ap [ipx::get_hdl_parameters -quiet -of_objects $core C_S00_AXI_ADDR_WIDTH]
        if { [llength $ap] > 0 } {
            puts "INFO: core C_S00_AXI_ADDR_WIDTH = [get_property value [lindex $ap 0]]  (must be 7)"
        } else {
            puts "WARNING: C_S00_AXI_ADDR_WIDTH not found in the core - check it manually"
        }
      } err] } {
    puts "WARNING: could not read back C_S00_AXI_ADDR_WIDTH: $err"
}

#-----------------------------------------------------------------------------
# 5. prune: the core should contain exactly the hdl/*.v files, nothing else.
#    (Guards against a testbench that lives in the project's sim_1 fileset
#     being dragged into the shipped IP.)
#
#    !!! THIS STEP IS BEST-EFFORT AND MUST NEVER ABORT THE SCRIPT !!!
#    ipx::get_file_sets does NOT exist in Vivado 2024.2 (the earlier version of
#    this script died here, which meant ipx::save_core never ran and the core
#    changes were silently lost).  So everything below is (a) guarded by an
#    explicit command-availability check and (b) wrapped in catch.  Losing the
#    prune only leaves a harmless extra file in a fileset; losing save_core
#    loses the whole repackage.
#-----------------------------------------------------------------------------
set want {}
foreach f $rtl_files { lappend want "hdl/[file tail $f]" }

if { [llength [info commands ipx::get_file_sets]] == 0 } {
    puts "INFO: this Vivado build has no ipx::get_file_sets -> file prune skipped"
    puts "      (not fatal; check the core file list manually if you care:"
    puts "       Package IP -> File Groups, or just grep component.xml)"
} elseif { [catch {

    foreach fsname {xilinx_verilogsynthesis_view_fileset \
                    xilinx_verilogbehavioralsimulation_view_fileset} {
        set fs [ipx::get_file_sets -quiet -of_objects $core $fsname]
        if { [llength $fs] == 0 } {
            puts "WARNING: fileset $fsname not found in the core"
            continue
        }
        set fs [lindex $fs 0]

        foreach cf [ipx::get_files -quiet -of_objects $fs] {
            set cn [get_property name $cf]
            if { [lsearch -exact $want $cn] < 0 } {
                if { [catch { ipx::remove_file $cn $fs } err] } {
                    puts "WARNING: could not remove stale core file '$cn' from $fsname : $err"
                } else {
                    puts "INFO: removed stale core file '$cn' from $fsname"
                }
            }
        }

        foreach cn $want {
            if { [llength [ipx::get_files -quiet -of_objects $fs $cn]] == 0 } {
                if { [catch { ipx::add_file $cn $fs } err] } {
                    puts "WARNING: could not add '$cn' to $fsname : $err"
                } else {
                    puts "INFO: added '$cn' to $fsname"
                }
            }
        }
    }

} err] } {
    puts "INFO: file prune skipped ($err) - not fatal"
}

#-----------------------------------------------------------------------------
# 6. save FIRST, so that no reporting or pruning problem can ever lose the
#    repackage.  Everything after this point is read-only reporting.
#-----------------------------------------------------------------------------
ipx::update_checksums $core
ipx::save_core $core
puts "INFO: component.xml SAVED"

#-----------------------------------------------------------------------------
# 7. report (read-only, best effort)
#-----------------------------------------------------------------------------
puts ""
puts "INFO: ports now in the core:"
foreach p [ipx::get_ports -of_objects $core] {
    puts "        [get_property name $p]"
}
puts ""
puts "INFO: HDL files now in the core:"
if { [llength [info commands ipx::get_file_sets]] == 0 } {
    puts "        (cannot enumerate on this Vivado build - no ipx::get_file_sets)"
    puts "        check component.xml's xilinx_verilogsynthesis_view_fileset instead"
} elseif { [catch {
    foreach fsname {xilinx_verilogsynthesis_view_fileset \
                    xilinx_verilogbehavioralsimulation_view_fileset} {
        set fs [ipx::get_file_sets -quiet -of_objects $core $fsname]
        if { [llength $fs] == 0 } { continue }
        foreach cf [ipx::get_files -quiet -of_objects [lindex $fs 0]] {
            puts "        $fsname : [get_property name $cf]"
        }
    }
} err] } {
    puts "        (enumeration failed: $err)"
}

if { $we_opened_it } {
    close_project
} else {
    puts "INFO: leaving your project open (this script did not open it)"
}

puts ""
puts "INFO: repackaging finished"
puts "      verify with:  report_ip_status   (or check component.xml)"
