# Rebuild bitstream for Zynq_Base_Platform (Vivado batch mode).
#
# Usage:
#   vivado.bat -mode batch -source build\scripts\rebuild_bitstream.tcl
#   vivado.bat -mode batch -source build\scripts\rebuild_bitstream.tcl -tclargs --full
#
# Default  : reset impl_1 only (fast, use when only PS software changed).
# --full   : also re-run the IP out-of-context synthesis + synth_1.
#            REQUIRED when any HDL inside src/rtl/pwm_axi_ip_1_0/hdl changed,
#            otherwise the OOC dcp keeps the old logic and the board behaviour
#            never changes (see skill/references/build-flash.md section 3).

set script_dir [file dirname [file normalize [info script]]]
set proj [file normalize [file join $script_dir .. vivado_project Zynq_Base_Platform.xpr]]

if {![file exists $proj]} {
    puts "ERROR: project not found: $proj"
    exit 1
}

set full 0
if {[llength $argv] > 0 && [lindex $argv 0] eq "--full"} {
    set full 1
}

puts "=== project: $proj (full resynth: $full) ==="
open_project $proj

puts "=== before ==="
puts "impl_1 STATUS   : [get_property STATUS [get_runs impl_1]]"
puts "impl_1 PROGRESS : [get_property PROGRESS [get_runs impl_1]]"

if {$full} {
    set ip [get_ips -quiet base_system_pwm_axi_ip_0_0]
    if {[llength $ip] > 0} {
        puts "=== reset IP OOC synthesis: base_system_pwm_axi_ip_0_0 ==="
        reset_target all $ip
        generate_target all $ip
        reset_run base_system_pwm_axi_ip_0_0_synth_1
        launch_runs base_system_pwm_axi_ip_0_0_synth_1 -jobs 8
        wait_on_run base_system_pwm_axi_ip_0_0_synth_1
    } else {
        puts "WARNING: IP base_system_pwm_axi_ip_0_0 not found, skip OOC reset"
    }
    reset_run synth_1
    launch_runs synth_1 -jobs 8
    wait_on_run synth_1
}

reset_run impl_1
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1

puts "=== after ==="
puts "impl_1 STATUS   : [get_property STATUS [get_runs impl_1]]"
puts "impl_1 PROGRESS : [get_property PROGRESS [get_runs impl_1]]"

# Report unconstrained IO pins: silently unconstrained pins are the classic
# "synthesis passed but the board does nothing" cause.
if {[get_property PROGRESS [get_runs impl_1]] == "100%"} {
    open_run impl_1
    set unconstrained 0
    foreach p [get_ports] {
        if {[get_property PACKAGE_PIN $p] == ""} {
            puts "UNCONSTRAINED: $p"
            incr unconstrained
        }
    }
    puts "UNCONSTRAINED COUNT = $unconstrained"
    puts "BITSTREAM: [file join $script_dir .. vivado_project Zynq_Base_Platform.runs impl_1 base_system_wrapper.bit]"
    puts "DONE_OK"
} else {
    puts "IMPL_FAILED"
    exit 1
}
exit
