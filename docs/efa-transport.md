# AWS EFA Transport for Mooncake

This document describes how to build and use Mooncake with AWS Elastic Fabric Adapter (EFA) support using libfabric.

## Prerequisites

### 1. AWS EFA Driver and libfabric

EFA driver and libfabric should be pre-installed on AWS instances with EFA support (e.g., p5e.48xlarge, p4d.24xlarge).

Verify installation:
```bash
# Check EFA devices
fi_info -p efa

# Verify libfabric location
ls /opt/amazon/efa/lib/libfabric.so
ls /opt/amazon/efa/include/rdma/fabric.h
```

If not installed, follow [AWS EFA documentation](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/efa-start.html).

### 2. Build Dependencies

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libgflags-dev \
    libgoogle-glog-dev \
    libjsoncpp-dev \
    libnuma-dev \
    libibverbs-dev \
    libboost-all-dev \
    libcurl4-openssl-dev \
    libyaml-cpp-dev \
    libgtest-dev \
    pybind11-dev \
    python3-dev

# Install yalantinglibs (required)
cd /tmp
git clone https://github.com/alibaba/yalantinglibs.git
cd yalantinglibs
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
make -j$(nproc)
sudo make install
```

## Building Mooncake with EFA Support

### 1. Clone the Repository

```bash
git clone https://github.com/whn09/Mooncake.git
cd Mooncake
git submodule update --init --recursive
```

### 2. Build with EFA Enabled

```bash
mkdir build && cd build

cmake .. \
    -DUSE_EFA=ON \
    -DWITH_TE=ON \
    -DWITH_STORE=ON \
    -DBUILD_UNIT_TESTS=ON \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

make -j$(nproc)
```

### 3. Install Python Package

```bash
# Copy built modules to wheel directory
cp mooncake-integration/engine.cpython-*.so ../mooncake-wheel/mooncake/
cp mooncake-asio/libasio.so ../mooncake-wheel/mooncake/

# Install with pip
pip install -e ../mooncake-wheel --no-build-isolation
```

## Verification

Test EFA transport initialization:

```python
from mooncake.engine import TransferEngine

te = TransferEngine()
result = te.initialize('127.0.0.1', 'P2PHANDSHAKE', 'efa', '')
print(f'Initialize result: {result}')  # Should be 0

# You should see logs like:
# EFA device (libfabric): rdmap79s0, domain: rdmap79s0-rdm, provider: efa
```

## Usage with vLLM

### Prefill Instance

```bash
VLLM_MOONCAKE_BOOTSTRAP_PORT=8998 \
vllm serve <model_path> -tp 8 \
   --port 8010 \
   --trust-remote-code \
   --kv-transfer-config '{"kv_connector":"MooncakeConnector","kv_role":"kv_producer","kv_connector_extra_config":{"mooncake_protocol":"efa"}}'
```

### Decode Instance

```bash
vllm serve <model_path> -tp 8 \
   --port 8020 \
   --trust-remote-code \
   --kv-transfer-config '{"kv_connector":"MooncakeConnector","kv_role":"kv_consumer","kv_connector_extra_config":{"mooncake_protocol":"efa"}}'
```

## Unit Tests

Run the EFA transport unit tests (requires EFA hardware):

```bash
./build/mooncake-transfer-engine/tests/efa_transport_test
```

The test suite includes:

| Test | Description |
|------|-------------|
| `InstallTransport` | Verify EFA transport installation |
| `LoopbackWrite` | Loopback write operation |
| `WriteAndRead` | Write then read with data integrity check |
| `MultiWrite` | Batch write (16 requests) |
| `StressMultipleBatches` | Stress test (20 batches x 8 requests) |

You can also run all unit tests via CTest:

```bash
cd build && ctest --output-on-failure
```

Environment variables for test configuration:

```bash
export MC_METADATA_SERVER=P2PHANDSHAKE     # default
export MC_LOCAL_SERVER_NAME=127.0.0.1:12345  # default
```

## Performance Benchmark

Use `transfer_engine_bench` to measure EFA transport throughput between two nodes.

### Target Node (receiver)

```bash
./build/mooncake-transfer-engine/example/transfer_engine_bench \
    --mode=target \
    --protocol=efa \
    --metadata_server=P2PHANDSHAKE
```

### Initiator Node (sender)

```bash
./build/mooncake-transfer-engine/example/transfer_engine_bench \
    --mode=initiator \
    --protocol=efa \
    --metadata_server=P2PHANDSHAKE \
    --segment_id=<target_hostname>:<target_port> \
    --operation=write \
    --duration=10 \
    --threads=8 \
    --block_size=65536 \
    --batch_size=128 \
    --buffer_size=1073741824 \
    --report_unit=GB
```

Replace `<target_hostname>:<target_port>` with the target node's address shown in the target's startup log (e.g., `172.31.29.226:12345`).

### Key Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--block_size` | 65536 | Bytes per transfer request |
| `--batch_size` | 128 | Requests per batch |
| `--threads` | 12 | Concurrent submission threads |
| `--buffer_size` | 1 GB | Total buffer size |
| `--duration` | 10 | Test duration in seconds |
| `--operation` | read | `read` or `write` |
| `--report_unit` | GB | `GB\|GiB\|Gb\|MB\|MiB\|Mb` |

### Tuning Tips

- Increase `--threads` to saturate multiple EFA devices
- Set `MC_SLICE_SIZE` environment variable (default: 65536) to control how transfers are sliced across devices — larger values reduce overhead, smaller values improve multi-NIC parallelism
- On p5e.48xlarge (8 EFA devices), try `--threads=16` with the default block/slice size

Output example:
```
Test completed: duration 10.00, batch count 1250, throughput 85.50 GB/s
```

## Technical Details

### Why libfabric instead of ibverbs?

AWS EFA exposes RDMA-like devices through the ibverbs interface, but does not support the full ibverbs API. Specifically:
- Queue Pair (QP) creation fails with "Operation not supported" (error 95)
- EFA requires using libfabric's `FI_EP_RDM` (Reliable Datagram Message) endpoint type

### EFA Transport Architecture

```
┌─────────────────────────────────────────────────────┐
│                   EfaTransport                       │
├─────────────────────────────────────────────────────┤
│  EfaContext (per device)                            │
│  ├── fi_info      (fabric info)                     │
│  ├── fid_fabric   (fabric handle)                   │
│  ├── fid_domain   (protection domain)               │
│  ├── fid_av       (address vector for peer lookup)  │
│  ├── fid_cq       (completion queues)               │
│  └── fid_mr       (memory regions)                  │
├─────────────────────────────────────────────────────┤
│  EfaEndpoint (per connection)                       │
│  ├── fid_ep       (RDM endpoint)                    │
│  ├── fi_addr_t    (peer address)                    │
│  └── local_addr   (local endpoint address)          │
└─────────────────────────────────────────────────────┘
```

### Supported AWS Instance Types

- p5e.48xlarge (16 EFA devices, rdmap* naming)
- p4d.24xlarge (4 EFA devices)
- Other EFA-enabled instances

## Troubleshooting

### No EFA devices found

```
EfaTransport: No EFA devices found
```

Solution: Verify EFA is available with `fi_info -p efa`

### Permission denied

```
fi_fabric failed: Permission denied
```

Solution: Ensure proper permissions or run with sudo for testing

### libfabric not found

```
cannot find -lfabric
```

Solution: Verify `/opt/amazon/efa/lib` is in the library path:
```bash
export LD_LIBRARY_PATH=/opt/amazon/efa/lib:$LD_LIBRARY_PATH
```
