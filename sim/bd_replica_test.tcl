#=============================================================================
# bd_replica_test.tcl
#-----------------------------------------------------------------------------
# Validation harness for integration/add_pwm_ip_to_bd.tcl.
#
# It rebuilds an exact replica of the real block design by sourcing the
# project's own base_system.tcl (so the PS7 configuration, the 3-master
# SmartConnect, axi_uartlite_0 / axi_gpio_0 / breath_led_0 and all three
# address segments are identical to the real project), then runs the
# integration script on that replica and checks the result.
#
# Nothing here touches the real Zynq_Base_Platform project: everything is
# created inside ./bd_replica.
#=============================================================================

set ws       [file normalize [file join [file dirname [file normalize [info script]]] ..]]
set base     [file join $ws sim bd_replica]
set real_prj [file normalize [file join $ws .. ..]]
set pwm_repo [file normalize [file join $ws ip_repo pwm_axi_ip_1_0]]
set ble_repo [file normalize [file join $ws .. breath_led_ip ip_repo breath_led_1_0]]

puts "============================================================"
puts " BD replica test"
puts "------------------------------------------------------------"
puts " workspace : $ws"
puts " replica   : $base"
puts " pwm repo  : $pwm_repo"
puts " ble repo  : $ble_repo"
puts "============================================================"

file delete -force $base

create_project -force bd_replica $base -part xc7z020clg400-2

set_property ip_repo_paths [list $ble_repo $pwm_repo] [current_project]
update_ip_catalog

#-----------------------------------------------------------------------------
# 1. rebuild the real block design from the project's own script
#-----------------------------------------------------------------------------
set bd_tcl [file join $real_prj base_system.tcl]
if { ![file exists $bd_tcl] } { error "not found: $bd_tcl" }
puts "INFO: sourcing $bd_tcl"
source $bd_tcl

#-----------------------------------------------------------------------------
# 2. run the integration script under test
#-----------------------------------------------------------------------------
set ::PWM_IP_REPO $pwm_repo
source [file join $ws integration add_pwm_ip_to_bd.tcl]

#-----------------------------------------------------------------------------
# 3. verify the result
#-----------------------------------------------------------------------------
set ::errors 0

proc vcheck { name ok } {
    if { $ok } {
        puts "  PASS  $name"
    } else {
        puts "  FAIL  $name"
        incr ::errors
    }
}

proc vcheck_safe { name body } {
    if { [catch { uplevel #0 [list expr $body] } res] } {
        puts "  FAIL  $name  (error: $res)"
        incr ::errors
    } else {
        vcheck $name $res
    }
}

# Block design object names are returned as full paths ("/axi_smc/M03_AXI"),
# so connectivity has to be checked with a suffix match.
proc net_has_pin { pin_path suffix } {
    set pin [get_bd_intf_pins -quiet $pin_path]
    if { $pin eq "" } { return 0 }
    set net [get_bd_intf_nets -quiet -of_objects $pin]
    if { $net eq "" } { return 0 }
    foreach p [get_bd_intf_pins -quiet -of_objects $net] {
        if { [string match "*${suffix}" $p] } { return 1 }
    }
    return 0
}

puts "============================================================"
puts " verification"
puts "------------------------------------------------------------"

# the interconnect grew exactly one master port
vcheck_safe "axi_smc NUM_MI grew 3 -> 4" {
    [get_property CONFIG.NUM_MI [get_bd_cells axi_smc]] == 4
}
vcheck_safe "axi_smc NUM_SI unchanged (1)" {
    [get_property CONFIG.NUM_SI [get_bd_cells axi_smc]] == 1
}

# the pre-existing connections must be intact
vcheck_safe "M00_AXI still connected (breath_led_0)" {
    [get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins axi_smc/M00_AXI]] ne ""
}
vcheck_safe "M01_AXI still connected (axi_uartlite_0)" {
    [get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins axi_smc/M01_AXI]] ne ""
}
vcheck_safe "M02_AXI still connected (axi_gpio_0)" {
    [get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins axi_smc/M02_AXI]] ne ""
}
vcheck_safe "axi_uartlite_0/S_AXI still on the M01_AXI net" {
    [net_has_pin axi_uartlite_0/S_AXI "axi_smc/M01_AXI"]
}
vcheck_safe "axi_gpio_0/S_AXI still on the M02_AXI net" {
    [net_has_pin axi_gpio_0/S_AXI "axi_smc/M02_AXI"]
}
vcheck_safe "breath_led_0/S00_AXI still on the M00_AXI net" {
    [net_has_pin breath_led_0/S00_AXI "axi_smc/M00_AXI"]
}

# the new peripheral
vcheck_safe "pwm_axi_ip_0 instantiated" {
    [get_bd_cells -quiet pwm_axi_ip_0] ne ""
}
vcheck_safe "pwm_axi_ip_0/S00_AXI is on the M03_AXI net" {
    [net_has_pin pwm_axi_ip_0/S00_AXI "axi_smc/M03_AXI"]
}
vcheck_safe "axi_smc/M03_AXI net reaches pwm_axi_ip_0/S00_AXI" {
    [net_has_pin axi_smc/M03_AXI "pwm_axi_ip_0/S00_AXI"]
}

# clock and reset are shared with the existing peripherals
vcheck_safe "pwm_axi_ip_0 shares the uartlite clock net" {
    [get_bd_nets -of_objects [get_bd_pins pwm_axi_ip_0/s00_axi_aclk]] eq \
    [get_bd_nets -of_objects [get_bd_pins axi_uartlite_0/s_axi_aclk]]
}
vcheck_safe "pwm_axi_ip_0 shares the uartlite reset net" {
    [get_bd_nets -of_objects [get_bd_pins pwm_axi_ip_0/s00_axi_aresetn]] eq \
    [get_bd_nets -of_objects [get_bd_pins axi_uartlite_0/s_axi_aresetn]]
}

# top level ports
vcheck_safe "pwm_out[19:0] is a top level output" {
    [get_property LEFT  [get_bd_ports pwm_out]] == 19 &&
    [get_property RIGHT [get_bd_ports pwm_out]] == 0 &&
    [get_property DIR   [get_bd_ports pwm_out]] eq "O"
}
vcheck_safe "gate_en[9:0] is a top level output" {
    [get_property LEFT  [get_bd_ports gate_en]] == 9 &&
    [get_property RIGHT [get_bd_ports gate_en]] == 0 &&
    [get_property DIR   [get_bd_ports gate_en]] eq "O"
}

# address map: the three old segments untouched, the new one added
set as [get_bd_addr_spaces processing_system7_0/Data]

proc seg_of { as pattern } {
    foreach s [get_bd_addr_segs -quiet -of_objects $as] {
        if { [string match $pattern $s] } { return $s }
    }
    return ""
}

foreach { pat off } [list \
        "*axi_gpio_0*"     0x41200000 \
        "*axi_uartlite_0*" 0x42C00000 \
        "*breath_led_0*"   0x43C00000 \
        "*pwm_axi_ip_0*"   0x43C10000 ] {
    set s [seg_of $as $pat]
    if { $s eq "" } {
        puts "  FAIL  address segment $pat is missing"
        incr ::errors
    } else {
        set got [format 0x%08X [expr {[get_property OFFSET $s] + 0}]]
        vcheck "$pat mapped at $off" [expr {$got eq $off}]
    }
}

puts "------------------------------------------------------------"
puts " address map after integration"
foreach s [get_bd_addr_segs -quiet -of_objects $as] {
    set o [get_property -quiet OFFSET $s]
    set r [get_property -quiet RANGE  $s]
    if { $o eq "" } { continue }
    puts [format "   %s .. %s   %s" \
        [format 0x%08X [expr {$o + 0}]] \
        [format 0x%08X [expr {$o + $r - 1}]] \
        [regsub {^.*/} $s ""]]
}

# the auto-managed wrapper must now carry the 20 new pins
set wf [glob -nocomplain [file join [get_property DIRECTORY [current_project]] \
        *.gen sources_1 bd base_system hdl base_system_wrapper.v]]
if { [llength $wf] == 0 } {
    puts "  FAIL  base_system_wrapper.v was not generated"
    incr ::errors
} else {
    set fh [open [lindex $wf 0] r]
    set txt [read $fh]
    close $fh
    vcheck "wrapper exports pwm_out" [string match "*pwm_out*" $txt]
    vcheck "wrapper exports gate_en" [string match "*gate_en*" $txt]
}

puts "============================================================"
if { $::errors == 0 } {
    puts " REPLICA TEST: ALL BLOCK DESIGN CHECKS PASSED"
} else {
    puts " REPLICA TEST: $::errors CHECK(S) FAILED"
}
puts "============================================================"

#-----------------------------------------------------------------------------
# 4. run a full project synthesis of the block design wrapper to prove that the
#    IP elaborates and synthesises correctly in the block design context.
#    This is the same flow the user runs for the real project.
#-----------------------------------------------------------------------------
puts "INFO: runs in the project: [get_runs -quiet]"

# The replica project was assembled by script, so the generated block design
# wrapper is not registered as the synthesis top yet.  (In the real
# Zynq_Base_Platform project the top is already base_system_wrapper.)
set wf [glob -nocomplain [file join [get_property DIRECTORY [current_project]] \
        *.gen sources_1 bd base_system hdl base_system_wrapper.v]]
if { [llength $wf] > 0 } {
    if { [catch { add_files -norecurse -fileset sources_1 [lindex $wf 0] } aerr] } {
        puts "INFO: add_files: $aerr"
    }
    if { [catch { set_property top base_system_wrapper [get_filesets sources_1] } terr] } {
        puts "WARN: could not set the synthesis top: $terr"
    } else {
        puts "INFO: synthesis top set to base_system_wrapper"
    }
    update_compile_order -fileset sources_1
} else {
    puts "WARN: base_system_wrapper.v not found"
}

if { [llength [get_runs -quiet synth_1]] == 0 } {
    if { [catch { create_run -flow "Vivado Synthesis 2024" synth_1 \
                     -part xc7z020clg400-2 } cerr] } {
        puts "  FAIL  could not create synth_1: $cerr"
        incr ::errors
    }
}

if { [llength [get_runs -quiet synth_1]] > 0 } {
    puts "INFO: launching synth_1"
    launch_runs synth_1 -jobs 2
    wait_on_run synth_1
    set st [get_property STATUS   [get_runs synth_1]]
    set pr [get_property PROGRESS [get_runs synth_1]]
    puts "INFO: synth_1 status = '$st'  progress = '$pr'"
    vcheck "synthesis run reached 100%" [expr {$pr eq "100%"}]

    if { $pr eq "100%" } {
        open_run synth_1 -name synth_1
        set pwmcell [get_cells -hier -quiet pwm_axi_ip_0]
        vcheck "pwm_axi_ip_0 present in the synthesised netlist" \
               [expr {$pwmcell ne ""}]
        set pwmregs [get_cells -hier -quiet -filter {NAME =~ *pwm_axi_ip_0*}]
        puts "INFO: cells under pwm_axi_ip_0: [llength $pwmregs]"
        report_utilization -file [file join $base synth_utilization.rpt]
        puts "INFO: utilization report written to [file join $base synth_utilization.rpt]"
    }
}

puts "============================================================"
if { $::errors == 0 } {
    puts " FINAL: ALL CHECKS PASSED (block design + synthesis)"
} else {
    puts " FINAL: $::errors CHECK(S) FAILED"
}
puts "============================================================"
