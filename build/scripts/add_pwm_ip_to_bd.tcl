#=============================================================================
# add_pwm_ip_to_bd.tcl
#-----------------------------------------------------------------------------
# Adds the pwm_axi_ip (5 x complementary PWM / H-bridge generator) to the
# existing "base_system" block design, hanging it off the SAME AXI master port
# (processing_system7_0/M_AXI_GP0) that axi_uartlite_0 already uses.
#
# What this script does
#   1. registers the pwm_axi_ip IP repository with the project
#   2. adds ONE master port (M03_AXI) to the existing SmartConnect axi_smc by
#      raising CONFIG.NUM_MI from 3 to 4
#   3. instantiates pwm_axi_ip_0 and connects it to that new master port
#   4. reuses the EXISTING clock and reset nets (the ones that already feed
#      axi_uartlite_0) - no new clocking logic is created
#   5. creates the two top-level ports pwm_out[19:0] and gate_en[9:0]
#   6. assigns a 64 KiB address window
#   7. validates and saves the block design
#
# What this script deliberately does NOT touch
#   * axi_uartlite_0, axi_gpio_0 and breath_led_0 and their connections
#   * the existing M00_AXI / M01_AXI / M02_AXI nets of axi_smc
#   * the existing address segments and base addresses
#   * the processing system configuration
# The SmartConnect port count is the only property that changes, because a
# SmartConnect cannot grow a new downstream peripheral without one more master
# port.  This is the minimal possible change and is the normal way to add a
# peripheral to an existing Zynq AXI master.
#
# HOW TO RUN
#   1. open Zynq_Base_Platform.xpr in Vivado 2024.2
#   2. Tools -> Tcl Console:
#          cd <this folder>
#          source add_pwm_ip_to_bd.tcl
#      (or in batch mode: vivado -mode batch -source add_pwm_ip_to_bd.tcl)
#
#   Optional overrides - set these BEFORE sourcing the script:
#          set ::PWM_BASE_ADDR  0x43C20000
#          set ::PWM_IP_REPO    D:/somewhere/ip_repo/pwm_axi_ip_1_0
#          set ::PWM_BD_NAME    base_system
#          set ::PWM_INST_NAME  pwm_axi_ip_0
#=============================================================================

#-----------------------------------------------------------------------------
# Configuration
#-----------------------------------------------------------------------------
if { ![info exists ::PWM_BD_NAME] }    { set ::PWM_BD_NAME    base_system }
if { ![info exists ::PWM_INST_NAME] }  { set ::PWM_INST_NAME  pwm_axi_ip_0 }
if { ![info exists ::PWM_BASE_ADDR] }  { set ::PWM_BASE_ADDR  0x43C10000 }
if { ![info exists ::PWM_ADDR_RANGE] } { set ::PWM_ADDR_RANGE 0x00010000 }
set ::PWM_VLNV "xilinx.com:user:pwm_axi_ip:1.0"

#-----------------------------------------------------------------------------
# Sanity checks
#-----------------------------------------------------------------------------
if { [llength [get_projects -quiet]] == 0 } {
    error "No project is open - open Zynq_Base_Platform.xpr first."
}
set proj     [current_project]
set proj_dir [file normalize [file dirname [get_property DIRECTORY $proj]]]

if { ![info exists ::PWM_IP_REPO] } {
    set ::PWM_IP_REPO [file normalize \
        [file join $proj_dir ip_manage pwm_ip ip_repo pwm_axi_ip_1_0]]
}

puts "============================================================"
puts " add_pwm_ip_to_bd"
puts "------------------------------------------------------------"
puts " project dir   : $proj_dir"
puts " IP repository : $::PWM_IP_REPO"
puts " block design  : $::PWM_BD_NAME"
puts " instance      : $::PWM_INST_NAME"
puts " base address  : $::PWM_BASE_ADDR"
puts "============================================================"

if { ![file exists [file join $::PWM_IP_REPO component.xml]] } {
    error "no component.xml in $::PWM_IP_REPO - run scripts/repackage_ip.tcl first, or set ::PWM_IP_REPO"
}

#-----------------------------------------------------------------------------
# 0. register the IP repository and make sure the core is in the catalog
#-----------------------------------------------------------------------------
set repos [get_property ip_repo_paths $proj]
if { [lsearch -exact $repos $::PWM_IP_REPO] < 0 } {
    lappend repos $::PWM_IP_REPO
    set_property ip_repo_paths $repos $proj
    puts "INFO: registered IP repository with the project"
}
update_ip_catalog

if { [get_ipdefs -all $::PWM_VLNV] eq "" } {
    error "$::PWM_VLNV is not in the IP catalog after adding $::PWM_IP_REPO"
}
puts "INFO: $::PWM_VLNV found in the IP catalog"

#-----------------------------------------------------------------------------
# 1. open the block design
#-----------------------------------------------------------------------------
set bd_file [get_files -quiet ${::PWM_BD_NAME}.bd]
if { $bd_file eq "" } {
    error "block design ${::PWM_BD_NAME}.bd was not found in the project"
}
open_bd_design $bd_file
puts "INFO: block design ${::PWM_BD_NAME} is open"

if { [get_bd_cells -quiet $::PWM_INST_NAME] ne "" } {
    error "cell $::PWM_INST_NAME already exists in ${::PWM_BD_NAME} - nothing was changed"
}
if { [get_bd_ports -quiet pwm_out] ne "" || [get_bd_ports -quiet gate_en] ne "" } {
    error "pwm_out / gate_en ports already exist in ${::PWM_BD_NAME} - nothing was changed"
}

# a quick safety copy of the current block design, so the change can be undone
set backup [file join [file dirname [file normalize [info script]]] "${::PWM_BD_NAME}_before_pwm.tcl"]
if { [catch { write_bd_tcl -force $backup } err] } {
    puts "WARNING: could not write the backup $backup : $err"
} else {
    puts "INFO: backup of the current block design written to $backup"
}

#-----------------------------------------------------------------------------
# 2. find the AXI interconnect that sits behind M_AXI_GP0
#-----------------------------------------------------------------------------
set gp0_pin [get_bd_intf_pins -quiet processing_system7_0/M_AXI_GP0]
if { $gp0_pin eq "" } {
    error "processing_system7_0/M_AXI_GP0 not found - adjust the script for your design"
}
set gp0_net [get_bd_intf_nets -quiet -of_objects $gp0_pin]

set smc_pin ""
foreach p [get_bd_intf_pins -quiet -of_objects $gp0_net] {
    if { $p ne $gp0_pin } { set smc_pin $p }
}
if { $smc_pin eq "" } {
    error "no slave interface found on the M_AXI_GP0 net - is the interconnect connected?"
}
set smc [get_bd_cells -quiet -of_objects $smc_pin]
puts "INFO: M_AXI_GP0 drives ${smc}/${smc_pin}"

#-----------------------------------------------------------------------------
# 3. add one master port to the interconnect (the ONLY change to it)
#-----------------------------------------------------------------------------
set num_mi [get_property -quiet CONFIG.NUM_MI $smc]
if { $num_mi eq "" } {
    error "$smc does not expose CONFIG.NUM_MI - this script expects an AXI SmartConnect"
}
puts "INFO: $smc has NUM_MI = $num_mi (all existing master ports stay untouched)"
set_property -dict [list CONFIG.NUM_MI [expr {$num_mi + 1}]] $smc

set new_mi_name [format "M%02d_AXI" $num_mi]
set new_mi      [get_bd_intf_pins -quiet ${smc}/${new_mi_name}]
if { $new_mi eq "" } {
    error "expected a new master port ${smc}/${new_mi_name} after raising NUM_MI, but it is missing"
}
puts "INFO: new interconnect master port ${smc}/${new_mi_name} created"

#-----------------------------------------------------------------------------
# 4. instantiate the PWM IP and connect it to the new master port
#-----------------------------------------------------------------------------
set pwm [create_bd_cell -type ip -vlnv $::PWM_VLNV $::PWM_INST_NAME]
puts "INFO: created $::PWM_INST_NAME"
connect_bd_intf_net $new_mi [get_bd_intf_pins ${pwm}/S00_AXI]
puts "INFO: ${smc}/${new_mi_name} -> ${::PWM_INST_NAME}/S00_AXI"

#-----------------------------------------------------------------------------
# 5. reuse the existing clock and reset nets
#-----------------------------------------------------------------------------
proc ::pwm_net_driver { pin_path } {
    set pin [get_bd_pins -quiet $pin_path]
    if { $pin eq "" } { return "" }
    set net [get_bd_nets -quiet -of_objects $pin]
    if { $net eq "" } { return "" }
    set drv [get_bd_pins -quiet -of_objects $net -filter {DIR == O}]
    if { [llength $drv] == 0 } { return "" }
    return [lindex $drv 0]
}

set clk_drv [::pwm_net_driver axi_uartlite_0/s_axi_aclk]
if { $clk_drv eq "" } { set clk_drv [::pwm_net_driver processing_system7_0/FCLK_CLK0] }
if { $clk_drv eq "" } { error "could not find the AXI clock net" }
connect_bd_net $clk_drv [get_bd_pins ${pwm}/s00_axi_aclk]
puts "INFO: clock  : $clk_drv -> ${::PWM_INST_NAME}/s00_axi_aclk"

set rst_drv [::pwm_net_driver axi_uartlite_0/s_axi_aresetn]
if { $rst_drv eq "" } { set rst_drv [::pwm_net_driver rst_ps7_0_50M/peripheral_aresetn] }
if { $rst_drv eq "" } { error "could not find the AXI reset net" }
connect_bd_net $rst_drv [get_bd_pins ${pwm}/s00_axi_aresetn]
puts "INFO: reset  : $rst_drv -> ${::PWM_INST_NAME}/s00_axi_aresetn"

#-----------------------------------------------------------------------------
# 6. bring the outputs and the overcurrent inputs out to the top level
#-----------------------------------------------------------------------------
#   5 H bridges, bridge i (0..4):
#     pwm_out[4*i+0] = AH  leg A high side   pwm_out[4*i+1] = AL  leg A low side
#     pwm_out[4*i+2] = BH  leg B high side   pwm_out[4*i+3] = BL  leg B low  side
#     gate_en[2*i+0] = leg A driver enable   gate_en[2*i+1] = leg B driver enable
#     oc_n[i]        = bridge i overcurrent, ACTIVE LOW, internal pull-up
create_bd_port -dir O -from 19 -to 0 pwm_out
create_bd_port -dir O -from 9  -to 0 gate_en
create_bd_port -dir I -from 4  -to 0 oc_n
connect_bd_net [get_bd_pins ${pwm}/pwm_out] [get_bd_ports pwm_out]
connect_bd_net [get_bd_pins ${pwm}/gate_en] [get_bd_ports gate_en]
connect_bd_net [get_bd_pins ${pwm}/oc_n]    [get_bd_ports oc_n]
puts "INFO: created top level ports pwm_out[19:0], gate_en[9:0], oc_n[4:0]"

#-----------------------------------------------------------------------------
# 7. address assignment (checked against the existing segments)
#-----------------------------------------------------------------------------
set addr_space [get_bd_addr_spaces -quiet processing_system7_0/Data]
if { $addr_space eq "" } { error "address space processing_system7_0/Data not found" }

set base  [expr {$::PWM_BASE_ADDR + 0}]
set range [expr {$::PWM_ADDR_RANGE + 0}]

set used {}
foreach seg [get_bd_addr_segs -quiet -of_objects $addr_space] {
    set so [get_property -quiet OFFSET $seg]
    set sr [get_property -quiet RANGE  $seg]
    if { $so eq "" || $sr eq "" } { continue }
    set so [expr {$so + 0}]
    set sr [expr {$sr + 0}]
    lappend used [list $so $sr $seg]
    if { $base < ($so + $sr) && $so < ($base + $range) } {
        puts "ERROR: $::PWM_BASE_ADDR + $::PWM_ADDR_RANGE overlaps the existing segment:"
        puts "         $seg  (offset [format 0x%08X $so], range [format 0x%08X $sr])"
        puts "       existing map:"
        foreach u $used {
            puts "         [format 0x%08X [lindex $u 0]] .. [format 0x%08X [expr {[lindex $u 0] + [lindex $u 1] - 1}]]  [lindex $u 2]"
        }
        error "pick a different address, e.g.  set ::PWM_BASE_ADDR 0x43C20000  before sourcing this script"
    }
}

assign_bd_address -offset $::PWM_BASE_ADDR -range $::PWM_ADDR_RANGE \
    -target_address_space $addr_space \
    [get_bd_addr_segs ${pwm}/S00_AXI/S00_AXI_reg] -force
puts "INFO: ${::PWM_INST_NAME} mapped at $::PWM_BASE_ADDR (range $::PWM_ADDR_RANGE)"

#-----------------------------------------------------------------------------
# 8. validate and save
#-----------------------------------------------------------------------------
set vres [validate_bd_design]
puts "INFO: validate_bd_design -> $vres"
save_bd_design

# refresh the block design output products, including the auto-managed
# base_system_wrapper.v which now carries the 20 new ports
generate_target all $bd_file
puts "INFO: regenerated the block design output products (wrapper updated)"

#-----------------------------------------------------------------------------
# summary
#-----------------------------------------------------------------------------
puts "============================================================"
puts " done"
puts "------------------------------------------------------------"
puts " instance     : $::PWM_INST_NAME  ($::PWM_VLNV)"
puts " AXI master   : processing_system7_0/M_AXI_GP0 via ${smc}/${new_mi_name}"
puts " base address : $::PWM_BASE_ADDR"
puts " new ports    : pwm_out[19:0], gate_en[9:0]   (30 pins)"
puts "------------------------------------------------------------"
puts " NEXT STEPS"
puts "   1. add pin constraints for pwm_out[0..9] and gate_en[0..9]"
puts "      (see integration/pwm_pins.xdc.example)"
puts "   2. Generate Bitstream"
puts "   3. export a new XSA and rebuild the Vitis platform"
puts "============================================================"
