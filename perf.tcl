
set pcp [lindex [read_memory 0x20073204 32 1] 0]
set cpu [lindex [read_memory [expr {$pcp + 0}] 32 1] 0]
set cycaddr [expr {$cpu + 340}]
set c0 [lindex [read_memory $cycaddr 32 1] 0]
set n0 [lindex [read_memory 0x200304b8 32 1] 0]
set h0 [lindex [read_memory 0x200304b0 32 1] 0]
set t0 [clock milliseconds]
while {[expr {[clock milliseconds]-$t0}] < 15000} { }
set t1 [clock milliseconds]
set c1 [lindex [read_memory $cycaddr 32 1] 0]
set n1 [lindex [read_memory 0x200304b8 32 1] 0]
set h1 [lindex [read_memory 0x200304b0 32 1] 0]
puts "RESULT [expr {$t1-$t0}] [expr {$c1-$c0}] [expr {$n1-$n0}] [expr {$h1-$h0}]"
shutdown
