open_project fpga.xpr
update_compile_order -fileset sources_1
make_wrapper -files [get_files artyz7.bd] -top -import
launch_runs synth_1
wait_on_runs synth_1
launch_runs impl_1 # -to_step write_bitstream
wait_on_runs impl_1
write_hw_platform -fixed -force -file artyz7_wrapper.xsa -include_bit
