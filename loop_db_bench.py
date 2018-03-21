import subprocess as sp

def run(f, num, cmd):
    results = list()
    for _ in range(num):
    	output = sp.check_output(cmd)
        f.write(output)
    	#split_output = output.splitlines()
        #cur = list()
    	#cur.append([string for string in split_output if string.startswith('fillrandom')])
    	#cur.append([string for string in split_output if string.startswith('readrandom')])
    	#cur.append([string for string in split_output if string.startswith('seekrandom')])
        #results.append(cur)
    #for t in range(3):
        #for i in range(num):
            #f.write(str(results[i][t]) + "\n")
        #f.write("\n")

def test(num_iters, num_inserts, num_reads, date, counter):
    filename = './results' + str(num_inserts) + str(num_reads) + "__" + date + "__" + str(counter)
    with open(filename, 'w') as f:
        command = "sudo ./db_bench --use_existing_db=0 --benchmarks=fillrandom,stats,sstables --open_files=1000 --db=/mnt/ssd/pebblesdbtest-1000 --num=500000 --reads=100000 --value_size=1024 --write_buffer_size=4194304 --level0_slowdown_writes_trigger=20 --level0_stop_writes_trigger=24 --compression_type=none" #change  20 --> 8 and 24 --> 12
        cmd_arr = command.split()
        cmd_arr[6] = '--num=' + str(num_inserts)
        cmd_arr[7] = '--reads=' + str(num_reads)
    	run(f, num_iters, cmd_arr)

clear = "sudo ./clear_cache.sh"
clear_cmd = clear.split()
date = "2018_03_20"
counter = 0
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 1--------")
"""
sp.check_output(clear_cmd)
counter += 1
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 2--------")
sp.check_output(clear_cmd)
counter += 1
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 3--------")
sp.check_output(clear_cmd)
counter += 1
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 4--------")
sp.check_output(clear_cmd)
counter += 1
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 5--------")
sp.check_output(clear_cmd)
counter += 1
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 6--------")
sp.check_output(clear_cmd)
counter += 1
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 7--------")
sp.check_output(clear_cmd)
counter += 1
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 8--------")
sp.check_output(clear_cmd)
counter += 1
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 9--------")
sp.check_output(clear_cmd)
counter += 1
test(1, 2500000, 100000, date, counter)
print("--------DONE WITH 10--------")
sp.check_output(clear_cmd)
counter += 1
"""
"""
test(10, 500000, 125000)
print("--------DONE WITH 2--------")
test(10, 500000, 250000)
print("--------DONE WITH 3--------")
test(10, 500000, 375000)
print("--------DONE WITH 4--------")
test(10, 1000000, 200000)
print("--------DONE WITH 5--------")
test(10, 1000000, 250000)
print("--------DONE WITH 6--------")
test(10, 1000000, 500000)
print("--------DONE WITH 7--------")
test(10, 1000000, 750000)
print("--------DONE WITH 8--------")
test(10, 2500000, 500000)
print("--------DONE WITH 9--------")
test(10, 2500000, 625000)
print("--------DONE WITH 10--------")
test(10, 2500000, 1250000)
print("--------DONE WITH 11--------")
test(10, 2500000, 1875000)
print("--------DONE WITH 12--------")
"""
