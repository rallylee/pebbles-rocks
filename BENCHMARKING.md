# Benchmarking

- We use YCSB (https://github.com/brianfrankcooper/YCSB) with the RocksDB driver (https://github.com/brianfrankcooper/YCSB/pull/1052) for benchmarking.

## Setting up the RocksDB Java library
- Run `export JAVA_HOME=/usr/lib/jvm/java-8-openjdk`
- From the root of the RocksDB repository, run `make -j10 DISABLE_JEMALLOC=1 rocksdbjava` (adjust `-j10` as necessary)
- `cd java/target` and run `mvn install:install-file -Dfile=rocksdbjni-5.9.0-linux64.jar -DgroupId=org.rocksdb -DartifactId=rocksdbjni -Dversion=5.9.0 -Dpackaging=jar`
  to install the jar file to your local Maven repository.

## Setting up YCSB
- Download YCSB from https://github.com/souvik1997/YCSB and checkout the `rocksdb` branch
- Run `mvn -nsu -pl com.yahoo.ycsb:rocksdb-binding -am clean package`
- Load the data with `python2 ./bin/ycsb load rocksdb -s -P workloads/workloada -p rocksdb.dir=/tmp/ycsb-rocksdb-data`
- Run the workload with `python2 ./bin/ycsb run rocksdb -s -P workloads/workloada -p rocksdb.dir=/tmp/ycsb-rocksdb-data`
