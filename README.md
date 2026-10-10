# SapphMap-SIGMOD27

Artifacts of Paper "SapphMap: A Differentially Oblivious Map for Enclaves".

## 1 Artifacts Overview

### 1.1 Organization

In the repository root, the project is organized as follows:

```bash
.
├── applications
│   └── sapphmap
│       └── Enclave
│           └── TrustedLibrary
│               └── sapphmap_test.cpp
├── benchmark
│   ├── dependency
│   │   └── ycsb-0.17.0
│   ├── Exp1
│   │   ├── rangequery
│   │   ├── scalability
│   │   │   └── generator.cpp
│   │   └── workload
│   │       ├── ibm
│   │       │   ├── generator.cpp
│   │       │   └── raw
│   │       ├── metakv
│   │       │   ├── generator.cpp
│   │       │   └── raw
│   │       └── ycsb
│   │           └── generator.cpp
│   └── Exp2
│       ├── blocksize
│       ├── buffersize
│       ├── fanout
│       ├── generator.cpp
│       ├── payload
│       └── privacy
├── docker
│   ├── Dockerfile
│   └── docker-entrypoint.sh
├── DOSearch
│   ├── external_memory
│   │   └── test
│   │       └── experiment.hpp
│   └── ...
├── run.sh
└── run_exp.sh
```

### 1.2 Scripts

`run.sh` provides the low-level build and runtime utilities, including Docker image build.

`run_exp.sh` is the unified entry script for Exp1 (Section 7.2 of Paper) and Exp2 (Section 7.3 of Paper) experiment execution.

### 1.3 Experiment Workloads

The `./benchmark` directory organizes workload traces by experiment type.
Due to cloud storage limitations, we do not include dataset files in this artifact package; all data collection and generation scripts are provided in Section 2 of this README.

#### 1.3.1 Exp1 folders

- `./benchmark/Exp1/scalability/` stores YCSB size-sweep traces for scalability experiments.
- `./benchmark/Exp1/rangequery/` stores range-query traces used in the Exp1 range-query evaluation.
- `./benchmark/Exp1/workload/` contains workloads for workload-execution experiments: `ycsb`, `metakv`, and `ibm`. The `ycsb` directory provides traces for both `ycsb_a` and `ycsb_mix`.

#### 1.3.2 Exp2 folders

- `./benchmark/Exp2/blocksize/` is used for the block-size sensitivity experiment.
- `./benchmark/Exp2/buffersize/` is used for the buffer-size sensitivity experiment.
- `./benchmark/Exp2/fanout/` is used for the fanout (gamma) sensitivity experiment.
- `./benchmark/Exp2/privacy/` is used for the privacy-budget sensitivity experiment.
- `./benchmark/Exp2/payload/` is used for the payload-size sensitivity experiment.

The insertion-search trade-off experiment uses the same `ycsb_a` and `ycsb_mix` workload traces as Exp1.

**Note on Generators:** Several of these experiment and workload directories (`Exp1/scalability`, `Exp1/workload/ibm`, `Exp1/workload/metakv`, `Exp1/workload/ycsb`, `Exp2`) contain their own `generator.cpp` file, which is used to generate the required workload traces for that specific experiment.

#### 1.3.3 Shared dependency

- `./benchmark/dependency/ycsb-0.17.0/` is the YCSB dependency used to generate synthetic workloads.

### 1.4 Codes

#### 1.4.1 `applications/sapphmap/Enclave/TrustedLibrary/sapphmap_test.cpp`

This file is the runtime entry point of the benchmark code path in enclave mode.

#### 1.4.2 `DOSearch/external_memory/test/experiment.hpp`

This header contains the benchmark dispatcher and experiment implementations.

#### 1.4.3 Entry invocation chain

The execution path is:

```text
applications/sapphmap/Enclave/TrustedLibrary/sapphmap_test.cpp
  -> includes DOSearch/external_memory/test/experiment.hpp
  -> calls Experiment::run_bench_target()
```

#### 1.4.4 `DOSearch/`

This directory contains the core code implementations of the project.

## 2 Dataset Generation

### 2.1 Step 1: Prepare the YCSB dependency

Download YCSB 0.17.0 and extract it into the `dependency/` directory (in this repository: `./benchmark/dependency/`):

```bash
cd benchmark/dependency
wget -O ycsb-0.17.0.tar.gz \
  https://github.com/brianfrankcooper/YCSB/releases/download/0.17.0/ycsb-0.17.0.tar.gz
tar -xzf ycsb-0.17.0.tar.gz
```

After extraction, ensure the following path exists:

```bash
benchmark/dependency/ycsb-0.17.0
```

### 2.2 Step 2: Download required datasets

#### 2.2.1 MetaKV

Download the MetaKV trace and place it under `metakv/raw/`:

```bash
cd benchmark/Exp1/workload/metakv/raw
wget -O 202401_kv_traces_all_sort.csv.zst \
  https://cache-datasets.s3.amazonaws.com/cache_dataset_txt/2022_metaKV/202401_kv_traces_all_sort.csv.zst
zstd -d 202401_kv_traces_all_sort.csv.zst
```

This produces:

```bash
benchmark/Exp1/workload/metakv/raw/202401_kv_traces_all_sort.csv
```

#### 2.2.2 IBM Object Store Traces

Download IBM Object Store traces from:

```text
https://iotta.snia.org/traces/key-value
```

Place all part files from `IBMObjectStoreTrace000Part0` to `IBMObjectStoreTrace097Part0` into:

```bash
benchmark/Exp1/workload/ibm/raw/
```

### 2.3 Step 3: Run data generators

For each directory that contains a `generator.cpp` (e.g., `benchmark/Exp1/scalability`, `benchmark/Exp1/workload/ibm`, `benchmark/Exp1/workload/metakv`, `benchmark/Exp1/workload/ycsb`, and `benchmark/Exp2`), compile and run the generator to produce the corresponding experiment files.

Example:

```bash
cd benchmark/Exp2
g++ -O2 -std=c++17 generator.cpp -o generator
./generator
```

Note: The files used for range-query experiments are specifically generated by the generator in `./benchmark/Exp2/`.
The same generator also prepares the payload experiment files under `benchmark/Exp2/payload`.

## 3 Experimental Workflow

### 3.1 Build Docker image

Before running experiments, build the Docker image once:

```bash
./run.sh build
```

### 3.2 Run experiments with `run_exp.sh`

Use `run_exp.sh` as the unified script to execute experiments.
Exp1 supports SapphMap, EnigMap, NoLSMTree, NoHashMap, and H2O2RAM; all use the same entry points and workload traces.

Print usage:

```bash
./run_exp.sh --help
```

Run experiments:

```bash
./run_exp.sh Exp1 scalability <18|19|...|all> <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>
./run_exp.sh Exp1 workload <metakv|ibm|ycsb_mix|ycsb_a|all> <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>
./run_exp.sh Exp1 rangequery all <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>

./run_exp.sh Exp2 blocksize
./run_exp.sh Exp2 buffersize
./run_exp.sh Exp2 fanout
./run_exp.sh Exp2 privacy
./run_exp.sh Exp2 tradeoff
./run_exp.sh Exp2 payload
```

The insertion-search trade-off experiment varies the enclave buffer size for both `ycsb_a` and `ycsb_mix` under the 128 GB RAM / 32 GB EPC and 1 GB RAM / 256 MB EPC settings, and reports write and query time separately.

The payload-size sensitivity experiment uses payload sizes of 24 (the default), 40, 56, 72, and 88 bytes with the Exp2 parameter-sensitivity setup.

For SapphMap runs, result lines report both the total `size` and the external B+Tree index `bptree_size`.

Note: When running experiments with `run_exp.sh`, the disk-I/O file is written to `/home/data/sapphmap/storage.bin` by default; to change the default directory, edit `/home/data/sapphmap` in the `DISK_IO_DATA_DIR` assignment near the top of `run_exp.sh`.

### 3.3 Examples

Run Exp1 scalability with dataset power 18:

```bash
./run_exp.sh Exp1 scalability 18 sapphmap
```

Run Exp1 workload with MetaKV:

```bash
./run_exp.sh Exp1 workload metakv sapphmap
```

Run Exp2 block-size sensitivity:

```bash
./run_exp.sh Exp2 blocksize
```

Run Exp2 insertion-search trade-off:

```bash
./run_exp.sh Exp2 tradeoff
```

Run Exp2 payload-size sensitivity:

```bash
./run_exp.sh Exp2 payload
```
