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
    pybind11-dev \
    python3-dev

# Install yalantinglibs
git clone https://github.com/alibaba/yalantinglibs.git
cd yalantinglibs
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
make -j$(nproc) && sudo make install
```

## Building Mooncake with EFA Support

### 1. Clone the Repository

```bash
git clone https://github.com/whn09/Mooncake.git
cd Mooncake
```

### 2. Build with EFA Enabled

```bash
mkdir build && cd build

cmake .. \
    -DUSE_EFA=ON \
    -DWITH_TE=ON \
    -DWITH_STORE=ON \
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
