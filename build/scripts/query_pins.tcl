link_design -part xc7z020clg400-2
# repo root = <script dir>/../..  (works on any drive)
set script_dir [file dirname [file normalize [info script]]]
set out [file normalize [file join $script_dir .. .. data package_pins.txt]]
puts "writing: $out"
set f [open $out w]
puts $f "# package_pin bank  general_purpose  clock_capable"
foreach p [lsort [get_package_pins]] {
    set bank [get_property BANK $p]
    set gp   [get_property IS_GENERAL_PURPOSE $p]
    set clk  [get_property IS_CLK_CAPABLE $p]
    puts $f "$p $bank $gp $clk"
}
close $f
puts "DONE"
exit
